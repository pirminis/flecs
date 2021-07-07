#include "private_api.h"

#ifdef FLECS_SYSTEMS_H
#include "modules/system/system.h"
#endif

static
void activate_table(
    ecs_world_t *world,
    ecs_query_t *query,
    ecs_table_t *table,
    bool active);

static
int32_t rank_by_depth(
    ecs_world_t *world,
    ecs_entity_t rank_by_component,
    ecs_type_t type)
{
    int32_t result = 0;
    int32_t i, count = ecs_vector_count(type);
    ecs_entity_t *array = ecs_vector_first(type, ecs_entity_t);

    for (i = count - 1; i >= 0; i --) {
        if (ECS_HAS_RELATION(array[i], EcsChildOf)) {
            ecs_type_t c_type = ecs_get_type(world, ecs_pair_object(world, array[i]));
            int32_t j, c_count = ecs_vector_count(c_type);
            ecs_entity_t *c_array = ecs_vector_first(c_type, ecs_entity_t);

            for (j = 0; j < c_count; j ++) {
                if (c_array[j] == rank_by_component) {
                    result ++;
                    result += rank_by_depth(world, rank_by_component, c_type);
                    break;
                }
            }

            if (j != c_count) {
                break;
            }
        } else if (!(array[i] & ECS_ROLE_MASK)) {
            /* No more parents after this */
            break;
        }
    }

    return result;
}

static
void group_table(
    ecs_world_t *world,
    ecs_query_t *query,
    ecs_cached_table_t *table)
{
    ecs_assert(table != NULL, ECS_INTERNAL_ERROR, NULL);

    if (query->group_table) {
        ecs_assert(table->table != NULL, ECS_INTERNAL_ERROR, NULL);
        table->rank = query->group_table(
            world, query->rank_on_component, table->table->type);
    } else {
        table->rank = 0;
    }
}

/* Rank all tables of query. Only necessary if a new ranking function was
 * provided or if a monitored entity set the component used for ranking. */
static
void group_tables(
    ecs_world_t *world,
    ecs_query_t *query)
{
    if (query->group_table) {
        ecs_vector_each(query->tables, ecs_cached_table_t, table, {
            group_table(world, query, table);
        });

        ecs_vector_each(query->empty_tables, ecs_cached_table_t, table, {
            group_table(world, query, table);
        });              
    }
}

#ifndef NDEBUG

static
const char* query_name(
    ecs_world_t *world,
    ecs_query_t *q)
{
    if (q->system) {
        return ecs_get_name(world, q->system);
    } else if (q->filter.name) {
        return q->filter.name;
    } else {
        return q->filter.expr;
    }
}

#endif

static
void cached_type_copy(
    ecs_cached_type_t *dst,
    const ecs_cached_type_t *src,
    int32_t term_count)
{
    dst->ids = ecs_os_memdup(src->ids, term_count * sizeof(ecs_id_t));
    dst->subjects = ecs_os_memdup(src->subjects, term_count * sizeof(ecs_entity_t));
    dst->sizes = ecs_os_memdup(src->sizes, term_count * sizeof(ecs_size_t));
    dst->types = ecs_os_memdup(src->types, term_count * sizeof(ecs_type_t));
    dst->type_map = ecs_os_memdup(src->type_map, term_count * sizeof(int32_t));
}

static
void cached_type_free(
    ecs_cached_type_t *ptr)
{
    ecs_os_free(ptr->ids);
    ecs_os_free(ptr->subjects);
    ecs_os_free(ptr->sizes);
    ecs_os_free(ptr->types);
    ecs_os_free(ptr->type_map);
}

static
void cached_table_free(
    ecs_cached_table_t *table)
{
    cached_type_free(&table->type);
    ecs_os_free(table->references);
    ecs_os_free(table->sparse_columns);
    ecs_os_free(table->bitset_columns);
    ecs_os_free(table->monitor);
}

static
void insert_table(
    ecs_world_t *world,
    ecs_query_t *query,
    ecs_table_t *table,
    ecs_cached_type_t *cached_type)
{
    ecs_vector_t **tables;
    int32_t index;

    if (!table || !ecs_table_count(table)) {
        tables = &query->empty_tables; 
        index = -(ecs_vector_count(*tables) + 1);
    } else {
        tables = &query->tables;
        index = ecs_vector_count(*tables);
    }

    ecs_cached_table_t *cached_table = ecs_vector_add(
        tables, ecs_cached_table_t);

    cached_table->table = table;
    cached_type_copy(&cached_table->type, cached_type, 
        query->filter.term_count);

    if (table) {
        ecs_table_indices_t *ti = ecs_map_ensure(query->table_indices, 
            ecs_table_indices_t, table->id);

        ti->indices = ecs_os_realloc(ti->indices, 
            (1 + ti->count) * ECS_SIZEOF(int32_t));
        ti->indices[ti->count] = index;
        ti->count ++;
    }      
}

/* Match existing tables against query */
static
void match_tables(
    ecs_world_t *world,
    ecs_query_t *query)
{
    ecs_iter_t it = ecs_filter_iter(world, &query->filter);
    
    while (ecs_filter_next(&it)) {
        ecs_cached_type_t cached_type = { 
            .ids = it.ids, .subjects = it.subjects, 
            .sizes = (ecs_size_t*)it.sizes, .types = it.types, 
            .type_map = it.type_map 
        };

        insert_table(world, query, it.table, &cached_type);
    }
}

/* Match with query */
static
bool match_table(
    ecs_world_t *world,
    ecs_query_t *query,
    ecs_table_t *table)
{
    ecs_id_t ids[ECS_ITER_TERM_STORAGE_SIZE];
    ecs_entity_t subjects[ECS_ITER_TERM_STORAGE_SIZE];
    ecs_size_t sizes[ECS_ITER_TERM_STORAGE_SIZE];
    ecs_type_t types[ECS_ITER_TERM_STORAGE_SIZE];
    int32_t type_map[ECS_ITER_TERM_STORAGE_SIZE];

    ecs_cached_type_t cached_type = { .ids = ids, .subjects = subjects, 
        .sizes = sizes, .types = types, .type_map = type_map, };

    if (ecs_filter_populate_from_type(
        world, &query->filter, table ? table->type : NULL, &cached_type, true))
    {
        insert_table(world, query, table, &cached_type);
        return true;
    }

    return false;
}

static
void clear_tables(
    ecs_world_t *world,
    ecs_query_t *query)
{
    ecs_vector_each(query->empty_tables, ecs_cached_table_t, table, {
        cached_table_free(table);
    });

    ecs_vector_each(query->tables, ecs_cached_table_t, table, {
        cached_table_free(table);
    });  

    ecs_map_iter_t it = ecs_map_iter(query->table_indices);
    ecs_table_indices_t *ti;
    while ((ti = ecs_map_next(&it, ecs_table_indices_t, NULL))) {
        ecs_os_free(ti->indices);
    }
    
    ecs_map_clear(query->table_indices);
    ecs_vector_clear(query->tables);
    ecs_vector_clear(query->empty_tables);
    ecs_vector_clear(query->table_slices);     
}

#define ELEM(ptr, size, index) ECS_OFFSET(ptr, size * index)

static
int32_t qsort_partition(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_data_t *data,
    ecs_entity_t *entities,
    void *ptr,    
    int32_t elem_size,
    int32_t lo,
    int32_t hi,
    ecs_compare_action_t compare)
{
    int32_t p = (hi + lo) / 2;
    void *pivot = ELEM(ptr, elem_size, p);
    ecs_entity_t pivot_e = entities[p];
    int32_t i = lo - 1, j = hi + 1;
    void *el;    

repeat:
    {
        do {
            i ++;
            el = ELEM(ptr, elem_size, i);
        } while ( compare(entities[i], el, pivot_e, pivot) < 0);

        do {
            j --;
            el = ELEM(ptr, elem_size, j);
        } while ( compare(entities[j], el, pivot_e, pivot) > 0);

        if (i >= j) {
            return j;
        }

        ecs_table_swap(world, table, data, i, j);

        if (p == i) {
            pivot = ELEM(ptr, elem_size, j);
            pivot_e = entities[j];
        } else if (p == j) {
            pivot = ELEM(ptr, elem_size, i);
            pivot_e = entities[i];
        }

        goto repeat;
    }
}

static
void qsort_array(
    ecs_world_t *world,
    ecs_table_t *table,
    ecs_data_t *data,
    ecs_entity_t *entities,
    void *ptr,
    int32_t size,
    int32_t lo,
    int32_t hi,
    ecs_compare_action_t compare)
{   
    if ((hi - lo) < 1)  {
        return;
    }

    int32_t p = qsort_partition(
        world, table, data, entities, ptr, size, lo, hi, compare);

    qsort_array(world, table, data, entities, ptr, size, lo, p, compare);

    qsort_array(world, table, data, entities, ptr, size, p + 1, hi, compare); 
}

static
void sort_table(
    ecs_world_t *world,
    ecs_table_t *table,
    int32_t column_index,
    ecs_compare_action_t compare)
{
    ecs_data_t *data = ecs_table_get_data(table);
    if (!data || !data->entities) {
        /* Nothing to sort */
        return;
    }

    int32_t count = ecs_table_data_count(data);
    if (count < 2) {
        return;
    }

    ecs_entity_t *entities = ecs_vector_first(data->entities, ecs_entity_t);

    void *ptr = NULL;
    int32_t size = 0;
    if (column_index != -1) {
        ecs_column_t *column = &data->columns[column_index];
        size = column->size;
        ptr = ecs_vector_first_t(column->data, size, column->alignment);
    }

    qsort_array(world, table, data, entities, ptr, size, 0, count - 1, compare);
}

/* Helper struct for building sorted table ranges */
typedef struct sort_helper_t {
    ecs_cached_table_t *table;
    ecs_entity_t *entities;
    const void *ptr;
    int32_t row;
    int32_t elem_size;
    int32_t count;
    bool shared;
} sort_helper_t;

static
const void* ptr_from_helper(
    sort_helper_t *helper)
{
    ecs_assert(helper->row < helper->count, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(helper->elem_size >= 0, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(helper->row >= 0, ECS_INTERNAL_ERROR, NULL);
    if (helper->shared) {
        return helper->ptr;
    } else {
        return ELEM(helper->ptr, helper->elem_size, helper->row);
    }
}

static
ecs_entity_t e_from_helper(
    sort_helper_t *helper)
{
    if (helper->row < helper->count) {
        return helper->entities[helper->row];
    } else {
        return 0;
    }
}

static
void build_sorted_table_range(
    ecs_query_t *query,
    int32_t start,
    int32_t end)
{
    ecs_world_t *world = query->world;
    ecs_entity_t component = query->sort_on_component;
    ecs_compare_action_t compare = query->compare;

    /* Fetch data from all matched tables */
    ecs_cached_table_t *tables = ecs_vector_first(query->tables, ecs_cached_table_t);
    sort_helper_t *helper = ecs_os_malloc((end - start) * ECS_SIZEOF(sort_helper_t));

    int i, to_sort = 0;
    for (i = start; i < end; i ++) {
        ecs_cached_table_t *table_data = &tables[i];
        ecs_table_t *table = table_data->table;
        ecs_data_t *data = ecs_table_get_data(table);
        ecs_vector_t *entities;
        if (!data || !(entities = data->entities) || !ecs_table_count(table)) {
            continue;
        }

        int32_t index = ecs_type_index_of(table->type, component);
        if (index != -1) {
            ecs_column_t *column = &data->columns[index];
            int16_t size = column->size;
            int16_t align = column->alignment;
            helper[to_sort].ptr = ecs_vector_first_t(column->data, size, align);
            helper[to_sort].elem_size = size;
            helper[to_sort].shared = false;
        } else if (component) {
            /* Find component in prefab */
            ecs_entity_t base = ecs_find_entity_in_prefabs(
                world, 0, table->type, component, 0);
            
            /* If a base was not found, the query should not have allowed using
             * the component for sorting */
            ecs_assert(base != 0, ECS_INTERNAL_ERROR, NULL);

            const EcsComponent *cptr = ecs_get(world, component, EcsComponent);
            ecs_assert(cptr != NULL, ECS_INTERNAL_ERROR, NULL);

            helper[to_sort].ptr = ecs_get_id(world, base, component);
            helper[to_sort].elem_size = cptr->size;
            helper[to_sort].shared = true;
        } else {
            helper[to_sort].ptr = NULL;
            helper[to_sort].elem_size = 0;
            helper[to_sort].shared = false;
        }

        helper[to_sort].table = table_data;
        helper[to_sort].entities = ecs_vector_first(entities, ecs_entity_t);
        helper[to_sort].row = 0;
        helper[to_sort].count = ecs_table_count(table);
        to_sort ++;      
    }

    ecs_table_slice_t *cur = NULL;

    bool proceed;
    do {
        int32_t j, min = 0;
        proceed = true;

        ecs_entity_t e1;
        while (!(e1 = e_from_helper(&helper[min]))) {
            min ++;
            if (min == to_sort) {
                proceed = false;
                break;
            }
        }

        if (!proceed) {
            break;
        }

        for (j = min + 1; j < to_sort; j++) {
            ecs_entity_t e2 = e_from_helper(&helper[j]);
            if (!e2) {
                continue;
            }

            const void *ptr1 = ptr_from_helper(&helper[min]);
            const void *ptr2 = ptr_from_helper(&helper[j]);

            if (compare(e1, ptr1, e2, ptr2) > 0) {
                min = j;
                e1 = e_from_helper(&helper[min]);
            }
        }

        sort_helper_t *cur_helper = &helper[min];
        if (!cur || cur->table != cur_helper->table) {
            cur = ecs_vector_add(&query->table_slices, ecs_table_slice_t);
            ecs_assert(cur != NULL, ECS_INTERNAL_ERROR, NULL);
            cur->table = cur_helper->table;
            cur->start_row = cur_helper->row;
            cur->count = 1;
        } else {
            cur->count ++;
        }

        cur_helper->row ++;
    } while (proceed);

    ecs_os_free(helper);
}

static
void build_sorted_tables(
    ecs_query_t *query)
{
    /* Clean previous sorted tables */
    ecs_vector_free(query->table_slices);
    query->table_slices = NULL;

    int32_t i, count = ecs_vector_count(query->tables);
    ecs_cached_table_t *tables = ecs_vector_first(query->tables, ecs_cached_table_t);
    ecs_cached_table_t *table = NULL;

    int32_t start = 0, rank = 0;
    for (i = 0; i < count; i ++) {
        table = &tables[i];
        if (rank != table->rank) {
            if (start != i) {
                build_sorted_table_range(query, start, i);
                start = i;
            }
            rank = table->rank;
        }
    }

    if (start != i) {
        build_sorted_table_range(query, start, i);
    }
}

static
bool tables_dirty(
    ecs_query_t *query)
{
    int32_t i, count = ecs_vector_count(query->tables);
    ecs_cached_table_t *tables = ecs_vector_first(query->tables, 
        ecs_cached_table_t);
    bool is_dirty = false;

    for (i = 0; i < count; i ++) {
        ecs_cached_table_t *table_data = &tables[i];
        ecs_table_t *table = table_data->table;

        if (!table_data->monitor) {
            table_data->monitor = ecs_table_get_monitor(table);
            is_dirty = true;
        }

        int32_t *dirty_state = ecs_table_get_dirty_state(table);
        int32_t t, type_count = table->column_count;
        for (t = 0; t < type_count + 1; t ++) {
            is_dirty = is_dirty || (dirty_state[t] != table_data->monitor[t]);
        }
    }

    is_dirty = is_dirty || (query->match_count != query->prev_match_count);

    return is_dirty;
}

static
void tables_reset_dirty(
    ecs_query_t *query)
{
    query->prev_match_count = query->match_count;

    int32_t i, count = ecs_vector_count(query->tables);
    ecs_cached_table_t *tables = ecs_vector_first(
        query->tables, ecs_cached_table_t);

    for (i = 0; i < count; i ++) {
        ecs_cached_table_t *table_data = &tables[i];
        ecs_table_t *table = table_data->table;

        if (!table_data->monitor) {
            /* If one table doesn't have a monitor, none of the tables will have
             * a monitor, so early out. */
            return;
        }

        int32_t *dirty_state = ecs_table_get_dirty_state(table);
        int32_t t, type_count = table->column_count;
        for (t = 0; t < type_count + 1; t ++) {
            table_data->monitor[t] = dirty_state[t];
        }
    }
}

static
void sort_tables(
    ecs_world_t *world,
    ecs_query_t *query)
{
    ecs_compare_action_t compare = query->compare;
    if (!compare) {
        return;
    }
    
    ecs_entity_t sort_on_component = query->sort_on_component;

    /* Iterate over active tables. Don't bother with inactive tables, since
     * they're empty */
    int32_t i, count = ecs_vector_count(query->tables);
    ecs_cached_table_t *tables = ecs_vector_first(
        query->tables, ecs_cached_table_t);
    bool tables_sorted = false;

    for (i = 0; i < count; i ++) {
        ecs_cached_table_t *table_data = &tables[i];
        ecs_table_t *table = table_data->table;

        /* If no monitor had been created for the table yet, create it now */
        bool is_dirty = false;
        if (!table_data->monitor) {
            table_data->monitor = ecs_table_get_monitor(table);

            /* A new table is always dirty */
            is_dirty = true;
        }

        int32_t *dirty_state = ecs_table_get_dirty_state(table);

        is_dirty = is_dirty || (dirty_state[0] != table_data->monitor[0]);

        int32_t index = -1;
        if (sort_on_component) {
            /* Get index of sorted component. We only care if the component we're
            * sorting on has changed or if entities have been added / re(moved) */
            index = ecs_type_index_of(table->type, sort_on_component);
            if (index != -1) {
                ecs_assert(index < ecs_vector_count(table->type), ECS_INTERNAL_ERROR, NULL); 
                is_dirty = is_dirty || (dirty_state[index + 1] != table_data->monitor[index + 1]);
            } else {
                /* Table does not contain component which means the sorted
                 * component is shared. Table does not need to be sorted */
                continue;
            }
        }      
        
        /* Check both if entities have moved (element 0) or if the component
         * we're sorting on has changed (index + 1) */
        if (is_dirty) {
            /* Sort the table */
            sort_table(world, table, index, compare);
            tables_sorted = true;
        }
    }

    if (tables_sorted || query->match_count != query->prev_match_count) {
        build_sorted_tables(query);
        query->match_count ++; /* Increase version if tables changed */
    }
}

static
bool has_refs(
    ecs_query_t *query)
{
    ecs_term_t *terms = query->filter.terms;
    int32_t i, count = query->filter.term_count;

    for (i = 0; i < count; i ++) {
        ecs_term_t *term = &terms[i];
        ecs_term_id_t *subj = &term->args[0];

        if (term->oper == EcsNot && !subj->entity) {
            /* Special case: if oper kind is Not and the query contained a
             * shared expression, the expression is translated to FromEmpty to
             * prevent resolving the ref */
            return true;
        } else if (subj->entity && (subj->entity != EcsThis || subj->set.mask != EcsSelf)) {
            /* If entity is not this, or if it can be substituted by other
             * entities, the query can have references. */
            return true;
        } 
    }

    return false;
}

static
bool is_column_wildcard_pair(
    ecs_term_t *term)
{
    ecs_entity_t c = term->id;

    if (ECS_HAS_ROLE(c, PAIR)) {
        ecs_assert(ECS_PAIR_RELATION(c) != 0, ECS_INTERNAL_ERROR, NULL);
        ecs_entity_t rel = ECS_PAIR_RELATION(c);
        ecs_entity_t obj = ECS_PAIR_OBJECT(c);
        if (!obj || obj == EcsWildcard) {
            return true;
        }
        if (rel == EcsWildcard) {
            return true;
        }
    } else {
        return false;
    }

    return false;
}

static
bool has_pairs(
    ecs_query_t *query)
{
    ecs_term_t *terms = query->filter.terms;
    int32_t i, count = query->filter.term_count;

    for (i = 0; i < count; i ++) {
        if (is_column_wildcard_pair(&terms[i])) {
            return true;
        }
    }

    return false;    
}

static
void register_monitors(
    ecs_world_t *world,
    ecs_query_t *query)
{
    ecs_term_t *terms = query->filter.terms;
    int32_t i, count = query->filter.term_count;

    for (i = 0; i < count; i++) {
        ecs_term_t *term = &terms[i];
        ecs_term_id_t *subj = &term->args[0];

        /* If component is requested with EcsCascade register component as a
         * parent monitor. Parent monitors keep track of whether an entity moved
         * in the hierarchy, which potentially requires the query to reorder its
         * tables. 
         * Also register a regular component monitor for EcsCascade columns.
         * This ensures that when the component used in the EcsCascade column
         * is added or removed tables are updated accordingly*/
        if (subj->set.mask & EcsSuperSet && subj->set.mask & EcsCascade && 
            subj->set.relation != EcsIsA) 
        {
            if (term->oper != EcsOr) {
                if (term->args[0].set.relation != EcsIsA) {
                    ecs_monitor_register(
                        world, term->args[0].set.relation, term->id, query);
                }
                ecs_monitor_register(world, 0, term->id, query);
            }

        /* FromAny also requires registering a monitor, as FromAny columns can
         * be matched with prefabs. The only term kinds that do not require
         * registering a monitor are FromOwned and FromEmpty. */
        } else if ((subj->set.mask & EcsSuperSet) || (subj->entity != EcsThis)){
            if (term->oper != EcsOr) {
                ecs_monitor_register(world, 0, term->id, query);
            }
        }
    };
}

static
void process_signature(
    ecs_world_t *world,
    ecs_query_t *query)
{
    ecs_term_t *terms = query->filter.terms;
    int32_t i, count = query->filter.term_count;

    for (i = 0; i < count; i ++) {
        ecs_term_t *term = &terms[i];
        ecs_term_id_t *pred = &term->pred;
        ecs_term_id_t *subj = &term->args[0];
        ecs_term_id_t *obj = &term->args[1];
        ecs_oper_kind_t op = term->oper; 
        ecs_inout_kind_t inout = term->inout;

        (void)pred;
        (void)obj;

        /* Queries do not support variables */
        ecs_assert(pred->var != EcsVarIsVariable, 
            ECS_UNSUPPORTED, NULL);
        ecs_assert(subj->var != EcsVarIsVariable, 
            ECS_UNSUPPORTED, NULL);
        ecs_assert(obj->var != EcsVarIsVariable, 
            ECS_UNSUPPORTED, NULL);

        /* Queries do not support subset substitutions */
        ecs_assert(!(pred->set.mask & EcsSubSet), ECS_UNSUPPORTED, NULL);
        ecs_assert(!(subj->set.mask & EcsSubSet), ECS_UNSUPPORTED, NULL);
        ecs_assert(!(obj->set.mask & EcsSubSet), ECS_UNSUPPORTED, NULL);

        /* Superset/subset substitutions aren't supported for pred/obj */
        ecs_assert(pred->set.mask == EcsDefaultSet, ECS_UNSUPPORTED, NULL);
        ecs_assert(obj->set.mask == EcsDefaultSet, ECS_UNSUPPORTED, NULL);

        if (subj->set.mask == EcsDefaultSet) {
            subj->set.mask = EcsSelf;
        }

        /* If self is not included in set, always start from depth 1 */
        if (!subj->set.min_depth && !(subj->set.mask & EcsSelf)) {
            subj->set.min_depth = 1;
        }

        if (inout != EcsIn) {
            query->flags |= EcsQueryHasOutColumns;
        }

        if (op == EcsOptional) {
            query->flags |= EcsQueryHasOptional;
        }

        if (!(query->flags & EcsQueryMatchDisabled)) {
            if (op == EcsAnd || op == EcsOr || op == EcsOptional) {
                if (term->id == EcsDisabled) {
                    query->flags |= EcsQueryMatchDisabled;
                }
            }
        }

        if (!(query->flags & EcsQueryMatchPrefab)) {
            if (op == EcsAnd || op == EcsOr || op == EcsOptional) {
                if (term->id == EcsPrefab) {
                    query->flags |= EcsQueryMatchPrefab;
                }
            }
        }

        if (subj->entity == EcsThis) {
            query->flags |= EcsQueryNeedsTables;
        }

        if (subj->set.mask & EcsCascade && term->oper == EcsOptional) {
            query->cascade_by = i + 1;
            query->rank_on_component = term->id;
        }

        if (subj->entity && subj->entity != EcsThis && 
            subj->set.mask == EcsSelf) 
        {
            ecs_set_watch(world, term->args[0].entity);
        }
    }

    query->flags |= (ecs_flags32_t)(has_refs(query) * EcsQueryHasRefs);
    query->flags |= (ecs_flags32_t)(has_pairs(query) * EcsQueryHasTraits);

    if (!(query->flags & EcsQueryIsSubquery)) {
        register_monitors(world, query);
    }
}

/* Move table from empty to non-empty list, or vice versa */
static
int32_t move_table(
    ecs_query_t *query,
    ecs_table_t *table,
    int32_t index,
    ecs_vector_t **dst_array,
    ecs_vector_t *src_array,
    bool activate)
{
    (void)table;

    int32_t new_index = 0;
    int32_t last_src_index = ecs_vector_count(src_array) - 1;
    ecs_assert(last_src_index >= 0, ECS_INTERNAL_ERROR, NULL);

    ecs_cached_table_t *mt = ecs_vector_last(src_array, ecs_cached_table_t);
    
    /* The last table of the source array will be moved to the location of the
     * table to move, do some bookkeeping to keep things consistent. */
    if (last_src_index) {
        ecs_table_indices_t *ti = ecs_map_get(query->table_indices, 
            ecs_table_indices_t, mt->table->id);

        int i, count = ti->count;
        for (i = 0; i < count; i ++) {
            int32_t old_index = ti->indices[i];
            if (activate) {
                if (old_index >= 0) {
                    /* old_index should be negative if activate is true, since
                     * we're moving from the empty list to the non-empty list. 
                     * However, if the last table in the source array is also
                     * the table being moved, this can happen. */
                    ecs_assert(table == mt->table, 
                        ECS_INTERNAL_ERROR, NULL);
                    continue;
                }
                /* If activate is true, src = the empty list, and index should
                 * be negative. */
                old_index = old_index * -1 - 1; /* Normalize */
            }

            /* Ensure to update correct index, as there can be more than one */
            if (old_index == last_src_index) {
                if (activate) {
                    ti->indices[i] = index * -1 - 1;
                } else {
                    ti->indices[i] = index;
                }
                break;
            }
        }

        /* If the src array contains tables, there must be a table that will get
         * moved. */
        ecs_assert(i != count, ECS_INTERNAL_ERROR, NULL);
    } else {
        /* If last_src_index is 0, the table to move was the only table in the
         * src array, so no other administration needs to be updated. */
    }

    /* Actually move the table. Only move from src to dst if we have a
     * dst_array, otherwise just remove it from src. */
    if (dst_array) {
        new_index = ecs_vector_count(*dst_array);
        ecs_vector_move_index(dst_array, src_array, ecs_cached_table_t, index);

        /* Make sure table is where we expect it */
        mt = ecs_vector_last(*dst_array, ecs_cached_table_t);
        ecs_assert(mt->table == table, ECS_INTERNAL_ERROR, NULL);
        ecs_assert(ecs_vector_count(*dst_array) == (new_index + 1), 
            ECS_INTERNAL_ERROR, NULL);  
    } else {
        ecs_vector_remove(src_array, ecs_cached_table_t, index);
    }

    /* Ensure that src array has now one element less */
    ecs_assert(ecs_vector_count(src_array) == last_src_index, 
        ECS_INTERNAL_ERROR, NULL);

    /* Return new index for table */
    if (activate) {
        /* Table is now active, index is positive */
        return new_index;
    } else {
        /* Table is now inactive, index is negative */
        return new_index * -1 - 1;
    }
}

/** Table activation happens when a table was or becomes empty. Deactivated
 * tables are not considered by the system in the main loop. */
static
void activate_table(
    ecs_world_t *world,
    ecs_query_t *query,
    ecs_table_t *table,
    bool active)
{    
    ecs_vector_t *src_array, *dst_array;
    int32_t activated = 0;
    int32_t prev_dst_count = 0;
    (void)world;
    (void)prev_dst_count; /* Only used when built with systems module */

    if (active) {
        src_array = query->empty_tables;
        dst_array = query->tables;
        prev_dst_count = ecs_vector_count(dst_array);
    } else {
        src_array = query->tables;
        dst_array = query->empty_tables;
    }

    ecs_table_indices_t *ti = ecs_map_get(
        query->table_indices, ecs_table_indices_t, table->id);

    if (ti) {
        int32_t i, count = ti->count;
        for (i = 0; i < count; i ++) {
            int32_t index = ti->indices[i];

            if (index < 0) {
                if (!active) {
                    /* If table is already inactive, no need to move */
                    continue;
                }
                index = index * -1 - 1;
            } else {
                if (active) {
                    /* If table is already active, no need to move */
                    continue;
                }
            }

            ecs_cached_table_t *mt = ecs_vector_get(
                src_array, ecs_cached_table_t, index);
            ecs_assert(mt->table == table, ECS_INTERNAL_ERROR, NULL);
            (void)mt;
            
            activated ++;

            ti->indices[i] = move_table(
                query, table, index, &dst_array, src_array, active);
        }

        if (activated) {
            /* Activate system if registered with query */
#ifdef FLECS_SYSTEMS_H
            if (query->system) {
                int32_t dst_count = ecs_vector_count(dst_array);
                if (active) {
                    if (!prev_dst_count && dst_count) {
                        ecs_system_activate(world, query->system, true, NULL);
                    }
                } else if (ecs_vector_count(src_array) == 0) {
                    ecs_system_activate(world, query->system, false, NULL);
                }
            }
#endif
        }

        if (active)  {
            query->tables = dst_array;
        } else {
            query->empty_tables = dst_array;
        }
    }
    
    if (!activated) {
        /* Received an activate event for a table we're not matched with. This
         * can only happen if this is a subquery */
        ecs_assert((query->flags & EcsQueryIsSubquery) != 0, 
            ECS_INTERNAL_ERROR, NULL);
        return;
    }

    /* Signal query it needs to reorder tables. Doing this in place could slow
     * down scenario's where a large number of tables is matched with an ordered
     * query. Since each table would trigger the activate signal, there would be
     * as many sorts as added tables, vs. only one when ordering happens when an
     * iterator is obtained. */
    query->needs_reorder = true;
}

static
void add_subquery(
    ecs_world_t *world, 
    ecs_query_t *parent, 
    ecs_query_t *subquery) 
{
    ecs_query_t **elem = ecs_vector_add(&parent->subqueries, ecs_query_t*);
    *elem = subquery;

    /* Iterate matched tables, match them with subquery */
    ecs_cached_table_t *tables = ecs_vector_first(parent->tables, ecs_cached_table_t);
    int32_t i, count = ecs_vector_count(parent->tables);

    for (i = 0; i < count; i ++) {
        ecs_cached_table_t *table = &tables[i];
        match_table(world, subquery, table->table);
        activate_table(world, subquery, table->table, true);
    }

    /* Do the same for inactive tables */
    tables = ecs_vector_first(parent->empty_tables, ecs_cached_table_t);
    count = ecs_vector_count(parent->empty_tables);

    for (i = 0; i < count; i ++) {
        ecs_cached_table_t *table = &tables[i];
        match_table(world, subquery, table->table);
    }    
}

static
void notify_subqueries(
    ecs_world_t *world,
    ecs_query_t *query,
    ecs_query_event_t *event)
{
    if (query->subqueries) {
        ecs_query_t **queries = ecs_vector_first(query->subqueries, ecs_query_t*);
        int32_t i, count = ecs_vector_count(query->subqueries);

        ecs_query_event_t sub_event = *event;
        sub_event.parent_query = query;

        for (i = 0; i < count; i ++) {
            ecs_query_t *sub = queries[i];
            ecs_query_notify(world, sub, &sub_event);
        }
    }
}

/** Check if a table was matched with the system */
static
ecs_table_indices_t* get_table_indices(
    ecs_query_t *query,
    ecs_table_t *table)
{
    return ecs_map_get(query->table_indices, ecs_table_indices_t, table->id);
}

/* Remove table */
static
void remove_table(
    ecs_query_t *query,
    ecs_table_t *table,
    ecs_vector_t *tables,
    int32_t index,
    bool empty)
{
    ecs_cached_table_t *mt = ecs_vector_get(
        tables, ecs_cached_table_t, index);
    if (!mt) {
        /* Query was notified of a table it doesn't match with, this can only
         * happen if query is a subquery. */
        ecs_assert(query->flags & EcsQueryIsSubquery, ECS_INTERNAL_ERROR, NULL);
        return;
    }
    
    ecs_assert(mt->table == table, ECS_INTERNAL_ERROR, NULL);
    (void)table;

    /* Free table before moving, as the move will cause another table to occupy
     * the memory of mt */
    cached_table_free(mt);  
    move_table(query, mt->table, index, NULL, tables, empty);
}

static
void unmatch_table(
    ecs_query_t *query,
    ecs_table_t *table,
    ecs_table_indices_t *ti)
{
    if (!ti) {
        ti = get_table_indices(query, table);
        if (!ti) {
            return;
        }
    }

    int32_t i, count = ti->count;
    for (i = 0; i < count; i ++) {
        int32_t index = ti->indices[i];
        if (index < 0) {
            index = index * -1 - 1;
            remove_table(query, table, query->empty_tables, index, true);
        } else {
            remove_table(query, table, query->tables, index, false);
        }
    }

    ecs_os_free(ti->indices);
    ecs_map_remove(query->table_indices, table->id);
}



static
bool satisfy_constraints(
    ecs_world_t *world,
    const ecs_filter_t *filter)
{
    ecs_term_t *terms = filter->terms;
    int32_t i, count = filter->term_count;

    for (i = 0; i < count; i ++) {
        ecs_term_t *term = &terms[i];
        ecs_term_id_t *subj = &term->args[0];
        ecs_oper_kind_t oper = term->oper;

        if (subj->entity != EcsThis && subj->set.mask & EcsSelf) {
            ecs_type_t type = ecs_get_type(world, subj->entity);
            if (ecs_type_has_id(world, type, term->id)) {
                if (oper == EcsNot) {
                    return false;
                }
            } else {
                if (oper != EcsNot) {
                    return false;
                }
            }
        }
    }

    return true;
}

static
void remove_subquery(
    ecs_query_t *parent, 
    ecs_query_t *sub)
{
    ecs_assert(parent != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(sub != NULL, ECS_INTERNAL_ERROR, NULL);
    ecs_assert(parent->subqueries != NULL, ECS_INTERNAL_ERROR, NULL);

    int32_t i, count = ecs_vector_count(parent->subqueries);
    ecs_query_t **sq = ecs_vector_first(parent->subqueries, ecs_query_t*);

    for (i = 0; i < count; i ++) {
        if (sq[i] == sub) {
            break;
        }
    }

    ecs_vector_remove(parent->subqueries, ecs_query_t*, i);
}

/* -- Private API -- */

void ecs_query_notify(
    ecs_world_t *world,
    ecs_query_t *query,
    ecs_query_event_t *event)
{
    bool notify = true;

    switch(event->kind) {
    case EcsQueryTableMatch:
        /* Creation of new table */
        if (match_table(world, query, event->table)) {
            if (query->subqueries) {
                notify_subqueries(world, query, event);
            }
        }
        notify = false;
        break;
    case EcsQueryTableUnmatch:
        /* Deletion of table */
        unmatch_table(query, event->table, NULL);
        break;
    case EcsQueryTableRematch:
        /* Rematch tables of query */
        clear_tables(world, query);
        match_tables(world, query);
        break;        
    case EcsQueryTableEmpty:
        /* Table is empty, deactivate */
        activate_table(world, query, event->table, false);
        break;
    case EcsQueryTableNonEmpty:
        /* Table is non-empty, activate */
        activate_table(world, query, event->table, true);
        break;
    case EcsQueryOrphan:
        ecs_assert(query->flags & EcsQueryIsSubquery, ECS_INTERNAL_ERROR, NULL);
        query->flags |= EcsQueryIsOrphaned;
        query->parent = NULL;
        break;
    }

    if (notify) {
        notify_subqueries(world, query, event);
    }
}


/* -- Public API -- */

ecs_query_t* ecs_query_init(
    ecs_world_t *world,
    const ecs_query_desc_t *desc)
{
    ecs_filter_t f;
    if (ecs_filter_init(world, &f, &desc->filter)) {
        return NULL;
    }

    ecs_query_t *result = ecs_sparse_add(world->queries, ecs_query_t);
    result->world = world;
    result->filter = f;
    result->table_indices = ecs_map_new(ecs_table_indices_t, 0);
    result->tables = ecs_vector_new(ecs_cached_table_t, 0);
    result->empty_tables = ecs_vector_new(ecs_cached_table_t, 0);
    result->system = desc->system;
    result->prev_match_count = -1;
    result->id = ecs_sparse_last_id(world->queries);

    if (desc->parent != NULL) {
        result->flags |= EcsQueryIsSubquery;
    }

    /* If a system is specified, ensure that if there are any subjects in the
     * filter that refer to the system, the component is added */
    if (desc->system)  {
        int32_t t, term_count = result->filter.term_count;
        ecs_term_t *terms = result->filter.terms;

        for (t = 0; t < term_count; t ++) {
            ecs_term_t *term = &terms[t];
            if (term->args[0].entity == desc->system) {
                ecs_add_id(world, desc->system, term->id);
            }
        }        
    }

    process_signature(world, result);

    ecs_trace_2("query #[green]%s#[reset] created with expression #[red]%s", 
        query_name(world, result), result->filter.expr);

    ecs_log_push();

    if (!desc->parent) {
        if (result->flags & EcsQueryNeedsTables) {
            if (desc->system) {
                if (ecs_has_id(world, desc->system, EcsMonitor)) {
                    result->flags |= EcsQueryMonitor;
                }
                
                if (ecs_has_id(world, desc->system, EcsOnSet)) {
                    result->flags |= EcsQueryOnSet;
                }

                if (ecs_has_id(world, desc->system, EcsUnSet)) {
                    result->flags |= EcsQueryUnSet;
                }  
            }      

            match_tables(world, result);
        } else {
            /* Add stub table that resolves references (if any) so everything is
             * preprocessed when the query is evaluated. */
            match_table(world, result, NULL);
        }
    } else {
        add_subquery(world, desc->parent, result);
        result->parent = desc->parent;
    }

    result->constraints_satisfied = satisfy_constraints(world, &result->filter);

    if (result->cascade_by) {
        result->group_table = rank_by_depth;
    }

    if (desc->order_by) {
        ecs_query_order_by(world, result, desc->order_by_id, desc->order_by);
    }

    if (desc->group_by) {
        ecs_query_group_by(world, result, desc->group_by_id, desc->group_by);
    }

    ecs_log_pop();

    return result;
}

void ecs_query_fini(
    ecs_query_t *query)
{
    ecs_world_t *world = query->world;
    ecs_assert(world != NULL, ECS_INTERNAL_ERROR, NULL);

    if ((query->flags & EcsQueryIsSubquery) &&
        !(query->flags & EcsQueryIsOrphaned))
    {
        remove_subquery(query->parent, query);
    }

    notify_subqueries(world, query, &(ecs_query_event_t){
        .kind = EcsQueryOrphan
    });

    clear_tables(world, query);

    ecs_map_free(query->table_indices);
    ecs_vector_free(query->tables);
    ecs_vector_free(query->empty_tables);
    ecs_vector_free(query->table_slices); 

    ecs_vector_free(query->subqueries);
    ecs_filter_fini(&query->filter);
    
    /* Remove query from storage */
    ecs_sparse_remove(world->queries, query->id);
}

const ecs_filter_t* ecs_query_get_filter(
    ecs_query_t *query)
{
    return &query->filter;
}

/* Create query iterator */
ecs_iter_t ecs_query_iter_page(
    ecs_query_t *query,
    int32_t offset,
    int32_t limit)
{
    ecs_assert(query != NULL, ECS_INVALID_PARAMETER, NULL);
    ecs_assert(!(query->flags & EcsQueryIsOrphaned), ECS_INVALID_PARAMETER, NULL);

    ecs_world_t *world = query->world;

    sort_tables(world, query);

    if (!world->is_readonly && query->flags & EcsQueryHasRefs) {
        ecs_eval_component_monitors(world);
    }

    tables_reset_dirty(query);

    ecs_query_iter_t it = {
        .query = query,
        .page_iter = {
            .offset = offset,
            .limit = limit,
            .remaining = limit
        },
        .index = 0,
    };

    ecs_iter_t result = {
        .world = world,
        .term_count = query->filter.term_count_actual,
        .private.iter.query = it
    };

    result.columns = result.private.columns_storage;

    return result;
}

ecs_iter_t ecs_query_iter(
    ecs_query_t *query)
{
    return ecs_query_iter_page(query, 0, 0);
}

void ecs_query_set_iter(
    ecs_world_t *world,
    ecs_query_t *query,
    ecs_iter_t *it,
    int32_t table_index,
    int32_t row,
    int32_t count)
{
    (void)world;
    
    ecs_assert(query != NULL, ECS_INVALID_PARAMETER, NULL);
    ecs_assert(!(query->flags & EcsQueryIsOrphaned), ECS_INVALID_PARAMETER, NULL);
    
    ecs_cached_table_t *table_data = ecs_vector_get(
        query->tables, ecs_cached_table_t, table_index);
    ecs_assert(table_data != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_table_t *table = table_data->table;
    ecs_data_t *data = ecs_table_get_data(table);
    ecs_assert(data != NULL, ECS_INTERNAL_ERROR, NULL);

    ecs_entity_t *entity_buffer = ecs_vector_first(data->entities, ecs_entity_t);  
    it->entities = &entity_buffer[row];

    it->world = NULL;
    it->offset = row;
    it->count = count;
    it->term_count = query->filter.term_count;
    it->term_count_actual = query->filter.term_count_actual;
    it->terms = query->filter.terms;
    it->private.total_count = count;

    ecs_iter_init_from_cached_type(it, &table_data->type);

    ecs_assert(it->term_count_actual < ECS_ITER_TERM_STORAGE_SIZE, 
        ECS_UNSUPPORTED, NULL);
    it->columns = it->private.columns_storage;

    ecs_filter_populate_from_table(
        world, &query->filter, table, &table_data->type, it->columns);
}

static
int ecs_page_iter_next(
    ecs_page_iter_t *it,
    ecs_page_cursor_t *cur)
{
    int32_t offset = it->offset;
    int32_t limit = it->limit;
    if (!(offset || limit)) {
        return cur->count == 0;
    }

    int32_t count = cur->count;
    int32_t remaining = it->remaining;

    if (offset) {
        if (offset > count) {
            /* No entities to iterate in current table */
            it->offset -= count;
            return 1;
        } else {
            cur->first += offset;
            count = cur->count -= offset;
            it->offset = 0;
        }
    }

    if (remaining) {
        if (remaining > count) {
            it->remaining -= count;
        } else {
            count = cur->count = remaining;
            it->remaining = 0;
        }
    } else if (limit) {
        /* Limit hit: no more entities left to iterate */
        return -1;
    }

    return count == 0;
}

static
int find_smallest_column(
    ecs_table_t *table,
    ecs_cached_table_t *table_data,
    ecs_vector_t *sparse_columns)
{
    ecs_sparse_column_t *sparse_column_array = 
        ecs_vector_first(sparse_columns, ecs_sparse_column_t);
    int32_t i, count = ecs_vector_count(sparse_columns);
    int32_t min = INT_MAX, index = 0;

    for (i = 0; i < count; i ++) {
        /* The array with sparse queries for the matched table */
        ecs_sparse_column_t *sparse_column = &sparse_column_array[i];

        /* Pointer to the switch column struct of the table */
        ecs_sw_column_t *sc = sparse_column->sw_column;

        /* If the sparse column pointer hadn't been retrieved yet, do it now */
        if (!sc) {
            /* Get the table column index from the signature column index */
            int32_t table_column_index = table_data->type.type_map[
                sparse_column->signature_column_index];

            /* Translate the table column index to switch column index */
            table_column_index -= table->sw_column_offset;
            ecs_assert(table_column_index >= 1, ECS_INTERNAL_ERROR, NULL);

            /* Get the sparse column */
            ecs_data_t *data = ecs_table_get_data(table);
            sc = sparse_column->sw_column = 
                &data->sw_columns[table_column_index - 1];
        }

        /* Find the smallest column */
        ecs_switch_t *sw = sc->data;
        int32_t case_count = ecs_switch_case_count(sw, sparse_column->sw_case);
        if (case_count < min) {
            min = case_count;
            index = i + 1;
        }
    }

    return index;
}

static
int sparse_column_next(
    ecs_table_t *table,
    ecs_cached_table_t *matched_table,
    ecs_vector_t *sparse_columns,
    ecs_query_iter_t *iter,
    ecs_page_cursor_t *cur)
{
    bool first_iteration = false;
    int32_t sparse_smallest;

    if (!(sparse_smallest = iter->sparse_smallest)) {
        sparse_smallest = iter->sparse_smallest = find_smallest_column(
            table, matched_table, sparse_columns);
        first_iteration = true;
    }

    sparse_smallest -= 1;

    ecs_sparse_column_t *columns = ecs_vector_first(
        sparse_columns, ecs_sparse_column_t);
    ecs_sparse_column_t *column = &columns[sparse_smallest];
    ecs_switch_t *sw, *sw_smallest = column->sw_column->data;
    ecs_entity_t case_smallest = column->sw_case;

    /* Find next entity to iterate in sparse column */
    int32_t first;
    if (first_iteration) {
        first = ecs_switch_first(sw_smallest, case_smallest);
    } else {
        first = ecs_switch_next(sw_smallest, iter->sparse_first);
    }

    if (first == -1) {
        goto done;
    }    

    /* Check if entity matches with other sparse columns, if any */
    int32_t i, count = ecs_vector_count(sparse_columns);
    do {
        for (i = 0; i < count; i ++) {
            if (i == sparse_smallest) {
                /* Already validated this one */
                continue;
            }

            column = &columns[i];
            sw = column->sw_column->data;

            if (ecs_switch_get(sw, first) != column->sw_case) {
                first = ecs_switch_next(sw_smallest, first);
                if (first == -1) {
                    goto done;
                }
            }
        }
    } while (i != count);

    cur->first = iter->sparse_first = first;
    cur->count = 1;

    return 0;
done:
    /* Iterated all elements in the sparse list, we should move to the
     * next matched table. */
    iter->sparse_smallest = 0;
    iter->sparse_first = 0;

    return -1;
}

#define BS_MAX ((uint64_t)0xFFFFFFFFFFFFFFFF)

static
int bitset_column_next(
    ecs_table_t *table,
    ecs_vector_t *bitset_columns,
    ecs_query_iter_t *iter,
    ecs_page_cursor_t *cur)
{
    /* Precomputed single-bit test */
    static const uint64_t bitmask[64] = {
    (uint64_t)1 << 0, (uint64_t)1 << 1, (uint64_t)1 << 2, (uint64_t)1 << 3,
    (uint64_t)1 << 4, (uint64_t)1 << 5, (uint64_t)1 << 6, (uint64_t)1 << 7,
    (uint64_t)1 << 8, (uint64_t)1 << 9, (uint64_t)1 << 10, (uint64_t)1 << 11,
    (uint64_t)1 << 12, (uint64_t)1 << 13, (uint64_t)1 << 14, (uint64_t)1 << 15,
    (uint64_t)1 << 16, (uint64_t)1 << 17, (uint64_t)1 << 18, (uint64_t)1 << 19,
    (uint64_t)1 << 20, (uint64_t)1 << 21, (uint64_t)1 << 22, (uint64_t)1 << 23,
    (uint64_t)1 << 24, (uint64_t)1 << 25, (uint64_t)1 << 26, (uint64_t)1 << 27,  
    (uint64_t)1 << 28, (uint64_t)1 << 29, (uint64_t)1 << 30, (uint64_t)1 << 31,
    (uint64_t)1 << 32, (uint64_t)1 << 33, (uint64_t)1 << 34, (uint64_t)1 << 35,  
    (uint64_t)1 << 36, (uint64_t)1 << 37, (uint64_t)1 << 38, (uint64_t)1 << 39,
    (uint64_t)1 << 40, (uint64_t)1 << 41, (uint64_t)1 << 42, (uint64_t)1 << 43,
    (uint64_t)1 << 44, (uint64_t)1 << 45, (uint64_t)1 << 46, (uint64_t)1 << 47,  
    (uint64_t)1 << 48, (uint64_t)1 << 49, (uint64_t)1 << 50, (uint64_t)1 << 51,
    (uint64_t)1 << 52, (uint64_t)1 << 53, (uint64_t)1 << 54, (uint64_t)1 << 55,  
    (uint64_t)1 << 56, (uint64_t)1 << 57, (uint64_t)1 << 58, (uint64_t)1 << 59,
    (uint64_t)1 << 60, (uint64_t)1 << 61, (uint64_t)1 << 62, (uint64_t)1 << 63
    };

    /* Precomputed test to verify if remainder of block is set (or not) */
    static const uint64_t bitmask_remain[64] = {
    BS_MAX, BS_MAX - (BS_MAX >> 63), BS_MAX - (BS_MAX >> 62),
    BS_MAX - (BS_MAX >> 61), BS_MAX - (BS_MAX >> 60), BS_MAX - (BS_MAX >> 59),
    BS_MAX - (BS_MAX >> 58), BS_MAX - (BS_MAX >> 57), BS_MAX - (BS_MAX >> 56), 
    BS_MAX - (BS_MAX >> 55), BS_MAX - (BS_MAX >> 54), BS_MAX - (BS_MAX >> 53), 
    BS_MAX - (BS_MAX >> 52), BS_MAX - (BS_MAX >> 51), BS_MAX - (BS_MAX >> 50), 
    BS_MAX - (BS_MAX >> 49), BS_MAX - (BS_MAX >> 48), BS_MAX - (BS_MAX >> 47), 
    BS_MAX - (BS_MAX >> 46), BS_MAX - (BS_MAX >> 45), BS_MAX - (BS_MAX >> 44), 
    BS_MAX - (BS_MAX >> 43), BS_MAX - (BS_MAX >> 42), BS_MAX - (BS_MAX >> 41), 
    BS_MAX - (BS_MAX >> 40), BS_MAX - (BS_MAX >> 39), BS_MAX - (BS_MAX >> 38), 
    BS_MAX - (BS_MAX >> 37), BS_MAX - (BS_MAX >> 36), BS_MAX - (BS_MAX >> 35), 
    BS_MAX - (BS_MAX >> 34), BS_MAX - (BS_MAX >> 33), BS_MAX - (BS_MAX >> 32), 
    BS_MAX - (BS_MAX >> 31), BS_MAX - (BS_MAX >> 30), BS_MAX - (BS_MAX >> 29), 
    BS_MAX - (BS_MAX >> 28), BS_MAX - (BS_MAX >> 27), BS_MAX - (BS_MAX >> 26), 
    BS_MAX - (BS_MAX >> 25), BS_MAX - (BS_MAX >> 24), BS_MAX - (BS_MAX >> 23), 
    BS_MAX - (BS_MAX >> 22), BS_MAX - (BS_MAX >> 21), BS_MAX - (BS_MAX >> 20), 
    BS_MAX - (BS_MAX >> 19), BS_MAX - (BS_MAX >> 18), BS_MAX - (BS_MAX >> 17), 
    BS_MAX - (BS_MAX >> 16), BS_MAX - (BS_MAX >> 15), BS_MAX - (BS_MAX >> 14), 
    BS_MAX - (BS_MAX >> 13), BS_MAX - (BS_MAX >> 12), BS_MAX - (BS_MAX >> 11), 
    BS_MAX - (BS_MAX >> 10), BS_MAX - (BS_MAX >> 9), BS_MAX - (BS_MAX >> 8), 
    BS_MAX - (BS_MAX >> 7), BS_MAX - (BS_MAX >> 6), BS_MAX - (BS_MAX >> 5), 
    BS_MAX - (BS_MAX >> 4), BS_MAX - (BS_MAX >> 3), BS_MAX - (BS_MAX >> 2),
    BS_MAX - (BS_MAX >> 1)
    };

    int32_t i, count = ecs_vector_count(bitset_columns);
    ecs_bitset_column_t *columns = ecs_vector_first(
        bitset_columns, ecs_bitset_column_t);
    int32_t bs_offset = table->bs_column_offset;

    int32_t first = iter->bitset_first;
    int32_t last = 0;

    for (i = 0; i < count; i ++) {
        ecs_bitset_column_t *column = &columns[i];
        ecs_bs_column_t *bs_column = columns[i].bs_column;

        if (!bs_column) {
            ecs_data_t *data = table->data;
            int32_t index = column->column_index;
            ecs_assert((index - bs_offset >= 0), ECS_INTERNAL_ERROR, NULL);
            bs_column = &data->bs_columns[index - bs_offset];
            columns[i].bs_column = bs_column;
        }
        
        ecs_bitset_t *bs = &bs_column->data;
        int32_t bs_elem_count = bs->count;
        int32_t bs_block = first >> 6;
        int32_t bs_block_count = ((bs_elem_count - 1) >> 6) + 1;

        if (bs_block >= bs_block_count) {
            goto done;
        }

        uint64_t *data = bs->data;
        int32_t bs_start = first & 0x3F;

        /* Step 1: find the first non-empty block */
        uint64_t v = data[bs_block];
        uint64_t remain = bitmask_remain[bs_start];
        while (!(v & remain)) {
            /* If no elements are remaining, move to next block */
            if ((++bs_block) >= bs_block_count) {
                /* No non-empty blocks left */
                goto done;
            }

            bs_start = 0;
            remain = BS_MAX; /* Test the full block */
            v = data[bs_block];
        }

        /* Step 2: find the first non-empty element in the block */
        while (!(v & bitmask[bs_start])) {
            bs_start ++;

            /* Block was not empty, so bs_start must be smaller than 64 */
            ecs_assert(bs_start < 64, ECS_INTERNAL_ERROR, NULL);
        }
        
        /* Step 3: Find number of contiguous enabled elements after start */
        int32_t bs_end = bs_start, bs_block_end = bs_block;
        
        remain = bitmask_remain[bs_end];
        while ((v & remain) == remain) {
            bs_end = 0;
            bs_block_end ++;

            if (bs_block_end == bs_block_count) {
                break;
            }

            v = data[bs_block_end];
            remain = BS_MAX; /* Test the full block */
        }

        /* Step 4: find remainder of enabled elements in current block */
        if (bs_block_end != bs_block_count) {
            while ((v & bitmask[bs_end])) {
                bs_end ++;
            }
        }

        /* Block was not 100% occupied, so bs_start must be smaller than 64 */
        ecs_assert(bs_end < 64, ECS_INTERNAL_ERROR, NULL);

        /* Step 5: translate to element start/end and make sure that each column
         * range is a subset of the previous one. */
        first = bs_block * 64 + bs_start;
        int32_t cur_last = bs_block_end * 64 + bs_end;
        
        /* No enabled elements found in table */
        if (first == cur_last) {
            goto done;
        }

        /* If multiple bitsets are evaluated, make sure each subsequent range
         * is equal or a subset of the previous range */
        if (i) {
            /* If the first element of a subsequent bitset is larger than the
             * previous last value, start over. */
            if (first >= last) {
                i = -1;
                continue;
            }

            /* Make sure the last element of the range doesn't exceed the last
             * element of the previous range. */
            if (cur_last > last) {
                cur_last = last;
            }
        }
        
        last = cur_last;
        int32_t elem_count = last - first;

        /* Make sure last element doesn't exceed total number of elements in 
         * the table */
        if (elem_count > bs_elem_count) {
            elem_count = bs_elem_count;
        }
        
        cur->first = first;
        cur->count = elem_count;
        iter->bitset_first = first;
    }
    
    /* Keep track of last processed element for iteration */ 
    iter->bitset_first = last;

    return 0;
done:
    return -1;
}

static
void mark_columns_dirty(
    ecs_query_t *query,
    ecs_cached_table_t *table_data)
{
    ecs_table_t *table = table_data->table;

    if (table && table->dirty_state) {
        ecs_term_t *terms = query->filter.terms;
        int32_t c = 0, i, count = query->filter.term_count;
        for (i = 0; i < count; i ++) {
            ecs_term_t *term = &terms[i];
            ecs_term_id_t *subj = &term->args[0];
            if (term->inout != EcsIn && (term->inout != EcsInOutDefault || 
                (subj->entity == EcsThis && subj->set.mask == EcsSelf)))
            {
                int32_t table_column = table_data->type.type_map[c];
                if (table_column > 0) {
                    table->dirty_state[table_column] ++;
                }
            }

            if (terms[i].oper == EcsOr) {
                do {
                    i ++;
                } while ((i < count) && terms[i].oper == EcsOr);
            }

            c ++;
        }
    }
}

/* Return next table */
bool ecs_query_next(
    ecs_iter_t *it)
{
    ecs_assert(it != NULL, ECS_INVALID_PARAMETER, NULL);
    ecs_query_iter_t *iter = &it->private.iter.query;
    ecs_page_iter_t *piter = &iter->page_iter;
    ecs_query_t *query = iter->query;
    ecs_world_t *world = query->world;
    (void)world;

    ecs_assert(world->magic == ECS_WORLD_MAGIC, ECS_INTERNAL_ERROR, NULL);

    if (!query->constraints_satisfied) {
        return false;
    }

    ecs_table_slice_t *slice = ecs_vector_first(
        query->table_slices, ecs_table_slice_t);
    ecs_cached_table_t *tables = ecs_vector_first(
        query->tables, ecs_cached_table_t);

    ecs_assert(!slice || query->compare, ECS_INTERNAL_ERROR, NULL);
    
    ecs_page_cursor_t cur;
    int32_t table_count = ecs_vector_count(query->tables);
    int32_t prev_count = it->private.total_count;

    int i;
    for (i = iter->index; i < table_count; i ++) {
        ecs_cached_table_t *table_data = slice ? slice[i].table : &tables[i];
        ecs_cached_type_t *type_data = &table_data->type;
        ecs_table_t *table = table_data->table;

        iter->index = i + 1;
        
        if (table) {
            ecs_vector_t *bitset_columns = table_data->bitset_columns;
            ecs_vector_t *sparse_columns = table_data->sparse_columns;

            if (slice) {
                cur.first = slice[i].start_row;
                cur.count = slice[i].count;                
            } else {
                cur.first = 0;
                cur.count = ecs_table_count(table);
            }

            if (cur.count) {
                if (bitset_columns) {
            
                    if (bitset_column_next(table, bitset_columns, iter, 
                        &cur) == -1) 
                    {
                        /* No more enabled components for table */
                        continue; 
                    } else {
                        iter->index = i;
                    }
                }

                if (sparse_columns) {
                    if (sparse_column_next(table, table_data,
                        sparse_columns, iter, &cur) == -1)
                    {
                        /* No more elements in sparse column */
                        continue;    
                    } else {
                        iter->index = i;
                    }
                }

                int ret = ecs_page_iter_next(piter, &cur);
                if (ret < 0) {
                    return false;
                } else if (ret > 0) {
                    continue;
                }
            } else {
                continue;
            }

            ecs_filter_populate_from_table(
                world, &query->filter, table, type_data, it->columns);

            ecs_data_t *data = ecs_table_get_data(table);
            ecs_assert(data != NULL, ECS_INTERNAL_ERROR, NULL);

            ecs_entity_t *entity_buffer = ecs_vector_first(
                data->entities, ecs_entity_t); 

            it->entities = &entity_buffer[cur.first];
            it->offset = cur.first;
            it->count = cur.count;
            it->private.total_count = cur.count;
        }

        it->ids = type_data->ids;
        it->subjects = type_data->subjects;
        it->sizes = (size_t*)type_data->sizes;
        it->types = type_data->types;
        it->type_map = type_data->type_map;

        it->private.frame_offset += prev_count;

        if (query->flags & EcsQueryHasOutColumns) {
            if (table) {
                mark_columns_dirty(query, table_data);
            }
        }

        return true;
    }

    return false;
}

bool ecs_query_next_w_filter(
    ecs_iter_t *iter,
    const ecs_filter_t *filter)
{
    ecs_table_t *table;

    do {
        if (!ecs_query_next(iter)) {
            return false;
        }
        table = iter->table;
    } while (filter && !ecs_table_match_filter(iter->world, table, filter));
    
    return true;
}

bool ecs_query_next_worker(
    ecs_iter_t *it,
    int32_t current,
    int32_t total)
{
    int32_t per_worker, first, prev_offset = it->offset;

    do {
        if (!ecs_query_next(it)) {
            return false;
        }

        int32_t count = it->count;
        per_worker = count / total;
        first = per_worker * current;

        count -= per_worker * total;

        if (count) {
            if (current < count) {
                per_worker ++;
                first += current;
            } else {
                first += count;
            }
        }

        if (!per_worker && 
            !(it->private.iter.query.query->flags & EcsQueryNeedsTables)) 
        {
            if (current == 0) {
                return true;
            } else {
                return false;
            }
        }
    } while (!per_worker);

    it->private.frame_offset -= prev_offset;
    it->count = per_worker;
    it->offset += first;
    it->entities = &it->entities[first];
    it->private.frame_offset += first;

    return true;
}

void ecs_query_order_by(
    ecs_world_t *world,
    ecs_query_t *query,
    ecs_entity_t sort_component,
    ecs_compare_action_t compare)
{
    ecs_assert(query != NULL, ECS_INVALID_PARAMETER, NULL);
    ecs_assert(!(query->flags & EcsQueryIsOrphaned), ECS_INVALID_PARAMETER, NULL);    
    ecs_assert(query->flags & EcsQueryNeedsTables, ECS_INVALID_PARAMETER, NULL);

    query->sort_on_component = sort_component;
    query->compare = compare;

    ecs_vector_free(query->table_slices);
    query->table_slices = NULL;

    sort_tables(world, query);    

    if (!query->table_slices) {
        build_sorted_tables(query);
    }
}

void ecs_query_group_by(
    ecs_world_t *world,
    ecs_query_t *query,
    ecs_entity_t sort_component,
    ecs_rank_type_action_t group_table_action)
{
    ecs_assert(query != NULL, ECS_INVALID_PARAMETER, NULL);
    ecs_assert(!(query->flags & EcsQueryIsOrphaned), ECS_INVALID_PARAMETER, NULL);    
    ecs_assert(query->flags & EcsQueryNeedsTables, ECS_INVALID_PARAMETER, NULL);

    query->rank_on_component = sort_component;
    query->group_table = group_table_action;

    group_tables(world, query);

    build_sorted_tables(query);
}

bool ecs_query_changed(
    ecs_query_t *query)
{
    ecs_assert(query != NULL, ECS_INVALID_PARAMETER, NULL);
    ecs_assert(!(query->flags & EcsQueryIsOrphaned), ECS_INVALID_PARAMETER, NULL);
    return tables_dirty(query);
}

bool ecs_query_orphaned(
    ecs_query_t *query)
{
    return query->flags & EcsQueryIsOrphaned;
}

