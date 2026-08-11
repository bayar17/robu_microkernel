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
#include "robu/virtio_blk.h"
#include "robu/arch.h"
#include "robu/signal.h"
static tid_t console_writer_tid = 0;
void ipc_grant_console_writer(tid_t tid) {
    console_writer_tid = tid;
}
static tid_t console_fg_pgid = 0;
static tid_t blk_owner_tid = 0;
void ipc_grant_blk_owner(tid_t tid) {
    blk_owner_tid = tid;
}
static tid_t pager_owner_tid = 0;
void ipc_grant_pager_owner(tid_t tid) {
    pager_owner_tid = tid;
}

#define BLK_STAGING_SIZE 4096
static uint8_t blk_staging[BLK_STAGING_SIZE];
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
            if (cur->tid != console_writer_tid) {
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
            uint64_t words[5] = {0, 0, 0, 0, 0};
            int n = arch_console_read_line_bytes(vt, (uint8_t *)words, 40);
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
            f->rax = (uint64_t)rc;
            f->r8 = child_tid;
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
            if (rc != IPC_ERR_NONE) {

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
        if (flags & IPC_FLAG_BLK_IO) {

            if (cur->tid != blk_owner_tid || blk_owner_tid == 0) {
                f->rax = (uint64_t)IPC_ERR_NO_CAP;
                return;
            }
            uint64_t category = f->r8;
            if (category == 0) {

                uint64_t sector = f->r9;
                uint64_t count = f->r10;
                if (count == 0 || count > BLK_STAGING_SIZE / 512) {
                    count = BLK_STAGING_SIZE / 512;
                }
                int rc = virtio_blk_read(sector, (uint32_t)count, blk_staging);
                f->rax = (uint64_t)(rc == 0 ? IPC_ERR_NONE : IPC_ERR_TIMEOUT);
                return;
            }
            if (category == 1) {

                uint64_t offset = f->r9;
                if (offset >= BLK_STAGING_SIZE) {
                    f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                    return;
                }
                uint64_t n = BLK_STAGING_SIZE - offset;
                if (n > 40) {
                    n = 40;
                }
                uint8_t buf[40] = {0};
                memcpy(buf, blk_staging + offset, n);
                uint64_t words[5];
                memcpy(words, buf, 40);
                f->rax = (uint64_t)n;
                f->r8 = words[0];
                f->r9 = words[1];
                f->r10 = words[2];
                f->r12 = words[3];
                f->r13 = words[4];
                return;
            }
            if (category == 2) {

                uint64_t offset = f->r9;
                uint64_t len = f->r10;
                if (len > 24) {
                    len = 24;
                }
                if (offset >= BLK_STAGING_SIZE || offset + len > BLK_STAGING_SIZE) {
                    f->rax = (uint64_t)IPC_ERR_NOT_FOUND;
                    return;
                }
                uint64_t words[3] = { f->r12, f->r13, f->r14 };
                memcpy(blk_staging + offset, words, len);
                f->rax = (uint64_t)IPC_ERR_NONE;
                return;
            }
            if (category == 3) {

                uint64_t sector = f->r9;
                uint64_t count = f->r10;
                if (count == 0 || count > BLK_STAGING_SIZE / 512) {
                    count = BLK_STAGING_SIZE / 512;
                }
                int rc = virtio_blk_write(sector, (uint32_t)count, blk_staging);
                f->rax = (uint64_t)(rc == 0 ? IPC_ERR_NONE : IPC_ERR_TIMEOUT);
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
                f->r8 = (uint64_t)console_fg_pgid;
                f->rax = (uint64_t)IPC_ERR_NONE;
            } else if (category == SYS_INFO_CAT_TCSETPGRP) {
                console_fg_pgid = (tid_t)f->r9;
                f->rax = (uint64_t)IPC_ERR_NONE;
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
        if (flags & IPC_FLAG_NOBLOCK) {
            f->rax = (uint64_t)IPC_ERR_WOULDBLOCK;
            return;
        }
        cur->ipc_partner = src_filter;
        sched_block(THREAD_STATE_WAIT_RECV);
    }
}
