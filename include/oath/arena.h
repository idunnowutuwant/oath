#ifndef OATH_ARENA_H
#define OATH_ARENA_H

#include "oath/common.h"

typedef struct {
    uint8_t* memory;
    size_t allocated;
    size_t capacity;
} OathArena;

OathArena oath_arena_create(size_t capacity);
void* oath_arena_alloc(OathArena* arena, size_t size);
void oath_arena_reset(OathArena* arena);
void oath_arena_destroy(OathArena* arena);

#endif