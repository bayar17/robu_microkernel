#include "robu/shm.h"
#include "robu/ipc.h"
#include "robu/vm.h"
#include "robu/pmm.h"
#include "robu/kheap.h"
#include "robu/sched.h"
#include "robu/arch.h"

#define MAX_SHM_SEGMENTS 32
#define MAX_SHM_ATTACHES 64
#define SHM_MAX_SEGMENT_PAGES 4096

typedef struct {
    int in_use;
    int key;
    uint16_t generation;
    uint64_t size;
    uint64_t npages;
    paddr_t *frames;
    uint32_t nattch;
    int marked_rmid;
    tid_t cpid;
    tid_t lpid;
    uint64_t atime, dtime, ctime;
    uint32_t mode_bits;
} shm_segment_t;

typedef struct {
    int in_use;
    int shmid;
    paddr_t address_space;
    uint64_t va;
    uint64_t npages;
} shm_attach_t;

static shm_segment_t segments[MAX_SHM_SEGMENTS];
static shm_attach_t attaches[MAX_SHM_ATTACHES];

static int make_shmid(int slot, uint16_t generation) {
    return (int)((uint32_t)slot | ((uint32_t)generation << 8));
}
static int shmid_slot(int shmid) {
    return shmid & 0xFF;
}
static uint16_t shmid_gen(int shmid) {
    return (uint16_t)((uint32_t)shmid >> 8);
}
static shm_segment_t *find_segment(int shmid) {
    int slot = shmid_slot(shmid);
    if (slot < 0 || slot >= MAX_SHM_SEGMENTS) {
        return NULL;
    }
    shm_segment_t *s = &segments[slot];
    if (!s->in_use || s->generation != shmid_gen(shmid)) {
        return NULL;
    }
    return s;
}
static void free_segment_frames(shm_segment_t *s) {
    for (uint64_t i = 0; i < s->npages; i++) {
        if (s->frames[i]) {
            pmm_free(s->frames[i]);
        }
    }
    kfree(s->frames);
    s->frames = NULL;
    s->in_use = 0;
    s->generation++;
}

int shm_get(tid_t caller, int key, uint64_t size, uint32_t shmflg, int *out_id) {
    if (key != 0) {
        for (int i = 0; i < MAX_SHM_SEGMENTS; i++) {
            if (segments[i].in_use && !segments[i].marked_rmid && segments[i].key == key) {
                if ((shmflg & SHM_IPC_CREAT) && (shmflg & SHM_IPC_EXCL)) {
                    return IPC_ERR_EXISTS;
                }
                if (size != 0 && size > segments[i].size) {
                    return IPC_ERR_INVALID;
                }
                *out_id = make_shmid(i, segments[i].generation);
                return IPC_ERR_NONE;
            }
        }
        if (!(shmflg & SHM_IPC_CREAT)) {
            return IPC_ERR_NOT_FOUND;
        }
    }
    if (size == 0) {
        return IPC_ERR_INVALID;
    }
    uint64_t npages = (size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
    if (npages > SHM_MAX_SEGMENT_PAGES) {
        return IPC_ERR_INVALID;
    }
    int slot = -1;
    for (int i = 0; i < MAX_SHM_SEGMENTS; i++) {
        if (!segments[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return IPC_ERR_NO_SPACE;
    }
    paddr_t *frames = (paddr_t *)kmalloc(npages * sizeof(paddr_t));
    if (!frames) {
        return IPC_ERR_NO_MEM;
    }
    for (uint64_t i = 0; i < npages; i++) {
        paddr_t f = pmm_alloc(PMM_COLOR_ANY);
        if (!f) {
            for (uint64_t j = 0; j < i; j++) {
                pmm_free(frames[j]);
            }
            kfree(frames);
            return IPC_ERR_NO_MEM;
        }
        memset((void *)f, 0, PAGE_SIZE_4K);
        frames[i] = f;
    }
    shm_segment_t *s = &segments[slot];
    s->in_use = 1;
    s->key = key;
    s->size = size;
    s->npages = npages;
    s->frames = frames;
    s->nattch = 0;
    s->marked_rmid = 0;
    s->cpid = caller;
    s->lpid = caller;
    s->atime = 0;
    s->dtime = 0;
    s->ctime = sched_now();
    s->mode_bits = shmflg & 0x1FF;
    *out_id = make_shmid(slot, s->generation);
    return IPC_ERR_NONE;
}

int shm_at(tid_t caller, paddr_t address_space, int shmid, uint32_t shmflg, uint64_t *out_va) {
    shm_segment_t *s = find_segment(shmid);
    if (!s) {
        return IPC_ERR_NOT_FOUND;
    }
    uint64_t va = SHM_USER_VA_BASE;
    for (int i = 0; i < MAX_SHM_ATTACHES; i++) {
        if (attaches[i].in_use && attaches[i].address_space == address_space) {
            uint64_t end = attaches[i].va + attaches[i].npages * PAGE_SIZE_4K;
            if (end > va) {
                va = end;
            }
        }
    }
    int slot = -1;
    for (int i = 0; i < MAX_SHM_ATTACHES; i++) {
        if (!attaches[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return IPC_ERR_NO_SPACE;
    }
    uint32_t prot = VM_PROT_READ | VM_PROT_USER;
    if (!(shmflg & SHM_FLAG_RDONLY)) {
        prot |= VM_PROT_WRITE;
    }
    for (uint64_t i = 0; i < s->npages; i++) {
        if (arch_vm_map_page(address_space, va + i * PAGE_SIZE_4K, s->frames[i], prot) != 0) {
            for (uint64_t j = 0; j < i; j++) {
                arch_vm_unmap_page(address_space, va + j * PAGE_SIZE_4K);
            }
            return IPC_ERR_NO_MEM;
        }
    }
    attaches[slot].in_use = 1;
    attaches[slot].shmid = shmid;
    attaches[slot].address_space = address_space;
    attaches[slot].va = va;
    attaches[slot].npages = s->npages;
    s->nattch++;
    s->lpid = caller;
    s->atime = sched_now();
    *out_va = va;
    return IPC_ERR_NONE;
}

int shm_dt(paddr_t address_space, uint64_t shmaddr) {
    for (int i = 0; i < MAX_SHM_ATTACHES; i++) {
        if (attaches[i].in_use && attaches[i].address_space == address_space &&
            attaches[i].va == shmaddr) {
            shm_segment_t *s = find_segment(attaches[i].shmid);
            uint64_t npages = attaches[i].npages;
            for (uint64_t j = 0; j < npages; j++) {
                arch_vm_unmap_page(address_space, shmaddr + j * PAGE_SIZE_4K);
                arch_tlb_shootdown(address_space, shmaddr + j * PAGE_SIZE_4K);
            }
            attaches[i].in_use = 0;
            if (s) {
                s->nattch--;
                s->dtime = sched_now();
                if (s->nattch == 0 && s->marked_rmid) {
                    free_segment_frames(s);
                }
            }
            return IPC_ERR_NONE;
        }
    }
    return IPC_ERR_NOT_FOUND;
}

int shm_ctl(int shmid, int cmd, uint64_t out_words[6]) {
    shm_segment_t *s = find_segment(shmid);
    if (!s) {
        return IPC_ERR_NOT_FOUND;
    }
    if (cmd == SHM_IPC_STAT) {
        out_words[0] = s->size;
        out_words[1] = s->nattch;
        out_words[2] = (uint64_t)(uint32_t)s->cpid | ((uint64_t)(uint32_t)s->lpid << 32);
        out_words[3] = s->mode_bits | ((uint64_t)s->marked_rmid << 16);
        out_words[4] = s->atime;
        out_words[5] = s->dtime;
        return IPC_ERR_NONE;
    }
    if (cmd == SHM_IPC_RMID) {
        s->marked_rmid = 1;
        if (s->nattch == 0) {
            free_segment_frames(s);
        }
        return IPC_ERR_NONE;
    }
    return IPC_ERR_INVALID;
}

int shm_copy_out(int shmid, void *dst, uint64_t max_len, uint64_t *out_len) {
    shm_segment_t *s = find_segment(shmid);
    if (!s) {
        return IPC_ERR_NOT_FOUND;
    }
    uint64_t total = s->size < max_len ? s->size : max_len;
    uint64_t copied = 0;
    uint8_t *out = (uint8_t *)dst;
    for (uint64_t i = 0; i < s->npages && copied < total; i++) {
        uint64_t chunk = total - copied;
        if (chunk > PAGE_SIZE_4K) {
            chunk = PAGE_SIZE_4K;
        }
        memcpy(out + copied, (const void *)s->frames[i], chunk);
        copied += chunk;
    }
    *out_len = copied;
    return IPC_ERR_NONE;
}
void shm_detach_all_for_process(paddr_t address_space) {
    for (int i = 0; i < MAX_SHM_ATTACHES; i++) {
        if (attaches[i].in_use && attaches[i].address_space == address_space) {
            shm_segment_t *s = find_segment(attaches[i].shmid);
            for (uint64_t j = 0; j < attaches[i].npages; j++) {
                arch_vm_unmap_page(address_space, attaches[i].va + j * PAGE_SIZE_4K);
            }
            attaches[i].in_use = 0;
            if (s) {
                s->nattch--;
                if (s->nattch == 0 && s->marked_rmid) {
                    free_segment_frames(s);
                }
            }
        }
    }
}
