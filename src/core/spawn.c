#include "robu/spawn.h"
#include "robu/sched.h"
#include "robu/tcb.h"
#include "robu/vm.h"
#include "robu/elf.h"
#include "robu/rootfs.h"
#include "robu/ipc.h"
#include "robu/arch.h"
#include "robu/pipe.h"
#include "robu/execreq.h"
void spawn_exit_current(int status) {
    tcb_t *cur = current_thread;
    cur->exit_status = status;
    tcb_t *parent = cur->parent_tid ? sched_get_tcb(cur->parent_tid) : NULL;
    if (parent && parent->state == THREAD_STATE_WAIT_CHILD &&
        (parent->wait_filter == 0 || parent->wait_filter == cur->tid)) {
        parent->uctx.rax = (uint64_t)IPC_ERR_NONE;
        parent->uctx.r8 = cur->tid;
        parent->uctx.r9 = (uint64_t)(int64_t)status;
        sched_wake(parent);
        sched_terminate_to(THREAD_STATE_DEAD);
        return;
    }
    if (parent) {
        sched_terminate_to(THREAD_STATE_ZOMBIE);
        return;
    }
    sched_terminate_to(THREAD_STATE_DEAD);
}
int spawn_wait_begin(tcb_t *caller, tid_t filter, tid_t *out_tid, int *out_status) {
    int has_live = 0;
    tcb_t *z = sched_find_zombie_child(caller->tid, filter, &has_live);
    if (z) {
        *out_tid = z->tid;
        *out_status = z->exit_status;
        sched_reap_zombie(z);
        return 1;
    }
    return has_live ? 0 : -1;
}
#define SPAWN_NAME_MAX 64
static uint8_t spawn_scratch[SPAWN_REQ_MAX_LEN];
static char spawn_strbuf[SPAWN_REQ_MAX_LEN];
static char *spawn_argv[SPAWN_MAX_ARGS + 1];
static char *spawn_envp[SPAWN_MAX_ARGS + 1];
static int validate_range(uint32_t off, uint32_t len, uint32_t total_len) {
    if (off > total_len || len > total_len - off) {
        return -1;
    }
    return 0;
}
static int copy_name(const robu_spawn_req_t *req, uint32_t total_len,
                     char *out, uint32_t out_cap) {
    if (validate_range(req->name_off, req->name_len, total_len) != 0) {
        return -1;
    }
    if (req->name_len == 0 || req->name_len + 1 > out_cap) {
        return -1;
    }
    memcpy(out, spawn_scratch + req->name_off, req->name_len);
    out[req->name_len] = '\0';
    char *base = out;
    for (char *p = out; *p; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    if (base != out) {
        uint32_t i = 0;
        while (base[i]) {
            out[i] = base[i];
            i++;
        }
        out[i] = '\0';
    }
    return 0;
}
static int copy_str_array(uint32_t table_off, uint32_t count, uint32_t total_len,
                          char *out_ptrs[], char *strbuf, uint32_t strbuf_cap,
                          uint32_t *strbuf_used) {
    if (count == 0) {
        out_ptrs[0] = NULL;
        return 0;
    }
    if (validate_range(table_off, count * (uint32_t)sizeof(robu_spawn_str_t), total_len) != 0) {
        return -1;
    }
    const robu_spawn_str_t *table = (const robu_spawn_str_t *)(spawn_scratch + table_off);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = table[i].off, len = table[i].len;
        if (validate_range(off, len, total_len) != 0) {
            return -1;
        }
        if ((uint64_t)*strbuf_used + len + 1 > strbuf_cap) {
            return -1;
        }
        char *dst = strbuf + *strbuf_used;
        memcpy(dst, spawn_scratch + off, len);
        dst[len] = '\0';
        out_ptrs[i] = dst;
        *strbuf_used += len + 1;
    }
    out_ptrs[count] = NULL;
    return 0;
}
static int submit_execreq(tcb_t *caller, const char *name, uint32_t strbuf_used,
                          uint32_t argc, uint32_t envc,
                          uint32_t nfds, const robu_spawn_fd_t *fds, int is_spawn_create) {
    execreq_params_t params = {0};
    params.is_spawn_create = is_spawn_create;
    uint32_t namelen = 0;
    while (name[namelen] && namelen + 1 < sizeof(params.name)) {
        params.name[namelen] = name[namelen];
        namelen++;
    }
    params.name[namelen] = '\0';
    memcpy(params.strbuf, spawn_strbuf, strbuf_used);
    params.strbuf_used = strbuf_used;
    params.argc = argc;
    for (uint32_t i = 0; i < argc; i++) {
        params.argv_off[i] = (uint32_t)((const char *)spawn_argv[i] - spawn_strbuf);
    }
    params.envc = envc;
    for (uint32_t i = 0; i < envc; i++) {
        params.envp_off[i] = (uint32_t)((const char *)spawn_envp[i] - spawn_strbuf);
    }
    params.nfds = nfds;
    if (nfds > 0) {
        memcpy(params.fds, fds, (size_t)nfds * sizeof(robu_spawn_fd_t));
    }
    params.prio = caller->prio;
    params.pager_tid = caller->pager_tid;
    params.requester_tid = caller->tid;
    uint32_t slot;
    if (execreq_submit(caller, &params, &slot) != 0) {
        return IPC_ERR_NOT_FOUND;
    }
    return IPC_ERR_BLOCKED;
}
int spawn_create(tcb_t *caller, vaddr_t req_va, uint64_t req_len, tid_t *out_tid) {
    if (caller->address_space == 0) {
        return IPC_ERR_NO_CAP;
    }
    if (req_len == 0 || req_len > SPAWN_REQ_MAX_LEN) {
        return IPC_ERR_NO_CAP;
    }
    if (vm_copy_from_user(caller->address_space, req_va, spawn_scratch, req_len) != 0) {
        return IPC_ERR_NO_CAP;
    }
    if (req_len < sizeof(robu_spawn_req_t)) {
        return IPC_ERR_NO_CAP;
    }
    const robu_spawn_req_t *req = (const robu_spawn_req_t *)spawn_scratch;
    if (req->magic != SPAWN_REQ_MAGIC || req->total_len < sizeof(*req) ||
        req->total_len > req_len) {
        return IPC_ERR_NO_CAP;
    }
    uint32_t total_len = req->total_len;
    if (req->argc > SPAWN_MAX_ARGS || req->envc > SPAWN_MAX_ARGS) {
        return IPC_ERR_NO_CAP;
    }
    if (req->nfds > SPAWN_FD_INFO_MAX) {
        return IPC_ERR_NO_CAP;
    }
    if (validate_range(req->fds_off, req->nfds * (uint32_t)sizeof(robu_spawn_fd_t),
                       total_len) != 0) {
        return IPC_ERR_NO_CAP;
    }
    char name[SPAWN_NAME_MAX];
    if (copy_name(req, total_len, name, sizeof(name)) != 0) {
        return IPC_ERR_NO_CAP;
    }
    uint32_t strbuf_used = 0;
    if (copy_str_array(req->argv_off, req->argc, total_len, spawn_argv,
                       spawn_strbuf, sizeof(spawn_strbuf), &strbuf_used) != 0) {
        return IPC_ERR_NO_CAP;
    }
    if (copy_str_array(req->envp_off, req->envc, total_len, spawn_envp,
                       spawn_strbuf, sizeof(spawn_strbuf), &strbuf_used) != 0) {
        return IPC_ERR_NO_CAP;
    }
    robu_spawn_fd_t fds[SPAWN_FD_INFO_MAX];
    if (req->nfds > 0) {
        memcpy(fds, spawn_scratch + req->fds_off, (size_t)req->nfds * sizeof(robu_spawn_fd_t));
    }
    const uint8_t *elf_start, *elf_end;
    if (rootfs_lookup(name, &elf_start, &elf_end) != 0) {
        return submit_execreq(caller, name, strbuf_used, req->argc, req->envc,
                              req->nfds, fds, 1);
    }
    tcb_t *child = elf_load_and_spawn_req(name, elf_start, elf_end, caller->prio,
                                          caller->pager_tid,
                                          (int)req->argc, (const char *const *)spawn_argv,
                                          (int)req->envc, (const char *const *)spawn_envp,
                                          req->nfds, fds, 1);
    if (!child) {
        return IPC_ERR_NO_MEM;
    }
    child->parent_tid = caller->tid;
    *out_tid = child->tid;
    return IPC_ERR_NONE;
}

int spawn_exec(tcb_t *caller, vaddr_t req_va, uint64_t req_len) {
    if (caller->address_space == 0) {
        return IPC_ERR_NO_CAP;
    }
    if (req_len == 0 || req_len > SPAWN_REQ_MAX_LEN) {
        return IPC_ERR_NO_CAP;
    }
    if (vm_copy_from_user(caller->address_space, req_va, spawn_scratch, req_len) != 0) {
        return IPC_ERR_NO_CAP;
    }
    if (req_len < sizeof(robu_spawn_req_t)) {
        return IPC_ERR_NO_CAP;
    }
    const robu_spawn_req_t *req = (const robu_spawn_req_t *)spawn_scratch;
    if (req->magic != SPAWN_REQ_MAGIC || req->total_len < sizeof(*req) ||
        req->total_len > req_len) {
        return IPC_ERR_NO_CAP;
    }
    uint32_t total_len = req->total_len;
    if (req->argc > SPAWN_MAX_ARGS || req->envc > SPAWN_MAX_ARGS) {
        return IPC_ERR_NO_CAP;
    }
    if (req->nfds > SPAWN_FD_INFO_MAX) {
        return IPC_ERR_NO_CAP;
    }
    if (validate_range(req->fds_off, req->nfds * (uint32_t)sizeof(robu_spawn_fd_t),
                       total_len) != 0) {
        return IPC_ERR_NO_CAP;
    }
    char name[SPAWN_NAME_MAX];
    if (copy_name(req, total_len, name, sizeof(name)) != 0) {
        return IPC_ERR_NO_CAP;
    }
    uint32_t strbuf_used = 0;
    if (copy_str_array(req->argv_off, req->argc, total_len, spawn_argv,
                       spawn_strbuf, sizeof(spawn_strbuf), &strbuf_used) != 0) {
        return IPC_ERR_NO_CAP;
    }
    if (copy_str_array(req->envp_off, req->envc, total_len, spawn_envp,
                       spawn_strbuf, sizeof(spawn_strbuf), &strbuf_used) != 0) {
        return IPC_ERR_NO_CAP;
    }
    robu_spawn_fd_t fds[SPAWN_FD_INFO_MAX];
    if (req->nfds > 0) {
        memcpy(fds, spawn_scratch + req->fds_off, (size_t)req->nfds * sizeof(robu_spawn_fd_t));
    }
    const uint8_t *elf_start, *elf_end;
    if (rootfs_lookup(name, &elf_start, &elf_end) != 0) {
        return submit_execreq(caller, name, strbuf_used, req->argc, req->envc,
                              req->nfds, fds, 0);
    }
    if (elf_exec_current(caller, name, elf_start, elf_end,
                         (int)req->argc, (const char *const *)spawn_argv,
                         (int)req->envc, (const char *const *)spawn_envp,
                         req->nfds, fds, 1) != 0) {
        return IPC_ERR_NO_MEM;
    }
    return IPC_ERR_NONE;
}
int spawn_fork_create(tcb_t *caller, tid_t *out_tid) {
    if (caller->address_space == 0) {
        return IPC_ERR_NO_CAP;
    }
    paddr_t child_as = vm_address_space_clone(caller->address_space);
    if (!child_as) {
        return IPC_ERR_NO_MEM;
    }
    tcb_t *child = thread_create_forked_locked(caller->name, child_as, caller->pager_tid,
                                               caller->prio, caller);
    if (!child) {
        vm_address_space_destroy(child_as);
        return IPC_ERR_NO_MEM;
    }
    child->parent_tid = caller->tid;
    child->pgid = caller->pgid;
    child->sid = caller->sid;
    pipe_inherit_fork(caller->tid, child->tid);
    child->uctx.rax = (uint64_t)IPC_ERR_NONE;
    child->uctx.r8 = 0;
    *out_tid = child->tid;
    return IPC_ERR_NONE;
}
