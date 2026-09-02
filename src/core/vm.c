#include "robu/vm.h"
#include "robu/tcb.h"
#include "robu/ipc.h"
#include "robu/sched.h"
#include "robu/kprintf.h"
#include "robu/trace.h"
#include "robu/arch.h"
extern paddr_t boot_pml4;
static paddr_t normalize_aspace(paddr_t aspace) {
    return aspace ? aspace : (paddr_t)&boot_pml4;
}
static vm_fault_queue_t global_fault_queue;
int fault_queue_push(vm_fault_queue_t *q, vm_fault_msg_t *msg) {
    uint32_t next_head = (q->head + 1) & (FAULT_QUEUE_SIZE - 1);
    if (next_head == q->tail) {
        return -1;
    }
    q->entries[q->head] = *msg;
    q->head = next_head;
    return 0;
}
int fault_queue_pop(vm_fault_queue_t *q, vm_fault_msg_t *out) {
    if (q->head == q->tail) {
        return -1;
    }
    *out = q->entries[q->tail];
    q->tail = (q->tail + 1) & (FAULT_QUEUE_SIZE - 1);
    return 0;
}
int fault_queue_empty(vm_fault_queue_t *q) {
    return q->head == q->tail;
}
static frame_cap_t cap_table[MAX_FRAME_CAPS];
static uint32_t cap_count = 0;
int vm_cap_grant(tid_t owner, paddr_t base, uint64_t count, uint32_t perms) {
    if (cap_count >= MAX_FRAME_CAPS) {
        return -1;
    }
    frame_cap_t *cap = &cap_table[cap_count++];
    cap->base_frame = base;
    cap->frame_count = count;
    cap->permissions = perms;
    cap->owner = owner;
    return 0;
}
int vm_cap_check(tid_t owner, paddr_t paddr, uint32_t perms) {
    for (uint32_t i = 0; i < cap_count; i++) {
        frame_cap_t *cap = &cap_table[i];
        if (cap->owner != owner) continue;
        if ((cap->permissions & perms) != perms) continue;
        paddr_t cap_end = cap->base_frame + (cap->frame_count * PAGE_SIZE_4K);
        if (paddr >= cap->base_frame && paddr < cap_end) {
            return 0;
        }
    }
    return -1;
}
int vm_batch_map(paddr_t aspace, tid_t requester, vm_map_msg_t *req) {
    if (!req || req->count == 0 || req->count > VM_BATCH_MAX) {
        return -1;
    }
    if (vm_cap_check(requester, req->paddr, CAP_PERM_MAP) != 0) {
        return -1;
    }
    paddr_t real_aspace = normalize_aspace(aspace);
    uint64_t page_size = (req->flags & VM_MAP_LARGE) ? PAGE_SIZE_2M : PAGE_SIZE_4K;
    for (uint32_t i = 0; i < req->count; i++) {
        vaddr_t va = req->vaddr + (i * page_size);
        paddr_t pa = req->paddr + (i * page_size);
        if (req->flags & VM_MAP_LARGE) {
            arch_vm_map_large_page(real_aspace, va, pa, req->flags);
        } else {
            arch_vm_map_page(real_aspace, va, pa, req->flags);
        }
    }
    return 0;
}
int vm_xfer_pages(paddr_t src_aspace, paddr_t dst_aspace, vm_xfer_msg_t *req) {
    if (!req || req->count == 0 || req->count > VM_BATCH_MAX) {
        return -1;
    }
    paddr_t real_src = normalize_aspace(src_aspace);
    paddr_t real_dst = normalize_aspace(dst_aspace);
    paddr_t frames[VM_BATCH_MAX];
    for (uint32_t i = 0; i < req->count; i++) {
        vaddr_t src_va = req->src_vaddr + ((uint64_t)i * PAGE_SIZE_4K);
        frames[i] = arch_vm_unmap_page(real_src, src_va);
        if (!frames[i]) {
            for (uint32_t j = 0; j < i; j++) {
                arch_vm_map_page(real_src, req->src_vaddr + ((uint64_t)j * PAGE_SIZE_4K),
                                  frames[j], req->flags);
            }
            return -1;
        }
    }
    for (uint32_t i = 0; i < req->count; i++) {
        vaddr_t dst_va = req->dst_vaddr + ((uint64_t)i * PAGE_SIZE_4K);
        arch_vm_map_page(real_dst, dst_va, frames[i], req->flags);
    }
    return 0;
}
int vm_share_pages(paddr_t src_aspace, paddr_t dst_aspace, vm_xfer_msg_t *req) {
    if (!req || req->count == 0 || req->count > VM_BATCH_MAX) {
        return -1;
    }
    paddr_t real_src = normalize_aspace(src_aspace);
    paddr_t real_dst = normalize_aspace(dst_aspace);
    for (uint32_t i = 0; i < req->count; i++) {
        vaddr_t src_va = req->src_vaddr + ((uint64_t)i * PAGE_SIZE_4K);
        vaddr_t dst_va = req->dst_vaddr + ((uint64_t)i * PAGE_SIZE_4K);
        paddr_t frame = arch_vm_translate(real_src, src_va);
        if (!frame) {
            return -1;
        }
        arch_vm_map_page(real_dst, dst_va, frame, req->flags);
    }
    return 0;
}
paddr_t vm_address_space_create(void) {
    return arch_vm_create_address_space();
}
void vm_address_space_destroy(paddr_t aspace) {
    arch_vm_destroy_address_space(aspace);
}
paddr_t vm_address_space_clone(paddr_t src) {
    return arch_vm_clone_address_space(src);
}
void vm_init(void) {
    global_fault_queue.head = 0;
    global_fault_queue.tail = 0;
    cap_count = 0;
}
int vm_copy_from_user(paddr_t as, vaddr_t src, void *dst, uint64_t len) {
    if (len != 0 && src + len < src) {
        return -1;
    }
    uint8_t *out = (uint8_t *)dst;
    uint64_t copied = 0;
    while (copied < len) {
        vaddr_t va = src + copied;
        vaddr_t page_va = va & ~(PAGE_SIZE_4K - 1);
        paddr_t frame = arch_vm_translate(as, page_va);
        if (!frame) {
            return -1;
        }
        uint64_t page_off = va - page_va;
        uint64_t chunk = PAGE_SIZE_4K - page_off;
        if (chunk > len - copied) {
            chunk = len - copied;
        }
        memcpy(out + copied, (const void *)(frame + page_off), chunk);
        copied += chunk;
    }
    return 0;
}
int vm_copy_to_user(paddr_t as, vaddr_t dst, const void *src, uint64_t len) {
    if (len != 0 && dst + len < dst) {
        return -1;
    }
    const uint8_t *in = (const uint8_t *)src;
    uint64_t copied = 0;
    while (copied < len) {
        vaddr_t va = dst + copied;
        vaddr_t page_va = va & ~(PAGE_SIZE_4K - 1);
        paddr_t frame = arch_vm_translate(as, page_va);
        if (!frame) {
            return -1;
        }
        uint64_t page_off = va - page_va;
        uint64_t chunk = PAGE_SIZE_4K - page_off;
        if (chunk > len - copied) {
            chunk = len - copied;
        }
        memcpy((void *)(frame + page_off), in + copied, chunk);
        copied += chunk;
    }
    return 0;
}
void vm_handle_page_fault(vaddr_t fault_addr, uint32_t error_code) {
    tcb_t *cur = current_thread;
    TRACE(TRACE_EVT_PAGE_FAULT, cur ? cur->tid : 0, fault_addr, error_code, 0, 0, 0);
    if (!cur || cur->pager_tid == 0) {
        kprintf("\n[vm] unhandled #PF addr=0x%lx err=0x%x tid=%u '%s' rip=0x%lx\n",
                fault_addr, error_code, cur ? cur->tid : 0,
                cur ? cur->name : "?", cur ? cur->uctx.rip : 0);
        if (cur && cur->address_space != 0) {
            kputs("[vm] terminating the faulting service, kernel continues");
            sched_terminate_current();
            return;
        }
        for (;;) { asm volatile("cli; hlt"); }
    }
    tcb_t *pager = sched_get_tcb(cur->pager_tid);
    if (!pager) {
        kprintf("\n[vm] tid=%u faulted but pager tid=%u is gone\n",
                cur->tid, cur->pager_tid);
        if (cur->address_space != 0) {
            kputs("[vm] terminating the orphaned service, kernel continues");
            sched_terminate_current();
            return;
        }
        for (;;) { asm volatile("cli; hlt"); }
    }
    vm_fault_msg_t fault;
    fault.fault_address = fault_addr;
    fault.error_code = error_code;
    fault.faulting_thread = cur->tid;
    fault.reserved = 0;
    sched_block(THREAD_STATE_WAIT_PAGEFAULT);
    if (pager->state == THREAD_STATE_WAIT_RECV &&
        (pager->ipc_partner == 0 || pager->ipc_partner == cur->tid)) {
        arch_uctx_t *pf = &pager->uctx;
        pf->r8 = fault.fault_address;
        pf->r9 = fault.error_code;
        pf->r10 = fault.faulting_thread;
        pf->rsi = fault.faulting_thread;
        pf->rax = 0;
        pager->timeslice_left = cur->timeslice_left;
        sched_direct_switch(pager);
        return;
    }
    if (fault_queue_push(&global_fault_queue, &fault) != 0) {
        kprintf("\n[vm] fatal: fault queue overflow (pager tid=%u wedged?)\n",
                pager->tid);
        for (;;) { asm volatile("cli; hlt"); }
    }
}
int vm_take_queued_fault(struct tcb *rcv, vm_fault_msg_t *out) {
    if (fault_queue_empty(&global_fault_queue)) {
        return 0;
    }
    vm_fault_msg_t *head = &global_fault_queue.entries[global_fault_queue.tail];
    tcb_t *faulter = sched_get_tcb(head->faulting_thread);
    if (!faulter || faulter->pager_tid != rcv->tid) {
        return 0;
    }
    return fault_queue_pop(&global_fault_queue, out) == 0;
}
