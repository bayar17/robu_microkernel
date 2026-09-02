#ifndef ROBU_VM_H
#define ROBU_VM_H
#include "robu/types.h"
#define VM_PROT_READ    (1 << 0)
#define VM_PROT_WRITE   (1 << 1)
#define VM_PROT_EXEC    (1 << 2)
#define VM_PROT_USER    (1 << 3)
#define PAGE_SIZE_4K    0x1000ULL
#define PAGE_SIZE_2M    0x200000ULL
#define VM_MAP_LARGE    (1 << 4)
#define FAULT_QUEUE_SIZE 16
typedef struct {
    vaddr_t fault_address;
    uint32_t error_code;
    tid_t faulting_thread;
    uint32_t reserved;
} vm_fault_msg_t;
typedef struct {
    vm_fault_msg_t entries[FAULT_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
} vm_fault_queue_t;
int fault_queue_push(vm_fault_queue_t *q, vm_fault_msg_t *msg);
int fault_queue_pop(vm_fault_queue_t *q, vm_fault_msg_t *out);
int fault_queue_empty(vm_fault_queue_t *q);
struct tcb;
int vm_take_queued_fault(struct tcb *rcv, vm_fault_msg_t *out);
#define VM_BATCH_MAX 32
typedef struct {
    vaddr_t vaddr;
    paddr_t paddr;
    uint32_t flags;
    uint32_t count;
} vm_map_msg_t;
int vm_batch_map(paddr_t aspace, tid_t requester, vm_map_msg_t *req);
typedef struct {
    vaddr_t src_vaddr;
    vaddr_t dst_vaddr;
    uint32_t flags;
    uint32_t count;
} vm_xfer_msg_t;
int vm_xfer_pages(paddr_t src_aspace, paddr_t dst_aspace, vm_xfer_msg_t *req);
int vm_share_pages(paddr_t src_aspace, paddr_t dst_aspace, vm_xfer_msg_t *req);
#define CAP_PERM_MAP       (1 << 0)
#define CAP_PERM_GRANT     (1 << 1)
#define CAP_PERM_REVOKE    (1 << 2)
typedef struct {
    paddr_t base_frame;
    uint64_t frame_count;
    uint32_t permissions;
    tid_t owner;
} frame_cap_t;
#define MAX_FRAME_CAPS 64
int vm_cap_grant(tid_t owner, paddr_t base, uint64_t count, uint32_t perms);
int vm_cap_check(tid_t owner, paddr_t paddr, uint32_t perms);
paddr_t arch_vm_create_address_space(void);
int arch_vm_map_page(paddr_t aspace, vaddr_t vaddr, paddr_t paddr, uint32_t flags);
int arch_vm_map_framebuffer_page(paddr_t aspace, vaddr_t vaddr, paddr_t paddr);
int arch_vm_map_device_page(paddr_t aspace, vaddr_t vaddr, paddr_t paddr);
int arch_vm_map_large_page(paddr_t aspace, vaddr_t vaddr, paddr_t paddr, uint32_t flags);
void arch_vm_enable_framebuffer_write_combining(void);
void arch_vm_destroy_address_space(paddr_t aspace);
paddr_t arch_vm_clone_address_space(paddr_t src);
paddr_t arch_vm_unmap_page(paddr_t aspace, vaddr_t vaddr);
paddr_t arch_vm_translate(paddr_t aspace, vaddr_t vaddr);
void vm_init(void);
void vm_handle_page_fault(vaddr_t fault_addr, uint32_t error_code);
paddr_t vm_address_space_create(void);
void vm_address_space_destroy(paddr_t aspace);
paddr_t vm_address_space_clone(paddr_t src);
int vm_copy_from_user(paddr_t as, vaddr_t src, void *dst, uint64_t len);
int vm_copy_to_user(paddr_t as, vaddr_t dst, const void *src, uint64_t len);
#endif
