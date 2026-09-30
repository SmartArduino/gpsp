#ifndef ESP32S31_JIT_H
#define ESP32S31_JIT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t sync_calls;
    uint64_t sync_ranges;
    uint64_t sync_bytes;
    uint64_t sync_time_us;
    uint64_t sync_max_us;
    unsigned dirty_fallbacks;
} gpsp_jit_cache_stats_t;

bool gpsp_jit_available(void);
void gpsp_jit_cache_sync(void *start, void *end);
void gpsp_jit_mark_dirty(void *start, void *end);
void gpsp_jit_publish_dirty(void);
void gpsp_jit_reset_cache_stats(void);
void gpsp_jit_get_cache_stats(gpsp_jit_cache_stats_t *stats);
void platform_cache_sync(void *start, void *end);

#endif
