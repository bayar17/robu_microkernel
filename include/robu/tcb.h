#ifndef ROBU_TCB_H
#define ROBU_TCB_H
#include "robu/types.h"
#include "robu/signal.h"
#include "context.h"
#include "percpu.h"
#define SYS_INFO_CAT_SETPGID   11
#define SYS_INFO_CAT_GETPGID   12
#define SYS_INFO_CAT_SETSID    13
#define SYS_INFO_CAT_TCGETPGRP 14
#define SYS_INFO_CAT_TCSETPGRP 15
typedef enum {
    THREAD_STATE_RUNNING = 0,
    THREAD_STATE_READY,
    THREAD_STATE_WAIT_RECV,
    THREAD_STATE_WAIT_SEND,
    THREAD_STATE_WAIT_PAGEFAULT,
    THREAD_STATE_SLEEPING,
    THREAD_STATE_WAIT_NOTIFICATION,
    THREAD_STATE_DEAD,
    THREAD_STATE_ZOMBIE,
    THREAD_STATE_WAIT_CHILD,
    THREAD_STATE_WAIT_CONSOLE,
    THREAD_STATE_WAIT_EXECREQ
} thread_state_t;
typedef struct {
    uint64_t word[6];
} msg_regs_t;
typedef struct tcb {
    tid_t tid;
    thread_state_t state;
    uint8_t prio;
    uint8_t in_ready_q;
    int32_t timeslice_left;
    uint64_t wake_at_tick;
    const char *name;
    int32_t exit_status;
    arch_uctx_t uctx;
    uint8_t fpu_state[512] __attribute__((aligned(16)));
    uint64_t fs_base;
    paddr_t address_space;
    void *kstack_top;
    uint64_t kstack_size;
    tid_t ipc_partner;
    tid_t ipc_recv_after;
    uint32_t ipc_flags;
    msg_regs_t ipc_buffer;
    tid_t pager_tid;
    tid_t parent_tid;
    tid_t wait_filter;
    uint32_t execreq_slot;
    uint32_t last_cpu;
    tid_t pgid;
    tid_t sid;
    uint64_t sig_pending;
    uint64_t sig_mask;
    sig_action_t sig_actions[ROBU_NSIG];
    int in_sig_handler;
    arch_uctx_t saved_uctx_before_sig;
    uint64_t saved_sig_mask;
    struct tcb *rq_next;
    struct tcb *sq_next;
    struct tcb *inbox_next;
    struct tcb *send_q_head;
    struct tcb *send_q_tail;
} tcb_t;
#define current_thread (this_cpu()->current_thread)
#endif
