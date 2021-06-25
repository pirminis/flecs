/**
 * @file map.h
 * @brief Set datastructure.
 *
 * Same as map, but with no payload.
 */

#ifndef FLECS_SET_H
#define FLECS_SET_H

#include "map.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ecs_set_new(elem_count)\
    _ecs_map_new(0, 0, elem_count)

#define ecs_set_ensure(set, key)\
    _ecs_map_ensure(set, 0, key)

#ifdef __cplusplus
}
#endif

#endif
