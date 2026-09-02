#ifndef ROBU_PMM_H
#define ROBU_PMM_H
#include "robu/types.h"
#include "robu/vm.h"
#define PMM_NUM_COLORS 8
#define PMM_COLOR_ANY  (-1)
#define ROBU_IDENTITY_MAP_LIMIT 0x40000000ULL
typedef struct {
    uint64_t total_frames;
    uint64_t free_frames;
    uint64_t free_by_color[PMM_NUM_COLORS];
    uint64_t alloc_calls;
    uint64_t free_calls;
} pmm_stats_t;
extern pmm_stats_t pmm_stats;
#define PMM_MAX_RESERVED_REGIONS 16
void pmm_init(paddr_t base, uint64_t len, const paddr_t *reserve_bases,
              const uint64_t *reserve_lens, int nreserved);
paddr_t pmm_alloc(int color);
void pmm_free(paddr_t frame);
#endif
