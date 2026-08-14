#include "robu/sched.h"
#include "robu/arch.h"
#include "robu/kprintf.h"
#include "robu/ipc.h"
#include "robu/spinlock.h"
#include "robu/kinfo.h"
#include "robu/vm.h"
#include "robu/captable.h"
#include "robu/trace.h"
#include "robu/pipe.h"
#include "robu/signal.h"
#include "robu/shm.h"
#include "robu/socket.h"
#include "percpu.h"
extern volatile uint32_t cpu_online[MAX_CPUS];
sched_stats_t sched_stats;
static spinlock_t kernel_lock = SPINLOCK_INIT;
void sched_lock_acquire(void) {
    while (!spin_trylock(&kernel_lock)) {
        arch_tlb_shootdown_handle_local();
        asm volatile("pause");
    }
}
void sched_lock_release(void) { spin_unlock(&kernel_lock); }
static tcb_t thread_table[SCHED_MAX_THREADS];
static tcb_t idle_thread[MAX_CPUS];
static uint8_t idle_stack[MAX_CPUS][2048] __attribute__((aligned(16)));
static tcb_t *ready_head[SCHED_PRIO_LEVELS];
static tcb_t *ready_tail[SCHED_PRIO_LEVELS];
static uint32_t ready_bitmap;
static tcb_t *wake_inbox;
static int need_resched[MAX_CPUS];
static uint64_t now_ticks;
static uint64_t armed_ticks[MAX_CPUS];
static uint64_t next_deadline = (uint64_t)-1;
void sched_note_deadline(uint64_t tick) {
    if (tick < next_deadline) {
        next_deadline = tick;
    }
}
#define SCHED_MAX_ARM_TICKS 500
static void sched_arm_next_deadline(void);
static void rq_push(tcb_t *t) {
    if (t->in_ready_q) {
        return;
    }
    t->in_ready_q = 1;
    t->rq_next = NULL;
    uint8_t p = t->prio;
    if (ready_tail[p]) {
        ready_tail[p]->rq_next = t;
    } else {
        ready_head[p] = t;
    }
    ready_tail[p] = t;
    ready_bitmap |= 1u << p;
}
static void rq_unlink(int p, tcb_t *prev, tcb_t *t) {
    if (prev) {
        prev->rq_next = t->rq_next;
    } else {
        ready_head[p] = t->rq_next;
    }
    if (ready_tail[p] == t) {
        ready_tail[p] = prev;
    }
    if (!ready_head[p]) {
        ready_bitmap &= ~(1u << p);
    }
    t->in_ready_q = 0;
    t->rq_next = NULL;
}
static tcb_t *rq_pop_highest(uint32_t preferred_cpu) {
    while (ready_bitmap) {
        int p = 31 - __builtin_clz(ready_bitmap);
        tcb_t *t = ready_head[p];
        rq_unlink(p, NULL, t);
        if (t->state == THREAD_STATE_READY) {
            if (t->last_cpu == preferred_cpu) {
                sched_stats.affinity_hits++;
            } else if (t->last_cpu != (uint32_t)-1) {
                sched_stats.affinity_misses++;
            }
            return t;
        }
    }
    return NULL;
}
void sched_wake(tcb_t *t) {
    if (!t || t->state == THREAD_STATE_DEAD || t->state == THREAD_STATE_RUNNING
        || t->state == THREAD_STATE_ZOMBIE) {
        return;
    }
    if (t->state == THREAD_STATE_READY && t->in_ready_q) {
        return;
    }
    t->state = THREAD_STATE_READY;
    if (t->in_ready_q) {
    } else {
        tcb_t *old = __atomic_load_n(&wake_inbox, __ATOMIC_RELAXED);
        do {
            t->inbox_next = old;
        } while (!__atomic_compare_exchange_n(&wake_inbox, &old, t, 0,
                                              __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    }
    if (t->prio > current_thread->prio) {
        need_resched[this_cpu()->cpu_id] = 1;
    }
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (c == this_cpu()->cpu_id) {
            continue;
        }
        if (!cpu_online[c]) {
            continue;
        }
        if (armed_ticks[c] <= SCHED_TIMESLICE_TICKS) {
            continue;
        }
        tcb_t *other_current = percpu_current_thread(c);
        if (other_current && t->prio > other_current->prio) {
            arch_timer_kick_cpu(c);
            sched_stats.kicks_sent++;
        }
    }
}
static void drain_inbox(void) {
    tcb_t *list = __atomic_exchange_n(&wake_inbox, NULL, __ATOMIC_ACQUIRE);
    while (list) {
        tcb_t *next = list->inbox_next;
        list->inbox_next = NULL;
        if (list->state == THREAD_STATE_READY) {
            rq_push(list);
        }
        list = next;
    }
}
void sched_ready_now(tcb_t *t) {
    t->state = THREAD_STATE_READY;
    rq_push(t);
}
void sched_direct_switch(tcb_t *dest) {
#if ROBU_TRACE
    tid_t from_tid = current_thread->tid;
#endif
    dest->state = THREAD_STATE_RUNNING;
    dest->last_cpu = this_cpu()->cpu_id;
    current_thread = dest;
    sched_stats.direct_switches++;
    TRACE(TRACE_EVT_CTX_SWITCH, from_tid, dest->tid, 1 , 0, 0, 0);
}
void sched_block(thread_state_t why) {
    current_thread->state = why;
}

int sched_signal_pgid(tid_t pgid, int signum) {
    if (pgid == 0) {
        return -1;
    }

    int sent = 0;
    for (tid_t tid = 1; tid < SCHED_MAX_THREADS; tid++) {
        tcb_t *t = sched_get_tcb(tid);
        if (!t || t->pgid != pgid) {
            continue;
        }
        if (sig_send(tid, signum) == 0) {
            sent++;
        }
    }
    return sent;
}
static void terminate_tcb(tcb_t *t, thread_state_t final_state) {
    t->state = final_state;
    tcb_t *s = t->send_q_head;
    t->send_q_head = t->send_q_tail = NULL;
    while (s) {
        tcb_t *next = s->sq_next;
        s->sq_next = NULL;
        s->uctx.rax = (uint64_t)IPC_ERR_NOT_FOUND;
        sched_wake(s);
        s = next;
    }
    int sibling_found = 0;
    for (int i = 1; i < SCHED_MAX_THREADS; i++) {
        tcb_t *w = &thread_table[i];
        if (w->state == THREAD_STATE_WAIT_RECV && w->ipc_partner == t->tid) {
            w->uctx.rax = (uint64_t)IPC_ERR_NOT_FOUND;
            sched_wake(w);
        }
        if (w != t && w->state != THREAD_STATE_DEAD && w->state != THREAD_STATE_ZOMBIE
            && w->address_space == t->address_space) {
            sibling_found = 1;
        }
        if (w->parent_tid == t->tid) {
            w->parent_tid = 0;
            if (w->state == THREAD_STATE_ZOMBIE) {
                w->state = THREAD_STATE_DEAD;
            }
        }
    }
    if (t->address_space != 0 && !sibling_found) {
        if (t == current_thread) {
            arch_vm_activate(0);
        }
        shm_detach_all_for_process(t->address_space);
        vm_address_space_destroy(t->address_space);
        t->address_space = 0;
    }
    kcap_invalidate_tcb_death(t->tid);
    notif_invalidate_waiter_death(t->tid);
    pipe_invalidate_tcb_death(t->tid);
    sock_cleanup_for_process(t->tid);
}
void sched_terminate_current(void) {
    sched_terminate_to(THREAD_STATE_DEAD);
}
void sched_terminate_to(thread_state_t final_state) {
    terminate_tcb(current_thread, final_state);
    need_resched[this_cpu()->cpu_id] = 1;
}
int sched_terminate(tid_t victim_tid) {
    tcb_t *t = sched_get_tcb(victim_tid);
    if (!t || t == current_thread || t->state == THREAD_STATE_RUNNING) {
        return -1;
    }
    terminate_tcb(t, THREAD_STATE_DEAD);
    return 0;
}
void sched_yield(void) {
    need_resched[this_cpu()->cpu_id] = 1;
}
void sched_request_resched(void) {
    need_resched[this_cpu()->cpu_id] = 1;
}
void sched_sleep(uint64_t ticks) {
    if (ticks == 0) {
        sched_yield();
        return;
    }
    uint64_t wake_at = now_ticks + ticks;
    current_thread->wake_at_tick = wake_at;
    current_thread->state = THREAD_STATE_SLEEPING;
    if (wake_at < next_deadline) {
        next_deadline = wake_at;
    }
}
static void schedule(void) {
    drain_inbox();
    tcb_t *my_idle = &idle_thread[this_cpu()->cpu_id];
    tcb_t *prev = current_thread;
    if (prev == my_idle) {
        prev->state = THREAD_STATE_READY;
    } else {
        if (prev->state == THREAD_STATE_RUNNING) {
            prev->state = THREAD_STATE_READY;
        }
        if (prev->state == THREAD_STATE_READY) {
            rq_push(prev);
        }
    }
    tcb_t *next = rq_pop_highest(this_cpu()->cpu_id);
    if (!next) {
        next = my_idle;
    }
    next->state = THREAD_STATE_RUNNING;
    next->last_cpu = this_cpu()->cpu_id;
    if (next->timeslice_left <= 0) {
        next->timeslice_left = SCHED_TIMESLICE_TICKS;
    }
    current_thread = next;
    need_resched[this_cpu()->cpu_id] = 0;
    sched_stats.full_scheds++;
    TRACE(TRACE_EVT_CTX_SWITCH, prev->tid, next->tid, 0 , 0, 0, 0);
}
static void sched_switch_to(tcb_t *from, tcb_t *to) {
    if (to != from) {
        arch_fpu_save(from->fpu_state);
        arch_fpu_restore(to->fpu_state);
        if (to->fs_base != from->fs_base) {
            arch_set_fsbase(to->fs_base);
        }
    }
    arch_vm_activate(to->address_space);
}
void sched_resume(void) {
    tcb_t *prev = current_thread;
    if (need_resched[this_cpu()->cpu_id] || current_thread->state != THREAD_STATE_RUNNING) {
        schedule();
        sched_arm_next_deadline();
    }
    tcb_t *next = current_thread;
    sched_switch_to(prev, next);

    while (next->address_space != 0 && !next->in_sig_handler
           && (next->sig_pending & ~next->sig_mask)) {
        sig_try_deliver(next);
        if (next->state == THREAD_STATE_RUNNING) {
            break;
        }
        schedule();
        sched_arm_next_deadline();
        tcb_t *resched_next = current_thread;
        sched_switch_to(next, resched_next);
        next = resched_next;
    }

    sched_lock_release();
    arch_enter_thread_raw(&next->uctx);
}
static void sched_tick_local(uint64_t n) {
    tcb_t *cur = current_thread;
    if (cur != &idle_thread[this_cpu()->cpu_id]) {
        cur->timeslice_left -= (int32_t)n;
        if (cur->timeslice_left <= 0) {
            need_resched[this_cpu()->cpu_id] = 1;
            sched_stats.preempts++;
        }
    }
}
void sched_tick(void) {
    uint32_t cpu = this_cpu()->cpu_id;
    uint64_t n = armed_ticks[cpu];
    if (n < 1) {
        n = 1;
    }
    sched_stats.timer_traps++;
    if (cpu == 0) {
        now_ticks += n;
        sched_stats.ticks += n;
        kinfo_tick(n);
        if (now_ticks >= next_deadline) {
            next_deadline = (uint64_t)-1;
            for (int i = 1; i < SCHED_MAX_THREADS; i++) {
                tcb_t *t = &thread_table[i];
                if (t->state != THREAD_STATE_SLEEPING) {
                    continue;
                }
                if (t->wake_at_tick <= now_ticks) {
                    sched_wake(t);
                } else if (t->wake_at_tick < next_deadline) {
                    next_deadline = t->wake_at_tick;
                }
            }
            uint64_t next_timer;
            cap_timer_rescan(now_ticks, &next_timer);
            if (next_timer < next_deadline) {
                next_deadline = next_timer;
            }
        }
    }
    sched_tick_local(n);
    sched_arm_next_deadline();
}
uint64_t sched_now(void) {
    return now_ticks;
}
static void sched_arm_next_deadline(void) {
    uint32_t cpu = this_cpu()->cpu_id;
    tcb_t *cur = current_thread;
    tcb_t *my_idle = &idle_thread[cpu];
    uint64_t n;
    if (cur == my_idle) {
        n = SCHED_MAX_ARM_TICKS;
    } else {
        n = cur->timeslice_left > 0 ? (uint64_t)cur->timeslice_left : 1;
    }
    if (cpu == 0 && next_deadline != (uint64_t)-1) {
        uint64_t sleeper_ticks = next_deadline > now_ticks
                                      ? next_deadline - now_ticks
                                      : 1;
        if (sleeper_ticks < n) {
            n = sleeper_ticks;
        }
    }
    if (n > SCHED_MAX_ARM_TICKS) {
        n = SCHED_MAX_ARM_TICKS;
    }
    if (n < 1) {
        n = 1;
    }
    armed_ticks[cpu] = n;
    arch_timer_arm(n);
}
static void idle_entry(void) {
    for (;;) {
        arch_idle();
    }
}
static void init_idle_for_this_cpu(void) {
    uint32_t id = this_cpu()->cpu_id;
    tcb_t *idle = &idle_thread[id];
    idle->state = THREAD_STATE_READY;
    idle->prio = 0;
    idle->name = (id == 0) ? "idle-bsp" : "idle-ap";
    arch_uctx_init(&idle->uctx, idle_entry, idle_stack[id] + sizeof(idle_stack[id]));
    idle->kstack_top = idle_stack[id] + sizeof(idle_stack[id]);
    idle->kstack_size = sizeof(idle_stack[id]);
    current_thread = idle;
}
void sched_init(void) {
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        thread_table[i].tid = (tid_t)i;
        thread_table[i].state = THREAD_STATE_DEAD;
    }
    init_idle_for_this_cpu();
}
void sched_init_ap(void) {
    init_idle_for_this_cpu();
}
static tcb_t *alloc_tcb(const char *name, uint8_t prio) {
    for (int i = 1; i < SCHED_MAX_THREADS; i++) {
        tcb_t *t = &thread_table[i];
        if (t->state != THREAD_STATE_DEAD) {
            continue;
        }
        tid_t tid = t->tid;
        memset(t, 0, sizeof(*t));
        t->tid = tid;
        t->name = name;
        t->prio = prio;
        t->pgid = tid;
        t->sid = tid;
        t->timeslice_left = SCHED_TIMESLICE_TICKS;
        t->last_cpu = (uint32_t)-1;
        arch_fpu_default_state(t->fpu_state);
        return t;
    }
    return NULL;
}
tcb_t *thread_create(const char *name, void (*entry)(void), void *stack_top, uint8_t prio) {
    sched_lock_acquire();
    tcb_t *t = alloc_tcb(name, prio);
    if (t) {
        arch_uctx_init(&t->uctx, entry, stack_top);
        t->kstack_top = stack_top;
        t->kstack_size = STACK_SIZE;
        sched_ready_now(t);
    }
    sched_lock_release();
    if (!t) {
        return NULL;
    }
    if (!quiet_mode) {
        kprintf("[sched] spawn tid=%u '%s' prio=%u\n", t->tid, name, prio);
    }
    return t;
}
tcb_t *thread_create_user_locked(const char *name, vaddr_t entry, vaddr_t user_stack_top,
                                 uint8_t prio, paddr_t address_space, tid_t pager_tid) {
    tcb_t *t = alloc_tcb(name, prio);
    if (t) {
        t->address_space = address_space;
        t->pager_tid = pager_tid;
        arch_uctx_init_user(&t->uctx, entry, user_stack_top);
        sched_ready_now(t);
    }
    return t;
}
tcb_t *thread_create_user(const char *name, vaddr_t entry, vaddr_t user_stack_top,
                          uint8_t prio, paddr_t address_space, tid_t pager_tid) {
    sched_lock_acquire();
    tcb_t *t = thread_create_user_locked(name, entry, user_stack_top, prio, address_space, pager_tid);
    sched_lock_release();
    if (!t) {
        return NULL;
    }
    if (!quiet_mode) {
        kprintf("[sched] spawn tid=%u '%s' prio=%u ring3 as=0x%lx pager=%u\n",
                t->tid, name, prio, address_space, pager_tid);
    }
    return t;
}
tcb_t *thread_create_user_argv_locked(const char *name, vaddr_t entry, vaddr_t user_stack_top,
                                      uint8_t prio, paddr_t address_space, tid_t pager_tid,
                                      uint64_t argc, uint64_t argv, uint64_t envp,
                                      uint64_t heap_base, uint64_t spawn_info) {
    tcb_t *t = alloc_tcb(name, prio);
    if (t) {
        t->address_space = address_space;
        t->pager_tid = pager_tid;
        arch_uctx_init_user_argv(&t->uctx, entry, user_stack_top, argc, argv, envp,
                                 heap_base, spawn_info);
        sched_ready_now(t);
    }
    return t;
}
tcb_t *thread_create_forked_locked(const char *name, paddr_t address_space, tid_t pager_tid,
                                   uint8_t prio, const tcb_t *parent) {
    tcb_t *t = alloc_tcb(name, prio);
    if (t) {
        t->address_space = address_space;
        t->pager_tid = pager_tid;
        t->uctx = parent->uctx;

        t->fs_base = parent->fs_base;
        memcpy(t->fpu_state, parent->fpu_state, sizeof(t->fpu_state));
        sched_ready_now(t);
    }
    return t;
}
tcb_t *thread_create_user_argv(const char *name, vaddr_t entry, vaddr_t user_stack_top,
                               uint8_t prio, paddr_t address_space, tid_t pager_tid,
                               uint64_t argc, uint64_t argv, uint64_t envp, uint64_t heap_base,
                               uint64_t spawn_info) {
    sched_lock_acquire();
    tcb_t *t = thread_create_user_argv_locked(name, entry, user_stack_top, prio, address_space,
                                              pager_tid, argc, argv, envp, heap_base, spawn_info);
    sched_lock_release();
    if (!t) {
        return NULL;
    }
    if (!quiet_mode) {
        kprintf("[sched] spawn tid=%u '%s' prio=%u ring3 as=0x%lx pager=%u argc=%lu heap=0x%lx\n",
                t->tid, name, prio, address_space, pager_tid, argc, heap_base);
    }
    return t;
}
tcb_t *sched_get_tcb(tid_t tid) {
    if (tid == 0 || tid >= SCHED_MAX_THREADS) {
        return NULL;
    }
    tcb_t *t = &thread_table[tid];
    return (t->state == THREAD_STATE_DEAD || t->state == THREAD_STATE_ZOMBIE) ? NULL : t;
}
int sched_thread_info(tid_t tid, thread_state_t *state_out, uint8_t *prio_out,
                      int32_t *exit_status_out, tid_t *parent_tid_out,
                      const char **name_out) {
    if (tid == 0 || tid >= SCHED_MAX_THREADS) {
        return -1;
    }
    tcb_t *t = &thread_table[tid];
    if (t->state == THREAD_STATE_DEAD) {
        return -1;
    }
    *state_out = t->state;
    *prio_out = t->prio;
    *exit_status_out = t->exit_status;
    *parent_tid_out = t->parent_tid;
    *name_out = t->name ? t->name : "";
    return 0;
}
tcb_t *sched_find_zombie_child(tid_t parent, tid_t filter, int *out_has_live) {
    *out_has_live = 0;
    tcb_t *found = NULL;
    for (int i = 1; i < SCHED_MAX_THREADS; i++) {
        tcb_t *w = &thread_table[i];
        if (w->parent_tid != parent) {
            continue;
        }
        if (filter != 0 && w->tid != filter) {
            continue;
        }
        if (w->state == THREAD_STATE_ZOMBIE) {
            if (!found) {
                found = w;
            }
        } else if (w->state != THREAD_STATE_DEAD) {
            *out_has_live = 1;
        }
    }
    return found;
}
void sched_reap_zombie(tcb_t *t) {
    t->state = THREAD_STATE_DEAD;
}
void sched_start(void) {
    kprintf("[sched] %u prio levels, %u Hz tick, %u-tick quantum, per-CPU kernel stacks\n",
            SCHED_PRIO_LEVELS, SCHED_HZ, SCHED_TIMESLICE_TICKS);
    sched_lock_acquire();
    schedule();
    sched_stats.full_scheds = 0;
    tcb_t *next = current_thread;
    arch_fpu_restore(next->fpu_state);
    arch_set_fsbase(next->fs_base);
    sched_arm_next_deadline();
    arch_vm_activate(next->address_space);
    sched_lock_release();
    arch_enter_thread_raw(&next->uctx);
}
void sched_join_ap(void) {
    sched_lock_acquire();
    schedule();
    tcb_t *next = current_thread;
    arch_fpu_restore(next->fpu_state);
    arch_set_fsbase(next->fs_base);
    sched_arm_next_deadline();
    arch_vm_activate(next->address_space);
    sched_lock_release();
    arch_enter_thread_raw(&next->uctx);
}
