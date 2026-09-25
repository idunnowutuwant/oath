#include "oath/arena.h"
#include <stdlib.h>

OathArena oath_arena_create(size_t capacity) {
    OathArena a;
    a.memory = (uint8_t*)malloc(capacity);
    a.allocated = 0;
    a.capacity = capacity;
    return a;
}

void* oath_arena_alloc(OathArena* arena, size_t size) {
    size_t aligned = (size + 7) & ~7;
    if (OATH_UNLIKELY(arena->allocated + aligned > arena->capacity)) {
        return NULL;
    }
    void* ptr = &arena->memory[arena->allocated];
    arena->allocated += aligned;
    return ptr;
}

void oath_arena_reset(OathArena* arena) {
    arena->allocated = 0;
}

void oath_arena_destroy(OathArena* arena) {
    free(arena->memory);
    arena->memory = NULL;
    arena->allocated = 0;
    arena->capacity = 0;
}