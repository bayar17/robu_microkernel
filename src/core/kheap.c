#include "robu/kheap.h"
#include "robu/spinlock.h"

typedef struct kheap_free_block {
    uint64_t size;
    struct kheap_free_block *next;
} kheap_free_block_t;

static uint8_t *heap_start;
static uint8_t *heap_end;
static kheap_free_block_t *free_list;
static spinlock_t heap_lock = SPINLOCK_INIT;

#define KHEAP_ALIGN 16
#define KHEAP_MIN_BLOCK (sizeof(kheap_free_block_t))

static uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

void kheap_init(paddr_t base, uint64_t size) {
    heap_start = (uint8_t *)base;
    heap_end = heap_start + size;
    free_list = (kheap_free_block_t *)heap_start;
    free_list->size = size;
    free_list->next = NULL;
}

void *kmalloc(uint64_t n) {
    if (n == 0) {
        return NULL;
    }
    uint64_t need = align_up(n + sizeof(uint64_t), KHEAP_ALIGN);
    spin_lock(&heap_lock);
    kheap_free_block_t **prev = &free_list;
    kheap_free_block_t *cur = free_list;
    while (cur) {
        if (cur->size >= need) {
            uint64_t remaining = cur->size - need;
            uint64_t take = need;
            if (remaining >= KHEAP_MIN_BLOCK + KHEAP_ALIGN) {
                kheap_free_block_t *rest = (kheap_free_block_t *)((uint8_t *)cur + need);
                rest->size = remaining;
                rest->next = cur->next;
                *prev = rest;
            } else {
                take = cur->size;
                *prev = cur->next;
            }
            uint64_t *hdr = (uint64_t *)cur;
            *hdr = take;
            spin_unlock(&heap_lock);
            return (void *)(hdr + 1);
        }
        prev = &cur->next;
        cur = cur->next;
    }
    spin_unlock(&heap_lock);
    return NULL;
}

void *kmalloc_zeroed(uint64_t n) {
    void *p = kmalloc(n);
    if (!p) {
        return NULL;
    }
    uint8_t *b = (uint8_t *)p;
    for (uint64_t i = 0; i < n; i++) {
        b[i] = 0;
    }
    return p;
}

void kfree(void *ptr) {
    if (!ptr || (uint8_t *)ptr < heap_start || (uint8_t *)ptr >= heap_end) {
        return;
    }
    spin_lock(&heap_lock);
    uint64_t *hdr = (uint64_t *)ptr - 1;
    uint64_t size = *hdr;
    kheap_free_block_t *blk = (kheap_free_block_t *)hdr;
    blk->size = size;
    blk->next = free_list;
    free_list = blk;
    spin_unlock(&heap_lock);
}
