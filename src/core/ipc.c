#include "robu/ipc.h"
#include "robu/sched.h"
#include "robu/vm.h"
#include "robu/captable.h"
#include "robu/trace.h"
#include "robu/kprintf.h"
#include "robu/spawn.h"
#include "robu/pipe.h"
#include "robu/pmm.h"
#include "robu/kinfo.h"
#include "robu/rootfs.h"
#include "robu/arch.h"
#include "robu/signal.h"
#include "robu/framebuffer.h"
#include "robu/shm.h"
#include "robu/socket.h"
#include "robu/hwdrv.h"
#include "robu/dma.h"
#include "robu/execreq.h"
#include "robu/vfs.h"
#include "robu/random.h"
#include "robu/blockinfo.h"
static tid_t console_writer_tid = 0;
static tid_t console_driver_tid = 0;
#define CONSOLE_VT_COUNT 6
static tid_t console_fg_pgid[CONSOLE_VT_COUNT];
static tid_t console_read_waiter[CONSOLE_VT_COUNT];

static void complete_console_read(tcb_t *t, int vt) {
    uint64_t words[5] = {0, 0, 0, 0, 0};
    int n = arch_console_read_line_bytes(vt, (uint8_t *)words, 40);

    if (n < 0) {
        return;
    }

    t->uctx.r8 = (uint64_t)n;
    t->uctx.r9 = words[0];
    t->uctx.r10 = words[1];
    t->uctx.r12 = words[2];
    t->uctx.r13 = words[3];
    t->uctx.r14 = words[4];
    t->uctx.rax = (uint64_t)IPC_ERR_NONE;
    console_read_waiter[vt] = 0;
}

void ipc_console_input_available(int vt) {
    if (vt < 0 || vt >= CONSOLE_VT_COUNT) {
        return;
    }

    sched_lock_acquire();

    tid_t tid = console_read_waiter[vt];
    if (tid != 0) {
        tcb_t *t = sched_get_tcb(tid);
        if (t && t->state == THREAD_STATE_WAIT_CONSOLE) {
            complete_console_read(t, vt);
            if (console_read_waiter[vt] == 0) {
                sched_wake(t);
            }
        } else {
            console_read_waiter[vt] = 0;
        }
    }

    sched_lock_release();
}

void ipc_console_interrupt(int vt, int signum) {
    if (vt < 0 || vt >= CONSOLE_VT_COUNT) {
        return;
    }

    tid_t waiter = console_read_waiter[vt];
    if (waiter != 0) {
        tcb_t *t = sched_get_tcb(waiter);
        if (t && t->state == THREAD_STATE_WAIT_CONSOLE) {
            t->uctx.rax = (uint64_t)IPC_ERR_CANCELED;
            console_read_waiter[vt] = 0;
            sched_wake(t);
        } else {
            console_read_waiter[vt] = 0;
        }
    }
    sched_signal_pgid(console_fg_pgid[vt], signum);
}

void ipc_grant_console_writer(tid_t tid) {
    console_writer_tid = tid;
}
void ipc_grant_console_driver(tid_t tid) {
    console_driver_tid = tid;
}
static tid_t pager_owner_tid = 0;
void ipc_grant_pager_owner(tid_t tid) {
    pager_owner_tid = tid;
}
static tid_t hw_driver_tid = 0;
void ipc_grant_hw_driver(tid_t tid) {
    hw_driver_tid = tid;
}
#define HW_DRIVER_SECONDARY_MAX 4
static tid_t hw_driver_secondary_tids[HW_DRIVER_SECONDARY_MAX];
void ipc_grant_hw_driver_secondary(tid_t tid) {
    for (int i = 0; i < HW_DRIVER_SECONDARY_MAX; i++) {
        if (hw_driver_secondary_tids[i] == 0) {
            hw_driver_secondary_tids[i] = tid;
            return;
        }
    }
}
static int hw_driver_allows(tid_t tid) {
    if (tid == hw_driver_tid) {
        return 1;
    }
    for (int i = 0; i < HW_DRIVER_SECONDARY_MAX; i++) {
        if (hw_driver_secondary_tids[i] == tid) {
            return 1;
        }
    }
    return 0;
}
#define HW_PORT_IO_SECONDARY_MAX 4
static tid_t hw_port_io_secondary_tids[HW_PORT_IO_SECONDARY_MAX];
void ipc_grant_hw_port_io_secondary(tid_t tid) {
    for (int i = 0; i < HW_PORT_IO_SECONDARY_MAX; i++) {
        if (hw_port_io_secondary_tids[i] == 0) {
            hw_port_io_secondary_tids[i] = tid;
            return;
        }
    }
}
static int hw_port_io_secondary_allows(tid_t tid) {
    for (int i = 0; i < HW_PORT_IO_SECONDARY_MAX; i++) {
        if (hw_port_io_secondary_tids[i] == tid) {
            return 1;
        }
    }
    return 0;
}
static tid_t pci_cfg_secondary_tid = 0;
void ipc_grant_pci_cfg_secondary(tid_t tid) {
    pci_cfg_secondary_tid = tid;
}

static void payload_from_frame(msg_regs_t *m, const arch_uctx_t *f) {
    m->word[0] = f->r8;
    m->word[1] = f->r9;
    m->word[2] = f->r10;
    m->word[3] = f->r12;
    m->word[4] = f->r13;
    m->word[5] = f->r14;
}

static void payload_to_frame(arch_uctx_t *f, const msg_regs_t *m) {
    f->r8 = m->word[0];
    f->r9 = m->word[1];
    f->r10 = m->word[2];
    f->r12 = m->word[3];
    f->r13 = m->word[4];
    f->r14 = m->word[5];
}

static void sendq_append(tcb_t *rcv, tcb_t *s) {
    s->sq_next = NULL;
    if (rcv->send_q_tail) {
        rcv->send_q_tail->sq_next = s;
    } else {
        rcv->send_q_head = s;
    }
    rcv->send_q_tail = s;
}

static tcb_t *sendq_take(tcb_t *rcv, tid_t filter) {
    tcb_t *prev = NULL;
    for (tcb_t *s = rcv->send_q_head; s; prev = s, s = s->sq_next) {
        if (filter != IPC_TID_ANY && s->tid != filter) {
            continue;
        }
        if (prev) {
            prev->sq_next = s->sq_next;
        } else {
            rcv->send_q_head = s->sq_next;
        }
        if (rcv->send_q_tail == s) {
            rcv->send_q_tail = prev;
        }
        s->sq_next = NULL;
        return s;
    }
    return NULL;
}

static void deliver_to_frame(tcb_t *rcv, tid_t from, const msg_regs_t *m) {
    arch_uctx_t *f = &rcv->uctx;
    payload_to_frame(f, m);
    f->rsi = from;
    f->rax = (uint64_t)IPC_ERR_NONE;
}

static void complete_parked_sender(tcb_t *cur, arch_uctx_t *f, tcb_t *s) {
    int xfer_rc = 0;
    if (s->ipc_flags & IPC_FLAG_MAP) {
        xfer_rc = vm_batch_map(cur->address_space, s->tid, (vm_map_msg_t *)&s->ipc_buffer);
    } else if (s->ipc_flags & IPC_FLAG_XFER) {
        xfer_rc = vm_xfer_pages(s->address_space, cur->address_space,
                                (vm_xfer_msg_t *)&s->ipc_buffer);
    } else if (s->ipc_flags & IPC_FLAG_SHARE) {
        xfer_rc = vm_share_pages(s->address_space, cur->address_space,
                                 (vm_xfer_msg_t *)&s->ipc_buffer);
    }
    payload_to_frame(f, &s->ipc_buffer);
    f->rsi = s->tid;
    f->rax = (uint64_t)IPC_ERR_NONE;
    s->uctx.rax = (uint64_t)(xfer_rc == 0 ? IPC_ERR_NONE : IPC_ERR_NO_CAP);
    sched_stats.ipc_msgs++;
    TRACE(TRACE_EVT_IPC_RECV, cur->tid, s->tid, s->ipc_flags, 0, 0, 0);
    if (s->ipc_recv_after != (tid_t)~0u) {
        s->ipc_partner = s->ipc_recv_after;
        s->ipc_recv_after = (tid_t)~0u;
        s->state = THREAD_STATE_WAIT_RECV;
    } else {
        sched_wake(s);
    }
}

void sys_ipc(void) {
    tcb_t *cur = current_thread;
    arch_uctx_t *f = &cur->uctx;
    tid_t dest_tid = (tid_t)f->rdi;
    tid_t src_filter = (tid_t)f->rsi;
    uint32_t flags = (uint32_t)f->rdx;
    int want_recv = flags & IPC_FLAG_RECV;

    if (dest_tid == 0 && !want_recv) {
        if (flags & IPC_FLAG_RETYPE) {
            uint64_t out_slot = 0, out_addr = 0;
            int rc = cap_retype(cur->tid, (uint32_t)f->r8, (uint32_t)f->r9,
                                 f->r10, f->r12, f->r13, f->r14, &out_slot, &out_addr);
            f->rax = (uint64_t)(rc == 0 ? IPC_ERR_NONE : IPC_ERR_NO_CAP);
            f->r8 = out_slot;
            f->r9 = out_addr;
            return;
        }
        if (flags & IPC_FLAG_DESTROY) {
            int rc = cap_destroy(cur->tid, (uint32_t)f->r8);
            f->rax = (uint64_t)(rc == 0 ? IPC_ERR_NONE : IPC_ERR_NO_CAP);
            return;
        }
        if (flags & IPC_FLAG_CONSOLE_WRITE) {
            if (cur->tid != console_writer_tid && cur->tid != console_driver_tid) {
                f->rax = (uint64_t)IPC_ERR_NO_CAP;
                return;
            }
            uint64_t len = f->r8 & 0xFF;
            if (len > 40) {
                len = 40;
            }
            int vt = (int)((f->r8 >> 8) & 0xFF);
            uint64_t words[5] = { f->r9, f->r10, f->r12, f->r13, f->r14 };
            kwrite(vt, (const uint8_t *)words, len);
            f->rax = (uint64_t)IPC_ERR_NONE;
            return;
        }
        if (flags & IPC_FLAG_CONSOLE_READ) {
            if (cur->tid != console_writer_tid) {
                f->rax = (uint64_t)IPC_ERR_NO_CAP;
                return;
            }
            int vt = (int)f->r8;
            if (vt < 0 || vt >= CONSOLE_VT_COUNT) {
                f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                return;
            }

            uint64_t words[5] = {0, 0, 0, 0, 0};
            int n = arch_console_read_line_bytes(vt, (uint8_t *)words, 40);

            if (n < 0) {
                if (console_read_waiter[vt] != 0) {
                    f->rax = (uint64_t)IPC_ERR_WOULDBLOCK;
                    return;
                }

                console_read_waiter[vt] = cur->tid;
                sched_block(THREAD_STATE_WAIT_CONSOLE);
                return;
            }

            f->r8 = (uint64_t)n;
            f->r9 = words[0];
            f->r10 = words[1];
            f->r12 = words[2];
            f->r13 = words[3];
            f->r14 = words[4];
            f->rax = (uint64_t)IPC_ERR_NONE;
            return;
        }
        if (flags & IPC_FLAG_NOTIFY_SIGNAL) {
            int rc = cap_notif_signal(cur->tid, (uint32_t)f->r8, f->r9);
            f->rax = (uint64_t)(rc == 0 ? IPC_ERR_NONE : IPC_ERR_NO_CAP);
            return;
        }
        if (flags & IPC_FLAG_NOTIFY_WAIT) {
            uint64_t bits = 0;
            int rc = cap_notif_wait_begin(cur->tid, (uint32_t)f->r8, &bits);
            if (rc < 0) {
                f->rax = (uint64_t)IPC_ERR_NO_CAP;
                return;
            }
            if (rc == 1) {
                f->rax = (uint64_t)IPC_ERR_NONE;
                f->r8 = bits;
                return;
            }
            sched_block(THREAD_STATE_WAIT_NOTIFICATION);
            return;
        }
        if (flags & IPC_FLAG_NOTIFY_POLL) {
            uint64_t bits = 0;
            int rc = cap_notif_poll(cur->tid, (uint32_t)f->r8, &bits);
            if (rc == -1) {
                f->rax = (uint64_t)IPC_ERR_NO_CAP;
                return;
            }
            if (rc == -2) {
                f->rax = (uint64_t)IPC_ERR_WOULDBLOCK;
                return;
            }
            f->rax = (uint64_t)IPC_ERR_NONE;
            f->r8 = bits;
            return;
        }
        if (flags & IPC_FLAG_TIMER_ARM) {
            int rc = cap_timer_arm(cur->tid, (uint32_t)f->r8, (uint32_t)f->r9,
                                   f->r10, f->r12, f->r13);
            f->rax = (uint64_t)(rc == 0 ? IPC_ERR_NONE : IPC_ERR_NO_CAP);
            return;
        }
        if (flags & IPC_FLAG_TIMER_DISARM) {
            int rc = cap_timer_disarm(cur->tid, (uint32_t)f->r8);
            f->rax = (uint64_t)(rc == 0 ? IPC_ERR_NONE : IPC_ERR_NO_CAP);
            return;
        }
        if (flags & IPC_FLAG_REVOKE_FRAME) {
            int rc = cap_revoke_frame(cur->tid, (uint32_t)f->r8);
            f->rax = (uint64_t)(rc == 0 ? IPC_ERR_NONE : IPC_ERR_NO_CAP);
            return;
        }
        if (flags & IPC_FLAG_EXIT) {
            if (cur->address_space == 0) {
                f->rax = (uint64_t)IPC_ERR_NO_CAP;
                return;
            }
            spawn_exit_current((int)(f->r8 & 0xFF));
            return;
        }
        if (flags & IPC_FLAG_SPAWN) {
            tid_t child_tid = 0;
            int rc = spawn_create(cur, f->r8, f->r9, &child_tid);
            if (rc != IPC_ERR_BLOCKED) {
                f->rax = (uint64_t)rc;
                f->r8 = child_tid;
            }
            return;
        }
        if (flags & IPC_FLAG_WAIT) {
            tid_t filter = (tid_t)f->r8;
            tid_t reaped_tid;
            int status;
            int rc = spawn_wait_begin(cur, filter, &reaped_tid, &status);
            if (rc == 1) {
                f->rax = (uint64_t)IPC_ERR_NONE;
                f->r8 = reaped_tid;
                f->r9 = (uint64_t)(int64_t)status;
                return;
            }
            if (rc == -1) {
                f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                return;
            }
            if (flags & IPC_FLAG_NOBLOCK) {
                f->rax = (uint64_t)IPC_ERR_WOULDBLOCK;
                return;
            }
            cur->wait_filter = filter;
            sched_block(THREAD_STATE_WAIT_CHILD);
            return;
        }
        if (flags & IPC_FLAG_PIPE_CREATE) {
            uint64_t handle = 0;
            int64_t rc = pipe_create(cur->tid, &handle);
            f->rax = (uint64_t)rc;
            f->r8 = handle;
            return;
        }
        if (flags & IPC_FLAG_PIPE_READ) {
            uint64_t out_len = 0;
            uint8_t buf[PIPE_CHUNK_MAX];
            int64_t rc = pipe_read(f->r8, buf, f->r9, &out_len);
            f->rax = (uint64_t)rc;
            f->r8 = out_len;
            uint64_t words[4] = {0, 0, 0, 0};
            for (uint64_t i = 0; i < out_len; i++) {
                ((uint8_t *)words)[i] = buf[i];
            }
            f->r10 = words[0];
            f->r12 = words[1];
            f->r13 = words[2];
            f->r14 = words[3];
            return;
        }
        if (flags & IPC_FLAG_PIPE_WRITE) {
            uint64_t len = f->r9 > PIPE_CHUNK_MAX ? PIPE_CHUNK_MAX : f->r9;
            uint64_t words[4] = { f->r10, f->r12, f->r13, f->r14 };
            uint64_t out_len = 0;
            int64_t rc = pipe_write(f->r8, (const uint8_t *)words, len, &out_len);
            f->rax = (uint64_t)rc;
            f->r8 = out_len;
            return;
        }
        if (flags & IPC_FLAG_PIPE_CLOSE) {
            int64_t rc = pipe_close(cur->tid, f->r8, (int)f->r9);
            f->rax = (uint64_t)rc;
            return;
        }
        if (flags & IPC_FLAG_FORK) {
            tid_t child_tid = 0;
            int rc = spawn_fork_create(cur, &child_tid);
            f->rax = (uint64_t)rc;
            f->r8 = child_tid;
            return;
        }
        if (flags & IPC_FLAG_SELF_TID) {
            f->rax = (uint64_t)IPC_ERR_NONE;
            f->r8 = (uint64_t)cur->tid;
            return;
        }
        if (flags & IPC_FLAG_EXEC) {
            int rc = spawn_exec(cur, f->r8, f->r9);
            if (rc != IPC_ERR_NONE && rc != IPC_ERR_BLOCKED) {

                f->rax = (uint64_t)rc;
            }
            return;
        }
        if (flags & IPC_FLAG_MOUNT) {

            if (cur->parent_tid != 0) {
                f->rax = (uint64_t)IPC_ERR_NO_CAP;
                return;
            }
            uint64_t len = f->r8;
            if (len >= MOUNT_PREFIX_MAX) {
                len = MOUNT_PREFIX_MAX - 1;
            }
            uint64_t words[5] = { f->r9, f->r10, f->r12, f->r13, f->r14 };
            char prefix[MOUNT_PREFIX_MAX];
            memcpy(prefix, words, len);
            prefix[len] = '\0';
            int rc = kinfo_mount_add(prefix, (uint32_t)cur->tid);
            f->rax = (uint64_t)(rc == 0 ? IPC_ERR_NONE : IPC_ERR_NO_MEM);
            return;
        }
        if (flags & IPC_FLAG_BOOTFS) {

            uint64_t category = f->r8;
            if (category == 0) {

                uint64_t words[3] = { f->r9, f->r10, f->r12 };
                char name[25];
                memcpy(name, words, 24);
                name[24] = '\0';
                const uint8_t *start, *end;
                if (rootfs_lookup(name, &start, &end) != 0) {
                    f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                    return;
                }
                f->rax = (uint64_t)IPC_ERR_NONE;
                f->r8 = (uint64_t)(end - start);
                return;
            }
            if (category == 1) {

                uint64_t index = f->r9;
                char name[25] = {0};
                uint64_t size;
                if (rootfs_readdir(index, name, sizeof(name), &size) != 0) {
                    f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                    return;
                }
                uint64_t words[3] = {0, 0, 0};
                memcpy(words, name, 24);
                f->rax = (uint64_t)IPC_ERR_NONE;
                f->r8 = size;
                f->r9 = words[0];
                f->r10 = words[1];
                f->r12 = words[2];
                return;
            }
            if (category == 2) {

                uint64_t words[3] = { f->r9, f->r10, f->r12 };
                char name[25];
                memcpy(name, words, 24);
                name[24] = '\0';
                uint64_t offset = f->r13;
                uint64_t len = f->r14;
                if (len > 40) {
                    len = 40;
                }
                const uint8_t *start, *end;
                if (rootfs_lookup(name, &start, &end) != 0) {
                    f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                    return;
                }
                uint64_t size = (uint64_t)(end - start);
                uint64_t avail = size > offset ? size - offset : 0;
                uint64_t n = len < avail ? len : avail;
                uint8_t buf[40] = {0};
                memcpy(buf, start + offset, n);
                uint64_t outwords[5];
                memcpy(outwords, buf, 40);
                f->rax = (uint64_t)n;
                f->r8 = outwords[0];
                f->r9 = outwords[1];
                f->r10 = outwords[2];
                f->r12 = outwords[3];
                f->r13 = outwords[4];
                return;
            }
            f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
            return;
        }
        if (flags & IPC_FLAG_RESOLVE_FAULT) {

            if (cur->tid != pager_owner_tid || pager_owner_tid == 0) {
                f->rax = (uint64_t)IPC_ERR_NO_CAP;
                return;
            }
            tid_t target = (tid_t)f->r8;
            vaddr_t page_va = (vaddr_t)f->r9;
            int color = (int)f->r10;
            uint32_t prot = (uint32_t)f->r12;
            tcb_t *target_tcb = sched_get_tcb(target);
            if (!target_tcb) {
                f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                return;
            }
            paddr_t frame = pmm_alloc(color);
            if (!frame) {
                f->rax = (uint64_t)IPC_ERR_NO_MEM;
                return;
            }
            arch_vm_map_page(target_tcb->address_space, page_va, frame, prot);
            f->rax = (uint64_t)IPC_ERR_NONE;
            return;
        }
        if (flags & IPC_FLAG_SET_FSBASE) {
            cur->fs_base = f->r8;
            arch_set_fsbase(cur->fs_base);
            f->rax = (uint64_t)IPC_ERR_NONE;
            return;
        }
        if (flags & IPC_FLAG_THREAD_INFO) {
            thread_state_t state;
            uint8_t prio;
            int32_t exit_status;
            tid_t parent_tid;
            const char *name;
            if (sched_thread_info((tid_t)f->r8, &state, &prio, &exit_status,
                                  &parent_tid, &name) != 0) {
                f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                return;
            }
            uint64_t w13 = 0, w14 = 0;
            int ni = 0;
            for (; ni < 8 && name[ni]; ni++) {
                w13 |= ((uint64_t)(uint8_t)name[ni]) << (8 * ni);
            }
            if (ni == 8) {
                for (int nj = 0; nj < 8 && name[8 + nj]; nj++) {
                    w14 |= ((uint64_t)(uint8_t)name[8 + nj]) << (8 * nj);
                }
            }
            f->rax = (uint64_t)IPC_ERR_NONE;
            f->r8 = (uint64_t)state;
            f->r9 = (uint64_t)prio;
            f->r10 = (uint64_t)(int64_t)exit_status;
            f->r12 = (uint64_t)parent_tid;
            f->r13 = w13;
            f->r14 = w14;
            return;
        }
        if (flags & IPC_FLAG_SYS_INFO) {
            uint64_t category = f->r8;
            if (category == 0) {
                f->rax = (uint64_t)IPC_ERR_NONE;
                f->r8 = pmm_stats.total_frames;
                f->r9 = pmm_stats.free_frames;
                f->r10 = pmm_stats.alloc_calls;
                f->r12 = pmm_stats.free_calls;
            } else if (category == 1) {
                f->rax = (uint64_t)IPC_ERR_NONE;
                f->r8 = sched_stats.full_scheds;
                f->r9 = sched_stats.preempts;
                f->r10 = sched_stats.ipc_msgs;
                f->r12 = sched_stats.ticks;
                f->r13 = sched_stats.timer_traps;
                f->r14 = sched_stats.kicks_sent;
            } else if (category == 2) {
                kprintf("[power] rebooting\n");
                arch_reboot();
            } else if (category == 3) {
                kprintf("[power] halting\n");
                arch_halt();
            } else if (category == 4) {
                kprintf("[power] shutting down\n");
                arch_shutdown();
            } else if (category == SYS_INFO_CAT_SIGACTION) {
                int signum = (int)f->r9;
                int do_set = (int)f->r13;
                uint64_t new_handler = f->r10;
                uint64_t new_flags = f->r12;
                uint64_t new_mask = f->r14;
                if (signum < 1 || signum >= ROBU_NSIG) {
                    f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                } else {
                    sig_action_t *sa = &cur->sig_actions[signum];
                    f->r8 = sa->handler;
                    f->r9 = sa->flags;
                    f->r10 = sa->mask;
                    if (do_set) {
                        sa->handler = new_handler;
                        sa->flags = new_flags;
                        sa->mask = new_mask;
                    }
                    f->rax = (uint64_t)IPC_ERR_NONE;
                }
            } else if (category == SYS_INFO_CAT_KILL) {
                tid_t target = (tid_t)f->r9;
                int signum = (int)f->r10;
                f->rax = (uint64_t)(sig_send(target, signum) == 0
                                     ? IPC_ERR_NONE : IPC_ERR_NOT_FOUND);
            } else if (category == SYS_INFO_CAT_SIGPROCMASK) {
                uint64_t how = f->r9;
                uint64_t new_mask = f->r10;
                int do_set = (int)f->r12;
                f->r8 = cur->sig_mask;
                if (do_set) {
                    if (how == SIG_BLOCK) {
                        cur->sig_mask |= new_mask;
                    } else if (how == SIG_UNBLOCK) {
                        cur->sig_mask &= ~new_mask;
                    } else {
                        cur->sig_mask = new_mask;
                    }
                }
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_SIGRETURN) {
                *f = cur->saved_uctx_before_sig;
                cur->sig_mask = cur->saved_sig_mask;
                cur->in_sig_handler = 0;
                return;
            } else if (category == SYS_INFO_CAT_SIGPENDING) {
                f->r8 = cur->sig_pending;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_ACTIVE_VT) {
                f->r8 = (uint64_t)arch_console_get_active_vt();
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_CONSOLE_MODE) {
                int vt = (int)f->r10;
                if (f->r9 == 2) {
                    f->r8 = (uint64_t)arch_console_get_raw_mode(vt);
                } else {
                    arch_console_set_raw_mode(vt, (int)f->r9);
                }
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_SETPGID) {
                tid_t target = (tid_t)f->r9;
                tid_t new_pgid = (tid_t)f->r10;
                if (target == 0) {
                    target = cur->tid;
                }
                if (new_pgid == 0) {
                    new_pgid = target;
                }
                tcb_t *t = sched_get_tcb(target);
                if (!t) {
                    f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                } else {
                    t->pgid = new_pgid;
                    f->rax = (uint64_t)IPC_ERR_NONE;
                }
            } else if (category == SYS_INFO_CAT_GETPGID) {
                tid_t target = (tid_t)f->r9;
                if (target == 0) {
                    target = cur->tid;
                }
                tcb_t *t = sched_get_tcb(target);
                if (!t) {
                    f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                } else {
                    f->r8 = (uint64_t)t->pgid;
                    f->rax = (uint64_t)IPC_ERR_NONE;
                }
            } else if (category == SYS_INFO_CAT_SETSID) {
                cur->sid = cur->tid;
                cur->pgid = cur->tid;
                f->r8 = (uint64_t)cur->sid;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_TCGETPGRP) {
                int vt = (int)f->r9;
                if (vt < 0 || vt >= CONSOLE_VT_COUNT) {
                    f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                    return;
                }
                f->r8 = (uint64_t)console_fg_pgid[vt];
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_TCSETPGRP) {
                int vt = (int)f->r9;
                if (vt < 0 || vt >= CONSOLE_VT_COUNT) {
                    f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                    return;
                }
                console_fg_pgid[vt] = (tid_t)f->r10;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_PORT_IO) {
                if (cur->tid != console_driver_tid) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                uint16_t port = (uint16_t)f->r9;
                int width = (int)f->r10;
                int is_write = (int)f->r12;
                uint64_t value = f->r13;
                int64_t rc = arch_console_port_io(port, width, is_write, value);
                if (rc < 0) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                f->r8 = (uint64_t)rc;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_CONSOLE_FEED) {
                if (cur->tid != console_driver_tid) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                int vt = (int)(f->r9 >> 8);
                uint64_t len = f->r9 & 0xFF;
                if (len > 32) {
                    len = 32;
                }
                uint64_t words[4] = { f->r10, f->r12, f->r13, f->r14 };
                arch_console_feed_bytes(vt, (const uint8_t *)words, (int)len);
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_CONSOLE_SIGNAL_FG) {
                if (cur->tid != console_driver_tid) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                ipc_console_interrupt((int)f->r9, (int)f->r10);
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_SET_ACTIVE_VT) {
                if (cur->tid != console_driver_tid) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                arch_console_switch_vt((int)f->r9);
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_CONSOLE_SCROLL) {
                if (cur->tid != console_driver_tid) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                arch_console_scroll((int)(int64_t)f->r9);
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_MOUSE_FEED) {
                if (cur->tid != console_driver_tid) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                arch_console_mouse_feed(f->r9);
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_MOUSE_READ) {
                if (cur->tid != console_writer_tid) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                uint64_t events[4] = {0, 0, 0, 0};
                int n = arch_console_mouse_read(events, 4);
                f->r8 = (uint64_t)n;
                f->r9 = events[0];
                f->r10 = events[1];
                f->r12 = events[2];
                f->r13 = events[3];
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_FB_MAP) {
                if (!g_boot_fb_present) {
                    f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                    return;
                }
                if (cur->address_space == 0) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                uint64_t fb_size = (uint64_t)g_boot_fb.pitch * g_boot_fb.height;
                uint64_t page_count = (fb_size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
                for (uint64_t i = 0; i < page_count; i++) {
                    arch_vm_map_framebuffer_page(cur->address_space,
                                                 FRAMEBUFFER_USER_VA + i * PAGE_SIZE_4K,
                                                 g_boot_fb.phys_addr + i * PAGE_SIZE_4K);
                }
                f->r8 = (uint64_t)g_boot_fb.width;
                f->r9 = (uint64_t)g_boot_fb.height;
                f->r10 = (uint64_t)g_boot_fb.pitch;
                f->r12 = (uint64_t)g_boot_fb.bpp
                       | ((uint64_t)g_boot_fb.red_pos    << 8)
                       | ((uint64_t)g_boot_fb.red_size   << 16)
                       | ((uint64_t)g_boot_fb.green_pos  << 24)
                       | ((uint64_t)g_boot_fb.green_size << 32)
                       | ((uint64_t)g_boot_fb.blue_pos   << 40)
                       | ((uint64_t)g_boot_fb.blue_size  << 48);
                f->r13 = (uint64_t)g_boot_fb.type;
                arch_console_set_fb_owner(cur->tid);
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_SHM_GET) {
                int id = -1;
                int rc = shm_get(cur->tid, (int)(int64_t)f->r9, f->r10, (uint32_t)f->r12, &id);
                if (rc != IPC_ERR_NONE) {
                    f->rax = (uint64_t)rc;
                    return;
                }
                f->r8 = (uint64_t)(int64_t)id;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_SHM_AT) {
                if (cur->address_space == 0) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                if (f->r10 != 0) {
                    f->rax = (uint64_t)IPC_ERR_INVALID;
                    return;
                }
                uint64_t va = 0;
                int rc = shm_at(cur->tid, cur->address_space, (int)(int64_t)f->r9,
                                (uint32_t)f->r12, &va);
                if (rc != IPC_ERR_NONE) {
                    f->rax = (uint64_t)rc;
                    return;
                }
                f->r8 = va;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_SHM_DT) {
                if (cur->address_space == 0) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                f->rax = (uint64_t)shm_dt(cur->address_space, f->r9);
            } else if (category == SYS_INFO_CAT_SHM_CTL) {
                uint64_t out_words[6] = {0, 0, 0, 0, 0, 0};
                int rc = shm_ctl((int)(int64_t)f->r9, (int)(int64_t)f->r10, out_words);
                if (rc != IPC_ERR_NONE) {
                    f->rax = (uint64_t)rc;
                    return;
                }
                f->r8 = out_words[0];
                f->r9 = out_words[1];
                f->r10 = out_words[2];
                f->r12 = out_words[3];
                f->r13 = out_words[4];
                f->r14 = out_words[5];
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_SOCK_CREATE) {
                int id = -1;
                int rc = sock_create(cur->tid, (int)f->r9, (int)f->r10, &id);
                if (rc != IPC_ERR_NONE) {
                    f->rax = (uint64_t)rc;
                    return;
                }
                f->r8 = (uint64_t)(int64_t)id;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_SOCK_BIND) {
                char path[SOCK_PATH_MAX];
                uint64_t words[4] = { f->r10, f->r12, f->r13, f->r14 };
                for (int wi = 0; wi < 4; wi++) {
                    for (int bi = 0; bi < 8; bi++) {
                        path[wi * 8 + bi] = (char)(uint8_t)(words[wi] >> (8 * bi));
                    }
                }
                path[SOCK_PATH_MAX - 1] = '\0';
                f->rax = (uint64_t)sock_bind(cur->tid, (int)(int64_t)f->r9, path);
            } else if (category == SYS_INFO_CAT_SOCK_LISTEN) {
                f->rax = (uint64_t)sock_listen(cur->tid, (int)(int64_t)f->r9, (int)f->r10);
            } else if (category == SYS_INFO_CAT_SOCK_CONNECT) {
                char path[SOCK_PATH_MAX];
                uint64_t words[4] = { f->r10, f->r12, f->r13, f->r14 };
                for (int wi = 0; wi < 4; wi++) {
                    for (int bi = 0; bi < 8; bi++) {
                        path[wi * 8 + bi] = (char)(uint8_t)(words[wi] >> (8 * bi));
                    }
                }
                path[SOCK_PATH_MAX - 1] = '\0';
                f->rax = (uint64_t)sock_connect(cur->tid, (int)(int64_t)f->r9, path);
            } else if (category == SYS_INFO_CAT_SOCK_ACCEPT) {
                int new_id = -1;
                int rc = sock_accept(cur->tid, (int)(int64_t)f->r9, &new_id);
                if (rc != IPC_ERR_NONE) {
                    f->rax = (uint64_t)rc;
                    return;
                }
                f->r8 = (uint64_t)(int64_t)new_id;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_SOCK_READ) {
                uint8_t buf[40] = {0};
                int max = (int)f->r10;
                if (max > 40) {
                    max = 40;
                }
                int n = 0;
                int rc = sock_read((int)(int64_t)f->r9, buf, max, &n);
                if (rc != IPC_ERR_NONE) {
                    f->rax = (uint64_t)rc;
                    return;
                }
                uint64_t words[5] = {0, 0, 0, 0, 0};
                for (int i = 0; i < n; i++) {
                    words[i / 8] |= ((uint64_t)buf[i]) << (8 * (i % 8));
                }
                f->r8 = (uint64_t)n;
                f->r9 = words[0];
                f->r10 = words[1];
                f->r12 = words[2];
                f->r13 = words[3];
                f->r14 = words[4];
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_SOCK_WRITE) {
                uint8_t buf[24];
                int len = (int)f->r10;
                if (len > 24) {
                    len = 24;
                }
                uint64_t words[3] = { f->r12, f->r13, f->r14 };
                for (int i = 0; i < len; i++) {
                    buf[i] = (uint8_t)(words[i / 8] >> (8 * (i % 8)));
                }
                int n = 0;
                int rc = sock_write((int)(int64_t)f->r9, buf, len, &n);
                if (rc != IPC_ERR_NONE) {
                    f->rax = (uint64_t)rc;
                    return;
                }
                f->r8 = (uint64_t)n;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_SOCK_CLOSE) {
                f->rax = (uint64_t)sock_close(cur->tid, (int)(int64_t)f->r9);
            } else if (category == SYS_INFO_CAT_PCI_CFG) {
                if (!hw_driver_allows(cur->tid) && cur->tid != pci_cfg_secondary_tid) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                uint8_t bus = (uint8_t)(f->r9 >> 16);
                uint8_t device = (uint8_t)(f->r9 >> 8);
                uint8_t function = (uint8_t)f->r9;
                uint8_t offset = (uint8_t)f->r10;
                int width = (int)(f->r10 >> 8) & 0xFF;
                int is_write = (int)(f->r10 >> 16) & 0xFF;
                uint64_t value = f->r12;
                int64_t rc = arch_pci_cfg_access(bus, device, function, offset, width, is_write, value);
                if (rc < 0) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                f->r8 = (uint64_t)rc;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_HW_PORT_IO) {
                if (!hw_driver_allows(cur->tid) && !hw_port_io_secondary_allows(cur->tid)) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                uint16_t port = (uint16_t)f->r9;
                int width = (int)f->r10;
                int is_write = (int)f->r12;
                uint64_t value = f->r13;
                int64_t rc = arch_hw_port_io(port, width, is_write, value);
                if (rc < 0) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                f->r8 = (uint64_t)rc;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_HW_PORT_IO_BULK_READ) {
                if (!hw_driver_allows(cur->tid) && !hw_port_io_secondary_allows(cur->tid)) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                uint16_t port = (uint16_t)f->r9;
                int count = (int)f->r10;
                if (count > HW_PORT_IO_BULK_READ_MAX) {
                    count = HW_PORT_IO_BULK_READ_MAX;
                }
                uint8_t buf[HW_PORT_IO_BULK_READ_MAX];
                int64_t rc = arch_hw_port_io_bulk_read(port, buf, count);
                if (rc < 0) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                uint64_t regs[5] = {0, 0, 0, 0, 0};
                for (int i = 0; i < count; i++) {
                    ((uint8_t *)regs)[i] = buf[i];
                }
                f->r8 = (uint64_t)rc;
                f->r9 = regs[0];
                f->r10 = regs[1];
                f->r12 = regs[2];
                f->r13 = regs[3];
                f->r14 = regs[4];
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_HW_PORT_IO_BULK_WRITE) {
                if (!hw_driver_allows(cur->tid) && !hw_port_io_secondary_allows(cur->tid)) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                uint16_t port = (uint16_t)f->r9;
                int count = (int)f->r10;
                if (count > HW_PORT_IO_BULK_WRITE_MAX) {
                    count = HW_PORT_IO_BULK_WRITE_MAX;
                }
                uint64_t regs[3] = {f->r12, f->r13, f->r14};
                uint8_t buf[HW_PORT_IO_BULK_WRITE_MAX];
                for (int i = 0; i < count; i++) {
                    buf[i] = ((uint8_t *)regs)[i];
                }
                int64_t rc = arch_hw_port_io_bulk_write(port, buf, count);
                if (rc < 0) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                f->r8 = (uint64_t)rc;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_DMA_ALLOC) {
                if (!hw_driver_allows(cur->tid) || cur->address_space == 0) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                uint64_t bytes = (f->r9 + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
                paddr_t phys = dma_region_alloc(bytes);
                if (!phys) {
                    f->rax = (uint64_t)IPC_ERR_NO_MEM;
                    return;
                }
                memset((void *)phys, 0, bytes);
                paddr_t dbase;
                uint64_t dsize;
                dma_range(&dbase, &dsize);
                uint64_t page_count = bytes / PAGE_SIZE_4K;
                uint64_t va_base = DMA_USER_VA_BASE + (uint64_t)(phys - dbase);
                for (uint64_t i = 0; i < page_count; i++) {
                    arch_vm_map_page(cur->address_space, va_base + i * PAGE_SIZE_4K,
                                      phys + i * PAGE_SIZE_4K,
                                      VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER);
                }
                f->r8 = va_base;
                f->r9 = (uint64_t)phys;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_HW_MMIO_MAP) {
                if (!hw_driver_allows(cur->tid) || cur->address_space == 0) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                paddr_t phys = (paddr_t)f->r9;
                uint64_t bytes = f->r10;
                uint64_t offset = phys & (PAGE_SIZE_4K - 1);
                paddr_t base = phys & ~(PAGE_SIZE_4K - 1);
                bytes = (bytes + offset + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
                if (bytes == 0 || bytes > HW_MMIO_MAP_MAX) {
                    f->rax = (uint64_t)IPC_ERR_INVALID;
                    return;
                }
                for (uint64_t i = 0; i < bytes; i += PAGE_SIZE_4K) {
                    arch_vm_map_device_page(cur->address_space, HW_MMIO_USER_VA_BASE + i,
                                            base + i);
                }
                f->r8 = HW_MMIO_USER_VA_BASE + offset;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_EXECREQ_REPLY) {
                if (cur->tid != (tid_t)kinfo_user()->ext2fs_tid) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                uint32_t slot = (uint32_t)f->r9;
                uint16_t generation = (uint16_t)f->r10;
                int status = (int)(int64_t)f->r12;
                int shmid = (int)(int64_t)f->r13;
                uint64_t size = f->r14;
                execreq_complete(slot, generation, status, shmid, size);
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_GETENTROPY) {
                uint64_t user_addr = f->r9;
                uint64_t length = f->r10;
                if (cur->address_space == 0) {
                    f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    return;
                }
                if (length > ROBU_RANDOM_MAX_REQUEST || (length != 0 && user_addr == 0)) {
                    f->rax = (uint64_t)IPC_ERR_INVALID;
                    return;
                }
                uint8_t random_bytes[ROBU_RANDOM_MAX_REQUEST];
                if (random_fill(random_bytes, length) != 0) {
                    f->rax = (uint64_t)IPC_ERR_NOT_SUPPORTED;
                    return;
                }
                if (vm_copy_to_user(cur->address_space, user_addr, random_bytes, length) != 0) {
                    f->rax = (uint64_t)IPC_ERR_INVALID;
                    return;
                }
                f->r8 = length;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_BLOCK_INFO) {
                uint64_t op = f->r9;
                if (op == BLOCK_INFO_OP_PUBLISH) {
                    uint32_t device = (uint32_t)f->r10;
                    uint32_t transport = (uint32_t)(f->r12 >> 32);
                    uint32_t filesystem = (uint32_t)f->r12;
                    uint64_t sectors = f->r13;
                    uint32_t sector_size = (uint32_t)f->r14;
                    if ((cur->tid == (tid_t)kinfo_user()->ext2fs_tid &&
                         device == BLOCK_DEVICE_ROOT) ||
                        (cur->tid == (tid_t)kinfo_user()->blockdrv_tid &&
                         device == BLOCK_DEVICE_DATA)) {
                        int rc = kinfo_block_info_publish(device, transport, filesystem,
                                                          sector_size, sectors);
                        f->rax = (uint64_t)(rc == 0 ? IPC_ERR_NONE : IPC_ERR_INVALID);
                    } else {
                        f->rax = (uint64_t)IPC_ERR_NO_CAP;
                    }
                    return;
                }
                if (op == BLOCK_INFO_OP_QUERY) {
                    block_device_info_t info;
                    if (kinfo_block_info_query((uint32_t)f->r10, &info) != 0) {
                        f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                        return;
                    }
                    f->r8 = info.device;
                    f->r9 = ((uint64_t)info.transport << 32) | info.filesystem;
                    f->r10 = info.sectors;
                    f->r12 = info.sector_size;
                    f->rax = (uint64_t)IPC_ERR_NONE;
                    return;
                }
                f->rax = (uint64_t)IPC_ERR_INVALID;
            } else {
                f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
            }
            return;
        }
        f->rax = (uint64_t)IPC_ERR_NONE;
        if (src_filter == 0) {
            sched_yield();
        } else {
            sched_sleep(src_filter);
        }
        return;
    }
    if (dest_tid != 0) {
        TRACE(TRACE_EVT_IPC_SEND, dest_tid, cur->tid, flags, 0, 0, 0);
        tcb_t *dest = sched_get_tcb(dest_tid);
        if (!dest || dest == cur) {
            f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
            return;
        }
        if (dest->state == THREAD_STATE_WAIT_PAGEFAULT &&
            cur->tid == dest->pager_tid) {
            sched_wake(dest);
            f->rax = (uint64_t)IPC_ERR_NONE;
            if (!want_recv) {
                return;
            }
        } else if (dest->state == THREAD_STATE_WAIT_RECV &&
                   (dest->ipc_partner == IPC_TID_ANY ||
                    dest->ipc_partner == cur->tid)) {
            msg_regs_t m;
            payload_from_frame(&m, f);
            int xfer_rc = 0;
            if (flags & IPC_FLAG_MAP) {
                xfer_rc = vm_batch_map(dest->address_space, cur->tid, (vm_map_msg_t *)&m);
            } else if (flags & IPC_FLAG_XFER) {
                xfer_rc = vm_xfer_pages(cur->address_space, dest->address_space,
                                        (vm_xfer_msg_t *)&m);
            } else if (flags & IPC_FLAG_SHARE) {
                xfer_rc = vm_share_pages(cur->address_space, dest->address_space,
                                         (vm_xfer_msg_t *)&m);
            }
            deliver_to_frame(dest, cur->tid, &m);
            sched_stats.ipc_msgs++;
            TRACE(TRACE_EVT_IPC_RECV, dest->tid, cur->tid, flags, 0, 0, 0);
            f->rax = (uint64_t)(xfer_rc == 0 ? IPC_ERR_NONE : IPC_ERR_NO_CAP);
            if (want_recv) {
                tcb_t *waiting = sendq_take(cur, src_filter);
                if (waiting) {
                    sched_wake(dest);
                    complete_parked_sender(cur, f, waiting);
                    return;
                }
                cur->ipc_partner = src_filter;
                sched_block(THREAD_STATE_WAIT_RECV);
                dest->timeslice_left = cur->timeslice_left;
                sched_direct_switch(dest);
            } else if (dest->prio >= cur->prio) {
                sched_ready_now(cur);
                dest->timeslice_left = cur->timeslice_left;
                sched_direct_switch(dest);
            } else {
                sched_wake(dest);
            }
            return;
        } else {
            payload_from_frame(&cur->ipc_buffer, f);
            cur->ipc_flags = flags;
            cur->ipc_partner = dest_tid;
            cur->ipc_recv_after = want_recv ? src_filter : (tid_t)~0u;
            sendq_append(dest, cur);
            sched_block(THREAD_STATE_WAIT_SEND);
            return;
        }
    }
    {
        tcb_t *s = sendq_take(cur, src_filter);
        if (s) {
            complete_parked_sender(cur, f, s);
            return;
        }
        vm_fault_msg_t fault;
        if (vm_take_queued_fault(cur, &fault)) {
            f->r8 = fault.fault_address;
            f->r9 = fault.error_code;
            f->r10 = fault.faulting_thread;
            f->rsi = fault.faulting_thread;
            f->rax = (uint64_t)IPC_ERR_NONE;
            return;
        }
        if (cur->tid == (tid_t)kinfo_user()->ext2fs_tid) {
            uint32_t slot;
            uint16_t generation;
            char name[25] = {0};
            if (execreq_try_deliver_queued(cur, &slot, &generation, name, sizeof(name))) {
                uint64_t words[3] = {0, 0, 0};
                memcpy(words, name, 24);
                f->r8 = (uint64_t)VFS_OP_READ_BULK;
                f->r9 = (uint64_t)slot;
                f->r10 = (uint64_t)generation;
                f->r12 = words[0];
                f->r13 = words[1];
                f->r14 = words[2];
                f->rsi = 0;
                f->rax = (uint64_t)IPC_ERR_NONE;
                return;
            }
        }
        if (flags & IPC_FLAG_NOBLOCK) {
            f->rax = (uint64_t)IPC_ERR_WOULDBLOCK;
            return;
        }
        cur->ipc_partner = src_filter;
        sched_block(THREAD_STATE_WAIT_RECV);
    }
}
