#define LOG_LOCAL_LEVEL ESP_LOG_INFO

#include "common.h"
#include "esp32s31_jit.h"
#include "jit_dirty_ranges.h"
#include "memmap.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mmu_map.h"
#include "esp_timer.h"
#include "hal/mmu_types.h"
#include "sdkconfig.h"

EXT_RAM_BSS_ATTR u8 ewram[1024 * 256 * 2];
EXT_RAM_BSS_ATTR u8 vram[1024 * 96];
u8 iwram[1024 * 32 * 2];
u8 *memory_map_read[8 * 1024];
u16 oam_ram[512];
u16 palette_ram[512];
u16 palette_ram_converted[512];
u16 io_registers[512];
u32 tmemld[11][16];
u32 tmemst[4][16];
u32 thnjal[15 * 16];

static void *jit_data;
static void *jit_exec;
static bool jit_mapping_owned;
static const char *TAG = "gpsp-jit";
static gpsp_jit_dirty_ranges_t jit_dirty;
static gpsp_jit_cache_stats_t cache_stats;

bool gpsp_jit_available(void)
{
    return jit_exec != NULL;
}

static void sync_cache_range(uintptr_t start, uintptr_t end)
{
    size_t size = end - start;
    ESP_ERROR_CHECK(esp_cache_msync((void *)start, size,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA));
    ESP_ERROR_CHECK(esp_cache_msync((void *)start, size,
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST));
    ++cache_stats.sync_ranges;
    cache_stats.sync_bytes += size;
}

static void finish_cache_sync(int64_t start_us)
{
    __asm__ volatile("fence.i" ::: "memory");
    uint64_t elapsed_us = esp_timer_get_time() - start_us;
    ++cache_stats.sync_calls;
    cache_stats.sync_time_us += elapsed_us;
    if (elapsed_us > cache_stats.sync_max_us) cache_stats.sync_max_us = elapsed_us;
}

void gpsp_jit_cache_sync(void *start, void *end)
{
    if ((uintptr_t)end <= (uintptr_t)start) return;
    uintptr_t aligned_start = (uintptr_t)start & ~(uintptr_t)(GPSP_JIT_CACHE_LINE_SIZE - 1);
    uintptr_t aligned_end = ((uintptr_t)end + GPSP_JIT_CACHE_LINE_SIZE - 1) & ~(uintptr_t)(GPSP_JIT_CACHE_LINE_SIZE - 1);
    int64_t start_us = esp_timer_get_time();
    sync_cache_range(aligned_start, aligned_end);
    finish_cache_sync(start_us);
}

void gpsp_jit_mark_dirty(void *start, void *end)
{
    gpsp_jit_dirty_mark(&jit_dirty, (uintptr_t)start, (uintptr_t)end);
}

void gpsp_jit_publish_dirty(void)
{
    if (!jit_dirty.count) return;
    int64_t start_us = esp_timer_get_time();
    for (size_t index = 0; index < jit_dirty.count; ++index) {
        sync_cache_range(jit_dirty.ranges[index].start, jit_dirty.ranges[index].end);
    }
    finish_cache_sync(start_us);
    gpsp_jit_dirty_clear(&jit_dirty);
}

void gpsp_jit_reset_cache_stats(void)
{
    memset(&cache_stats, 0, sizeof(cache_stats));
    jit_dirty.fallbacks = 0;
}

void gpsp_jit_get_cache_stats(gpsp_jit_cache_stats_t *stats)
{
    *stats = cache_stats;
    stats->dirty_fallbacks = jit_dirty.fallbacks;
}

void *map_jit_block(unsigned size)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    if (jit_exec) return NULL;
    size_t page_size = CONFIG_MMU_PAGE_SIZE;
    size_t allocation_size = (size + page_size - 1) & ~(page_size - 1);
    jit_data = heap_caps_aligned_alloc(page_size, allocation_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jit_data) return NULL;
    esp_paddr_t physical_address;
    mmu_target_t physical_target;
    esp_err_t result = esp_mmu_vaddr_to_paddr(jit_data, &physical_address, &physical_target);
    if (result == ESP_OK && physical_target == MMU_TARGET_PSRAM0) {
        result = esp_mmu_map(physical_address, allocation_size, MMU_TARGET_PSRAM0,
                            MMU_MEM_CAP_EXEC,
                            ESP_MMU_MMAP_FLAG_PADDR_SHARED, &jit_exec);
        jit_mapping_owned = result == ESP_OK;
        if (result == ESP_ERR_INVALID_STATE && jit_exec) result = ESP_OK;
    } else if (result == ESP_OK) {
        result = ESP_ERR_INVALID_ARG;
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Executable mapping failed: %s", esp_err_to_name(result));
        free(jit_data);
        jit_data = jit_exec = NULL;
        return NULL;
    }
    if (!gpsp_jit_dirty_init(&jit_dirty, (uintptr_t)jit_exec, allocation_size)) {
        unmap_jit_block(jit_exec, size);
        return NULL;
    }
    gpsp_jit_reset_cache_stats();
    volatile u32 *probe = jit_exec;
    probe[0] = 0x02a00513;
    probe[1] = 0x00008067;
    gpsp_jit_cache_sync(jit_exec, (u8 *)jit_exec + 8);
    int (*probe_function)(void) = (int (*)(void))jit_exec;
    int first = probe_function();
    probe[0] = 0x02b00513;
    gpsp_jit_cache_sync(jit_exec, (u8 *)jit_exec + 8);
    int second = probe_function();
    if (first != 42 || second != 43) {
        ESP_LOGE(TAG, "Executable cache probe failed: %d/%d", first, second);
        unmap_jit_block(jit_exec, size);
        return NULL;
    }
    ESP_LOGI(TAG, "RV32 executable PSRAM verified: data=%p exec=%p bytes=%u probe=%d/%d",
             jit_data, jit_exec, (unsigned)allocation_size, first, second);
    return jit_exec;
}

void unmap_jit_block(void *buffer, unsigned size)
{
    (void)size;
    if (buffer != jit_exec) return;
    if (jit_mapping_owned) ESP_ERROR_CHECK(esp_mmu_unmap(jit_exec));
    free(jit_data);
    jit_data = jit_exec = NULL;
    jit_mapping_owned = false;
    memset(&jit_dirty, 0, sizeof(jit_dirty));
}
