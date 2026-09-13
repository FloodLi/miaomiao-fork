#pragma once
#include <stddef.h>
#include <stdbool.h>
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
void *heap_caps_malloc(size_t size, unsigned caps);
void *heap_caps_calloc(size_t count, size_t size, unsigned caps);
bool esp_ptr_dma_capable(const void *ptr);
