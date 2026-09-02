#include "robu/pmm.h"
#include "robu/arch.h"
#include "robu/kprintf.h"
#include "robu/spinlock.h"
static spinlock_t pmm_lock = SPINLOCK_INIT;
extern uint8_t _end[];
#define PAGE_SHIFT_4K_ 12
static paddr_t free_head[PMM_NUM_COLORS];
static int next_color;
pmm_stats_t pmm_stats;
static inline int color_of(paddr_t frame) {
    return (int)((frame >> PAGE_SHIFT_4K_) & (PMM_NUM_COLORS - 1));
}
void pmm_init(paddr_t base, uint64_t len, const paddr_t *reserve_bases,
              const uint64_t *reserve_lens, int nreserved) {
    for (int c = 0; c < PMM_NUM_COLORS; c++) {
        free_head[c] = 0;
    }
    next_color = 0;
    pmm_stats = (pmm_stats_t){0};
    paddr_t floor = (paddr_t)_end;
    floor = (floor + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
    if (floor < base) {
        floor = base;
    }
    paddr_t ceiling = base + len;
    if (ceiling > ROBU_IDENTITY_MAP_LIMIT) {
        ceiling = ROBU_IDENTITY_MAP_LIMIT;
    }
    if (floor >= ceiling) {
        kprintf("[pmm] WARNING: no usable RAM after excluding kernel image\n");
        return;
    }
    if (nreserved > PMM_MAX_RESERVED_REGIONS) {
        nreserved = PMM_MAX_RESERVED_REGIONS;
    }
    uint64_t count = 0;
    for (paddr_t f = floor; f < ceiling; f += PAGE_SIZE_4K) {
        int reserved = 0;
        for (int i = 0; i < nreserved; i++) {
            if (!reserve_lens[i]) {
                continue;
            }
            paddr_t reserve_end = reserve_bases[i] + reserve_lens[i];
            if (f + PAGE_SIZE_4K > reserve_bases[i] && f < reserve_end) {
                reserved = 1;
                break;
            }
        }
        if (reserved) {
            continue;
        }
        pmm_free(f);
        count++;
    }
    pmm_stats.total_frames = count;
    pmm_stats.free_calls = 0;
    kprintf("[pmm] %lu frames (%lu KiB) managed, range [0x%lx-0x%lx), %u cache colors\n",
            count, count * 4, floor, ceiling, PMM_NUM_COLORS);
}
paddr_t pmm_alloc(int color) {
    spin_lock(&pmm_lock);
    if (color == PMM_COLOR_ANY) {
        for (int i = 0; i < PMM_NUM_COLORS; i++) {
            int c = next_color;
            next_color = (next_color + 1) & (PMM_NUM_COLORS - 1);
            if (free_head[c]) {
                color = c;
                break;
            }
        }
        if (color == PMM_COLOR_ANY) {
            spin_unlock(&pmm_lock);
            return 0;
        }
    }
    paddr_t frame = free_head[color];
    if (!frame) {
        spin_unlock(&pmm_lock);
        return 0;
    }
    free_head[color] = *(paddr_t *)frame;
    uint64_t *z = (uint64_t *)frame;
    for (int i = 0; i < (int)(PAGE_SIZE_4K / sizeof(uint64_t)); i++) {
        z[i] = 0;
    }
    pmm_stats.free_frames--;
    pmm_stats.free_by_color[color]--;
    pmm_stats.alloc_calls++;
    spin_unlock(&pmm_lock);
    return frame;
}
void pmm_free(paddr_t frame) {
    spin_lock(&pmm_lock);
    int c = color_of(frame);
    *(paddr_t *)frame = free_head[c];
    free_head[c] = frame;
    pmm_stats.free_frames++;
    pmm_stats.free_by_color[c]++;
    pmm_stats.free_calls++;
    spin_unlock(&pmm_lock);
}
