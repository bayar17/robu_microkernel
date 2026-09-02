#include "robu/dma.h"
static paddr_t dma_base;
static uint64_t dma_size;
static uint64_t dma_used;
void dma_region_init(paddr_t base, uint64_t size) {
    dma_base = base;
    dma_size = size;
    dma_used = 0;
}
paddr_t dma_region_alloc(uint64_t bytes) {
    uint64_t start = (dma_used + 4095) & ~(uint64_t)4095;
    uint64_t end = start + ((bytes + 4095) & ~(uint64_t)4095);
    if (end > dma_size) {
        return 0;
    }
    dma_used = end;
    return dma_base + start;
}
void dma_range(paddr_t *out_base, uint64_t *out_size) {
    *out_base = dma_base;
    *out_size = dma_size;
}
