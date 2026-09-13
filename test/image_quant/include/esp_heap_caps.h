#pragma once
#include <stddef.h>
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_SPIRAM 4
void *heap_caps_malloc(size_t size, unsigned caps);
static inline size_t heap_caps_get_free_size(unsigned caps)
{ (void)caps; return 1024 * 1024; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps)
{ return heap_caps_get_free_size(caps); }
