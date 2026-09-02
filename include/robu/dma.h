#ifndef ROBU_DMA_H
#define ROBU_DMA_H
#include "robu/types.h"

void dma_region_init(paddr_t base, uint64_t size);

paddr_t dma_region_alloc(uint64_t bytes);
void dma_range(paddr_t *out_base, uint64_t *out_size);
#endif
