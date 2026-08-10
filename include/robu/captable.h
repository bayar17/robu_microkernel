#ifndef ROBU_CAPTABLE_H
#define ROBU_CAPTABLE_H
#include "robu/types.h"

typedef enum {
    CAP_KIND_UNTYPED = 0,
    CAP_KIND_FRAME,
    CAP_KIND_ADDRSPACE,
    CAP_KIND_TCB,
    CAP_KIND_NOTIFICATION,
    CAP_KIND_TIMER,
    CAP_KIND_INVALID,
} cap_kind_t;

typedef struct {
    cap_kind_t kind;
    tid_t owner;
    uint64_t addr;
    uint64_t size;
    uint64_t mapped_va;
} kcap_t;

// Removemos a macro limitante MAX_KCAPS 40!
int kcap_grant(tid_t owner, cap_kind_t kind, uint64_t addr, uint64_t size);
uint32_t kcap_next_slot(void);
int cap_retype(tid_t caller, uint32_t untyped_slot, uint32_t kind,
               uint64_t as_slot, uint64_t entry, uint64_t stack, uint64_t frame_slot,
               uint64_t *out_slot, uint64_t *out_addr);
void kcap_invalidate_tcb_death(tid_t tid);
int cap_destroy(tid_t caller, uint32_t slot);
int cap_notif_signal(tid_t caller, uint32_t notif_slot, uint64_t bits);
int cap_notif_wait_begin(tid_t caller, uint32_t notif_slot, uint64_t *out_bits);
int cap_notif_poll(tid_t caller, uint32_t notif_slot, uint64_t *out_bits);
int cap_timer_arm(tid_t caller, uint32_t timer_slot, uint32_t notif_slot,
                  uint64_t ticks_from_now, uint64_t bits, uint64_t period_ticks);
int cap_timer_disarm(tid_t caller, uint32_t timer_slot);
void cap_timer_rescan(uint64_t now_ticks, uint64_t *out_next);
void notif_invalidate_waiter_death(tid_t tid);
int cap_revoke_frame(tid_t caller, uint32_t frame_slot);
#endif
