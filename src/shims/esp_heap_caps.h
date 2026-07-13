#ifndef JETSON_SHIM_ESP_HEAP_CAPS_H
#define JETSON_SHIM_ESP_HEAP_CAPS_H
/* heap_caps -> plain malloc on Linux. */
#include <cstdlib>
#include <cstdint>

#define MALLOC_CAP_INTERNAL  (1 << 0)
#define MALLOC_CAP_DMA       (1 << 1)
#define MALLOC_CAP_8BIT      (1 << 2)
#define MALLOC_CAP_SPIRAM    (1 << 4)
#define MALLOC_CAP_DEFAULT   MALLOC_CAP_8BIT

static inline void *heap_caps_malloc(size_t size, uint32_t /*caps*/) { return malloc(size); }
static inline void *heap_caps_calloc(size_t n, size_t sz, uint32_t /*caps*/) { return calloc(n, sz); }
static inline void *heap_caps_realloc(void *p, size_t sz, uint32_t /*caps*/) { return realloc(p, sz); }
static inline void  heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(uint32_t /*caps*/) { return (size_t)-1; }
static inline size_t heap_caps_get_minimum_free_size(uint32_t /*caps*/) { return (size_t)-1; }

static inline uint32_t esp_get_free_heap_size(void) { return (uint32_t)-1; }
static inline uint32_t esp_get_minimum_free_heap_size(void) { return (uint32_t)-1; }

#endif