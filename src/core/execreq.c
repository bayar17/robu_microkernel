#include "robu/execreq.h"
#include "robu/sched.h"
#include "robu/ipc.h"
#include "robu/kinfo.h"
#include "robu/shm.h"
#include "robu/elf.h"
#include "robu/vfs.h"
#include "robu/arch.h"
#include "robu/kprintf.h"

typedef enum {
    EXECREQ_EMPTY = 0,
    EXECREQ_QUEUED,
    EXECREQ_DELIVERED,
    EXECREQ_POLL_DONE,
} execreq_state_t;

typedef struct {
    execreq_state_t state;
    uint16_t generation;
    execreq_params_t params;
    tcb_t *poll_child;
} execreq_slot_t;

static execreq_slot_t g_slots[EXECREQ_MAX_SLOTS];
static uint8_t g_staging_buf[EXECREQ_STAGING_SIZE];

static int alloc_slot(void) {
    for (int i = 0; i < EXECREQ_MAX_SLOTS; i++) {
        if (g_slots[i].state == EXECREQ_EMPTY) {
            return i;
        }
    }
    return -1;
}

static void free_slot(int idx) {
    g_slots[idx].state = EXECREQ_EMPTY;
    g_slots[idx].generation++;
}

static void inject_request(tcb_t *ext2fs, int slot_idx, const execreq_params_t *params) {
    arch_uctx_t *f = &ext2fs->uctx;
    f->r8 = (uint64_t)VFS_OP_READ_BULK;
    f->r9 = (uint64_t)slot_idx;
    f->r10 = (uint64_t)g_slots[slot_idx].generation;
    uint64_t words[3] = {0, 0, 0};
    uint32_t namelen = 0;
    while (params->name[namelen] && namelen < 24) {
        namelen++;
    }
    memcpy(words, params->name, namelen < 24 ? namelen : 24);
    f->r12 = words[0];
    f->r13 = words[1];
    f->r14 = words[2];
    f->rsi = 0;
    f->rax = (uint64_t)IPC_ERR_NONE;
}

int execreq_submit(tcb_t *cur, const execreq_params_t *params, uint32_t *out_slot) {
    tid_t ext2fs_tid = (tid_t)kinfo_user()->ext2fs_tid;
    tcb_t *ext2fs = ext2fs_tid ? sched_get_tcb(ext2fs_tid) : NULL;
    if (!ext2fs) {
        return -1;
    }
    int slot_idx = alloc_slot();
    if (slot_idx < 0) {
        return -1;
    }
    g_slots[slot_idx].params = *params;
    g_slots[slot_idx].state = EXECREQ_QUEUED;
    cur->execreq_slot = (uint32_t)slot_idx;
    sched_block(THREAD_STATE_WAIT_EXECREQ);
    if (ext2fs->state == THREAD_STATE_WAIT_RECV &&
        (ext2fs->ipc_partner == IPC_TID_ANY || ext2fs->ipc_partner == 0)) {
        g_slots[slot_idx].state = EXECREQ_DELIVERED;
        inject_request(ext2fs, slot_idx, params);
        sched_direct_switch(ext2fs);
    }
    *out_slot = (uint32_t)slot_idx;
    return 0;
}

int execreq_submit_poll(const execreq_params_t *params, uint32_t *out_slot, uint16_t *out_generation) {
    tid_t ext2fs_tid = (tid_t)kinfo_user()->ext2fs_tid;
    tcb_t *ext2fs = ext2fs_tid ? sched_get_tcb(ext2fs_tid) : NULL;
    if (!ext2fs) {
        return -1;
    }
    int slot_idx = alloc_slot();
    if (slot_idx < 0) {
        return -1;
    }
    g_slots[slot_idx].params = *params;
    g_slots[slot_idx].params.requester_tid = 0;
    g_slots[slot_idx].poll_child = NULL;
    g_slots[slot_idx].state = EXECREQ_QUEUED;
    if (ext2fs->state == THREAD_STATE_WAIT_RECV &&
        (ext2fs->ipc_partner == IPC_TID_ANY || ext2fs->ipc_partner == 0)) {
        g_slots[slot_idx].state = EXECREQ_DELIVERED;
        inject_request(ext2fs, slot_idx, &g_slots[slot_idx].params);
        sched_wake(ext2fs);
    }
    *out_slot = (uint32_t)slot_idx;
    *out_generation = g_slots[slot_idx].generation;
    return 0;
}

int execreq_poll_ready(uint32_t slot, uint16_t generation) {
    if (slot >= EXECREQ_MAX_SLOTS || g_slots[slot].state == EXECREQ_EMPTY ||
        g_slots[slot].generation != generation) {
        return -1;
    }
    return g_slots[slot].state == EXECREQ_POLL_DONE ? 1 : 0;
}

tcb_t *execreq_finish_poll_spawn(uint32_t slot, uint16_t generation) {
    if (slot >= EXECREQ_MAX_SLOTS || g_slots[slot].generation != generation ||
        g_slots[slot].state != EXECREQ_POLL_DONE) {
        return NULL;
    }
    tcb_t *child = g_slots[slot].poll_child;
    free_slot((int)slot);
    return child;
}

int execreq_try_deliver_queued(tcb_t *ext2fs, uint32_t *out_slot, uint16_t *out_generation,
                                char *out_name, uint32_t out_name_max) {
    (void)ext2fs;
    for (int i = 0; i < EXECREQ_MAX_SLOTS; i++) {
        if (g_slots[i].state == EXECREQ_QUEUED) {
            g_slots[i].state = EXECREQ_DELIVERED;
            *out_slot = (uint32_t)i;
            *out_generation = g_slots[i].generation;
            uint32_t j = 0;
            while (g_slots[i].params.name[j] && j + 1 < out_name_max) {
                out_name[j] = g_slots[i].params.name[j];
                j++;
            }
            out_name[j] = '\0';
            return 1;
        }
    }
    return 0;
}

static void reconstruct_ptrs(const execreq_params_t *p, char *argv_ptrs[], char *envp_ptrs[]) {
    for (uint32_t i = 0; i < p->argc; i++) {
        argv_ptrs[i] = (char *)(p->strbuf + p->argv_off[i]);
    }
    argv_ptrs[p->argc] = NULL;
    for (uint32_t i = 0; i < p->envc; i++) {
        envp_ptrs[i] = (char *)(p->strbuf + p->envp_off[i]);
    }
    envp_ptrs[p->envc] = NULL;
}

static void fail_requester(tcb_t *requester) {
    requester->uctx.rax = (uint64_t)IPC_ERR_NOT_FOUND;
    requester->uctx.r8 = 0;
    sched_wake(requester);
}

void execreq_complete(uint32_t slot, uint16_t generation, int status, int shmid, uint64_t size) {
    if (slot >= EXECREQ_MAX_SLOTS || g_slots[slot].state != EXECREQ_DELIVERED ||
        g_slots[slot].generation != generation) {
        return;
    }
    execreq_params_t *params = &g_slots[slot].params;
    if (params->requester_tid == 0) {
        tcb_t *child = NULL;
        if (status != IPC_ERR_NONE) {
            kprintf("[boot] exec '%s': filesystem reply status=%d\n", params->name, status);
        } else {
            uint64_t copied = 0;
            int copy_rc = shm_copy_out(shmid, g_staging_buf, sizeof(g_staging_buf), &copied);
            if (copy_rc != IPC_ERR_NONE || copied != size || copied == 0) {
                kprintf("[boot] exec '%s': shared-memory copy rc=%d copied=%lu size=%lu\n",
                        params->name, copy_rc, copied, size);
            } else {
                char *argv_ptrs[SPAWN_MAX_ARGS + 1];
                char *envp_ptrs[SPAWN_MAX_ARGS + 1];
                reconstruct_ptrs(params, argv_ptrs, envp_ptrs);
                const uint8_t *elf_start = g_staging_buf;
                const uint8_t *elf_end = g_staging_buf + copied;
                child = elf_load_and_spawn_req(params->name, elf_start, elf_end, params->prio,
                                               params->pager_tid, (int)params->argc,
                                               (const char *const *)argv_ptrs,
                                               (int)params->envc, (const char *const *)envp_ptrs,
                                               params->nfds, params->fds, 0);
                if (!child) {
                    kprintf("[boot] exec '%s': ELF load failed\n", params->name);
                }
            }
        }
        g_slots[slot].poll_child = child;
        g_slots[slot].state = EXECREQ_POLL_DONE;
        return;
    }
    tcb_t *requester = sched_get_tcb(params->requester_tid);
    if (!requester || requester->state != THREAD_STATE_WAIT_EXECREQ ||
        requester->execreq_slot != slot) {
        free_slot((int)slot);
        return;
    }
    if (status != IPC_ERR_NONE) {
        fail_requester(requester);
        free_slot((int)slot);
        return;
    }
    uint64_t copied = 0;
    if (shm_copy_out(shmid, g_staging_buf, sizeof(g_staging_buf), &copied) != IPC_ERR_NONE ||
        copied != size || copied == 0) {
        fail_requester(requester);
        free_slot((int)slot);
        return;
    }
    char *argv_ptrs[SPAWN_MAX_ARGS + 1];
    char *envp_ptrs[SPAWN_MAX_ARGS + 1];
    reconstruct_ptrs(params, argv_ptrs, envp_ptrs);
    const uint8_t *elf_start = g_staging_buf;
    const uint8_t *elf_end = g_staging_buf + copied;
    if (params->is_spawn_create) {
        tcb_t *child = elf_load_and_spawn_req(params->name, elf_start, elf_end, params->prio,
                                              params->pager_tid, (int)params->argc,
                                              (const char *const *)argv_ptrs,
                                              (int)params->envc, (const char *const *)envp_ptrs,
                                              params->nfds, params->fds, 0);
        if (!child) {
            requester->uctx.rax = (uint64_t)IPC_ERR_NO_MEM;
            requester->uctx.r8 = 0;
        } else {
            child->parent_tid = requester->tid;
            requester->uctx.rax = (uint64_t)IPC_ERR_NONE;
            requester->uctx.r8 = child->tid;
        }
    } else {
        if (elf_exec_current(requester, params->name, elf_start, elf_end, (int)params->argc,
                             (const char *const *)argv_ptrs, (int)params->envc,
                             (const char *const *)envp_ptrs, params->nfds,
                             params->fds, 0) != 0) {
            requester->uctx.rax = (uint64_t)IPC_ERR_NO_MEM;
        }
    }
    sched_wake(requester);
    free_slot((int)slot);
}

void execreq_cancel_requester(tid_t tid) {
    for (int i = 0; i < EXECREQ_MAX_SLOTS; i++) {
        if (g_slots[i].state != EXECREQ_EMPTY && g_slots[i].params.requester_tid == tid) {
            free_slot(i);
        }
    }
}

void execreq_fail_all(void) {
    for (int i = 0; i < EXECREQ_MAX_SLOTS; i++) {
        if (g_slots[i].state == EXECREQ_EMPTY) {
            continue;
        }
        tcb_t *requester = sched_get_tcb(g_slots[i].params.requester_tid);
        if (requester && requester->state == THREAD_STATE_WAIT_EXECREQ &&
            requester->execreq_slot == (uint32_t)i) {
            fail_requester(requester);
        }
        free_slot(i);
    }
}
