#ifndef ROBU_KHEAP_H
#define ROBU_KHEAP_H
#include "robu/types.h"
void kheap_init(paddr_t base, uint64_t size);
void *kmalloc(uint64_t n);
void *kmalloc_zeroed(uint64_t n);
void kfree(void *ptr);
#endif
