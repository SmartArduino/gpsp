#ifndef GPSP_JIT_DIRTY_RANGES_H
#define GPSP_JIT_DIRTY_RANGES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define GPSP_JIT_CACHE_LINE_SIZE 64
#define GPSP_JIT_DIRTY_MAX_RANGES 32

typedef struct {
    uintptr_t start;
    uintptr_t end;
} gpsp_jit_dirty_range_t;

typedef struct {
    uintptr_t base;
    uintptr_t limit;
    size_t count;
    unsigned fallbacks;
    gpsp_jit_dirty_range_t ranges[GPSP_JIT_DIRTY_MAX_RANGES];
} gpsp_jit_dirty_ranges_t;

static inline bool gpsp_jit_dirty_init(gpsp_jit_dirty_ranges_t *dirty,
                                      uintptr_t base, size_t size)
{
    memset(dirty, 0, sizeof(*dirty));
    if (!size || (base & (GPSP_JIT_CACHE_LINE_SIZE - 1)) ||
        (size & (GPSP_JIT_CACHE_LINE_SIZE - 1)) || size > UINTPTR_MAX - base) {
        return false;
    }
    dirty->base = base;
    dirty->limit = base + size;
    return true;
}

static inline void gpsp_jit_dirty_mark(gpsp_jit_dirty_ranges_t *dirty,
                                      uintptr_t start, uintptr_t end)
{
    if (start >= end || start >= dirty->limit || end <= dirty->base) return;
    if (start < dirty->base) start = dirty->base;
    if (end > dirty->limit) end = dirty->limit;
    start &= ~(uintptr_t)(GPSP_JIT_CACHE_LINE_SIZE - 1);
    end = (end + GPSP_JIT_CACHE_LINE_SIZE - 1) & ~(uintptr_t)(GPSP_JIT_CACHE_LINE_SIZE - 1);

    size_t first = 0;
    while (first < dirty->count && dirty->ranges[first].end < start) ++first;
    size_t last = first;
    while (last < dirty->count && dirty->ranges[last].start <= end) {
        if (dirty->ranges[last].start < start) start = dirty->ranges[last].start;
        if (dirty->ranges[last].end > end) end = dirty->ranges[last].end;
        ++last;
    }

    if (first == last && dirty->count == GPSP_JIT_DIRTY_MAX_RANGES) {
        if (dirty->ranges[0].start < start) start = dirty->ranges[0].start;
        if (dirty->ranges[dirty->count - 1].end > end) end = dirty->ranges[dirty->count - 1].end;
        dirty->ranges[0] = (gpsp_jit_dirty_range_t){start, end};
        dirty->count = 1;
        ++dirty->fallbacks;
        return;
    }

    memmove(&dirty->ranges[first + 1], &dirty->ranges[last],
            (dirty->count - last) * sizeof(dirty->ranges[0]));
    dirty->ranges[first] = (gpsp_jit_dirty_range_t){start, end};
    dirty->count = dirty->count - (last - first) + 1;
}

static inline void gpsp_jit_dirty_clear(gpsp_jit_dirty_ranges_t *dirty)
{
    dirty->count = 0;
}

#endif
