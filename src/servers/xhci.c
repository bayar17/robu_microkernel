#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "xhci.h"

#define SYS_INFO_CAT_PCI_CFG 37
#define SYS_INFO_CAT_DMA_ALLOC 39
#define SYS_INFO_CAT_HW_MMIO_MAP 43

#define XHCI_MMIO_BYTES (1024U * 1024U)
#define XHCI_PAGE_SIZE 4096U
#define XHCI_RING_TRBS 256U
#define XHCI_DATA_BYTES (64U * 1024U)
#define XHCI_BOT_DATA_OFFSET 64U
#define XHCI_MAX_RW_SECTORS 127U
#define XHCI_FAILURE_NONE 0
#define XHCI_FAILURE_NO_CONTROLLER 1
#define XHCI_FAILURE_PCI 2
#define XHCI_FAILURE_MMIO 3
#define XHCI_FAILURE_OWNERSHIP 4
#define XHCI_FAILURE_HALT 5
#define XHCI_FAILURE_RESET 6
#define XHCI_FAILURE_READY 7
#define XHCI_FAILURE_LAYOUT 8
#define XHCI_FAILURE_DCBAA 9
#define XHCI_FAILURE_SCRATCHPAD_ARRAY 10
#define XHCI_FAILURE_SCRATCHPAD_PAGE 11
#define XHCI_FAILURE_RING 12
#define XHCI_FAILURE_ERST 13
#define XHCI_FAILURE_INPUT_CONTEXT 14
#define XHCI_FAILURE_DEVICE_CONTEXT 15
#define XHCI_FAILURE_DATA 16
#define XHCI_FAILURE_SLOTS 17
#define XHCI_FAILURE_RUN 18
#define XHCI_FAILURE_NO_PORT 19
#define XHCI_FAILURE_PORT_RESET 20
#define XHCI_FAILURE_PORT_DISABLED 21
#define XHCI_FAILURE_ENABLE_SLOT 22
#define XHCI_FAILURE_ADDRESS_DEVICE 23
#define XHCI_FAILURE_DEVICE_DESCRIPTOR 24
#define XHCI_FAILURE_EP0_MPS 25
#define XHCI_FAILURE_EVALUATE_EP0 26
#define XHCI_FAILURE_CONFIG_HEADER 27
#define XHCI_FAILURE_CONFIG_LENGTH 28
#define XHCI_FAILURE_CONFIG_BODY 29
#define XHCI_FAILURE_MASS_STORAGE 30
#define XHCI_FAILURE_SET_CONFIGURATION 31
#define XHCI_FAILURE_CONFIGURE_ENDPOINT 32
#define XHCI_FAILURE_STORAGE 33
#define XHCI_FAILURE_IO 34

#define XHCI_WAIT_HALT_TICKS 200U
#define XHCI_WAIT_RESET_TICKS 200U
#define XHCI_WAIT_READY_TICKS 1000U
#define XHCI_WAIT_OWNERSHIP_TICKS 500U
#define XHCI_WAIT_PORT_TICKS 200U
#define XHCI_WAIT_EVENT_TICKS 500U
#define XHCI_WAIT_SPINS 10000U

#define XHCI_CAP_HCSPARAMS1 0x04
#define XHCI_CAP_HCSPARAMS2 0x08
#define XHCI_CAP_HCCPARAMS1 0x10
#define XHCI_CAP_DBOFF 0x14
#define XHCI_CAP_RTSOFF 0x18

#define XHCI_OP_USBCMD 0x00
#define XHCI_OP_USBSTS 0x04
#define XHCI_OP_CRCR 0x18
#define XHCI_OP_DCBAAP 0x30
#define XHCI_OP_CONFIG 0x38
#define XHCI_OP_PORTSC 0x400

#define XHCI_RT_IR0 0x20
#define XHCI_IR_IMAN 0x00
#define XHCI_IR_ERSTSZ 0x08
#define XHCI_IR_ERSTBA 0x10
#define XHCI_IR_ERDP 0x18

#define XHCI_USBCMD_RUN (1U << 0)
#define XHCI_USBCMD_HCRST (1U << 1)
#define XHCI_USBSTS_HCH (1U << 0)
#define XHCI_USBSTS_CNR (1U << 11)

#define XHCI_PORTSC_CCS (1U << 0)
#define XHCI_PORTSC_PED (1U << 1)
#define XHCI_PORTSC_PR (1U << 4)
#define XHCI_PORTSC_PP (1U << 9)
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_CHANGE_BITS (0x7FU << 17)
#define XHCI_PORTSC_WPR (1U << 31)

#define XHCI_TRB_CYCLE (1U << 0)
#define XHCI_TRB_TC (1U << 1)
#define XHCI_TRB_ENT (1U << 1)
#define XHCI_TRB_ISP (1U << 2)
#define XHCI_TRB_IOC (1U << 5)
#define XHCI_TRB_IDT (1U << 6)
#define XHCI_TRB_DIR_IN (1U << 16)
#define XHCI_TRB_TYPE_SHIFT 10

#define XHCI_TRB_NORMAL 1U
#define XHCI_TRB_SETUP_STAGE 2U
#define XHCI_TRB_DATA_STAGE 3U
#define XHCI_TRB_STATUS_STAGE 4U
#define XHCI_TRB_LINK 6U
#define XHCI_TRB_ENABLE_SLOT_CMD 9U
#define XHCI_TRB_DISABLE_SLOT_CMD 10U
#define XHCI_TRB_ADDRESS_DEVICE_CMD 11U
#define XHCI_TRB_CONFIGURE_ENDPOINT_CMD 12U
#define XHCI_TRB_EVALUATE_CONTEXT_CMD 13U
#define XHCI_TRB_TRANSFER_EVENT 32U
#define XHCI_TRB_COMMAND_COMPLETION_EVENT 33U

#define XHCI_CC_SUCCESS 1U
#define XHCI_SETUP_TRT_NONE 0U
#define XHCI_SETUP_TRT_OUT 2U
#define XHCI_SETUP_TRT_IN 3U

#define USB_REQ_GET_DESCRIPTOR 6U
#define USB_REQ_SET_CONFIGURATION 9U
#define USB_DESC_DEVICE 1U
#define USB_DESC_CONFIG 2U
#define USB_DESC_INTERFACE 4U
#define USB_DESC_ENDPOINT 5U
#define USB_CLASS_MASS_STORAGE 8U
#define USB_SUBCLASS_SCSI 6U
#define USB_PROTOCOL_BULK_ONLY 0x50U

#define SCSI_TEST_UNIT_READY 0x00U
#define SCSI_READ_CAPACITY_10 0x25U
#define SCSI_READ_10 0x28U
#define SCSI_WRITE_10 0x2AU

typedef struct {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} __attribute__((packed)) xhci_trb_t;

typedef struct {
    uint64_t segment_base;
    uint32_t segment_size;
    uint32_t reserved;
} __attribute__((packed)) xhci_erst_entry_t;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t transfer_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cdb_length;
    uint8_t cdb[16];
} __attribute__((packed)) usb_cbw_t;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} __attribute__((packed)) usb_csw_t;

typedef struct {
    volatile xhci_trb_t *va;
    uint64_t pa;
    uint32_t index;
    uint8_t cycle;
} xhci_ring_t;

static volatile uint8_t *g_mmio;
static uint32_t g_op_base;
static uint32_t g_db_base;
static uint32_t g_rt_base;
static uint32_t g_context_size;
static uint8_t g_port;
static uint8_t g_slot;
static uint8_t g_bulk_in_ep;
static uint8_t g_bulk_out_ep;
static uint64_t g_capacity;
static uint32_t g_tag;
static int g_failure;
static xhci_ring_t g_command_ring;
static xhci_ring_t g_event_ring;
static xhci_ring_t g_ep0_ring;
static xhci_ring_t g_bulk_in_ring;
static xhci_ring_t g_bulk_out_ring;
static volatile uint64_t *g_dcbaa;
static uint64_t g_dcbaa_pa;
static volatile uint8_t *g_input_ctx;
static uint64_t g_input_ctx_pa;
static volatile uint8_t *g_device_ctx;
static uint64_t g_device_ctx_pa;
static volatile xhci_erst_entry_t *g_erst;
static uint64_t g_erst_pa;
static volatile uint8_t *g_data;
static uint64_t g_data_pa;

static void bytes_zero(volatile void *ptr, uint32_t n) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    for (uint32_t i = 0; i < n; i++) {
        p[i] = 0;
    }
}

static void bytes_copy(volatile uint8_t *dst, const volatile uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static uint16_t load_le16(const volatile uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t load_be32(const volatile uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_be16(volatile uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void store_be32(volatile uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int64_t pci_cfg(uint8_t bus, uint8_t device, uint8_t function,
                       uint8_t offset, int width, int is_write, uint64_t value) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_PCI_CFG;
    m.word[1] = ((uint64_t)bus << 16) | ((uint64_t)device << 8) | function;
    m.word[2] = (uint64_t)offset | ((uint64_t)width << 8) | ((uint64_t)is_write << 16);
    m.word[3] = value;
    if (robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL) != IPC_ERR_NONE) {
        return -1;
    }
    return (int64_t)m.word[0];
}

static int dma_alloc(uint64_t bytes, uint64_t *out_va, uint64_t *out_pa) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_DMA_ALLOC;
    m.word[1] = bytes;
    if (robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL) != IPC_ERR_NONE) {
        return -1;
    }
    *out_va = m.word[0];
    *out_pa = m.word[1];
    return 0;
}

static int map_mmio(uint64_t phys, uint64_t bytes, uint64_t *out_va) {
    msg_regs_t m = (msg_regs_t){0};
    m.word[0] = SYS_INFO_CAT_HW_MMIO_MAP;
    m.word[1] = phys;
    m.word[2] = bytes;
    if (robu_ipc_raw(0, 0, IPC_FLAG_SYS_INFO, &m, NULL) != IPC_ERR_NONE) {
        return -1;
    }
    *out_va = m.word[0];
    return 0;
}

static uint32_t mmio_read32(uint32_t offset) {
    return *(volatile uint32_t *)(g_mmio + offset);
}

static uint8_t mmio_read8(uint32_t offset) {
    return *(volatile uint8_t *)(g_mmio + offset);
}

static void mmio_write8(uint32_t offset, uint8_t value) {
    *(volatile uint8_t *)(g_mmio + offset) = value;
}

static void mmio_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(g_mmio + offset) = value;
}

static void mmio_write64(uint32_t offset, uint64_t value) {
    mmio_write32(offset, (uint32_t)value);
    mmio_write32(offset + 4, (uint32_t)(value >> 32));
}

static int wait_reg(uint32_t offset, uint32_t mask, int want_set, uint32_t ticks) {
    for (uint32_t i = 0; i < ticks; i++) {
        for (uint32_t spin = 0; spin < XHCI_WAIT_SPINS; spin++) {
            uint32_t value = mmio_read32(offset);
            if (((value & mask) != 0) == want_set) {
                return 0;
            }
            asm volatile("pause");
        }
        ipc_sleep(1);
    }
    return -1;
}

static int ring_init(xhci_ring_t *ring) {
    uint64_t va;
    if (dma_alloc(XHCI_PAGE_SIZE, &va, &ring->pa) != 0) {
        return -1;
    }
    ring->va = (volatile xhci_trb_t *)va;
    bytes_zero(ring->va, XHCI_PAGE_SIZE);
    ring->index = 0;
    ring->cycle = 1;
    return 0;
}

static uint64_t ring_push(xhci_ring_t *ring, uint64_t parameter,
                          uint32_t status, uint32_t control) {
    if (ring->index == XHCI_RING_TRBS - 1) {
        volatile xhci_trb_t *link = &ring->va[ring->index];
        link->parameter = ring->pa;
        link->status = 0;
        asm volatile("" ::: "memory");
        link->control = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) |
                        XHCI_TRB_TC | (ring->cycle ? XHCI_TRB_CYCLE : 0);
        ring->index = 0;
        ring->cycle ^= 1;
    }
    volatile xhci_trb_t *trb = &ring->va[ring->index];
    uint64_t pa = ring->pa + (uint64_t)ring->index * sizeof(xhci_trb_t);
    trb->parameter = parameter;
    trb->status = status;
    asm volatile("" ::: "memory");
    trb->control = control | (ring->cycle ? XHCI_TRB_CYCLE : 0);
    ring->index++;
    return pa;
}

static void event_advance(void) {
    g_event_ring.index++;
    if (g_event_ring.index == XHCI_RING_TRBS) {
        g_event_ring.index = 0;
        g_event_ring.cycle ^= 1;
    }
    mmio_write64(g_rt_base + XHCI_RT_IR0 + XHCI_IR_ERDP,
                 g_event_ring.pa + (uint64_t)g_event_ring.index * sizeof(xhci_trb_t) + 8);
}

static int event_wait(uint32_t type, uint64_t pointer, uint32_t *out_control) {
    for (uint32_t waited = 0; waited < XHCI_WAIT_EVENT_TICKS; waited++) {
        for (uint32_t spin = 0; spin < XHCI_WAIT_SPINS; spin++) {
            volatile xhci_trb_t *event = &g_event_ring.va[g_event_ring.index];
            uint32_t control = event->control;
            if ((control & XHCI_TRB_CYCLE) == (g_event_ring.cycle ? XHCI_TRB_CYCLE : 0)) {
                uint32_t event_type = (control >> XHCI_TRB_TYPE_SHIFT) & 0x3FU;
                uint64_t event_pointer = event->parameter;
                uint32_t status = event->status;
                event_advance();
                if (event_type == type && (pointer == 0 || event_pointer == pointer)) {
                    if (out_control) {
                        *out_control = control;
                    }
                    return ((status >> 24) & 0xFFU) == XHCI_CC_SUCCESS ? 0 : -1;
                }
                continue;
            }
            asm volatile("pause");
        }
        ipc_sleep(1);
    }
    return -1;
}

static int command_submit(uint64_t parameter, uint32_t control, uint32_t *out_control) {
    uint64_t trb_pa = ring_push(&g_command_ring, parameter, 0, control);
    asm volatile("" ::: "memory");
    mmio_write32(g_db_base, 0);
    return event_wait(XHCI_TRB_COMMAND_COMPLETION_EVENT, trb_pa, out_control);
}

static int transfer_submit(xhci_ring_t *ring, uint8_t endpoint, uint64_t data_pa, uint32_t bytes) {
    uint64_t trb_pa = ring_push(ring, data_pa, bytes,
                                (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC);
    asm volatile("" ::: "memory");
    mmio_write32(g_db_base + (uint32_t)g_slot * 4, endpoint);
    return event_wait(XHCI_TRB_TRANSFER_EVENT, trb_pa, NULL);
}

static int control_transfer(uint8_t request_type, uint8_t request, uint16_t value,
                            uint16_t index, uint16_t length, int data_in) {
    uint32_t trt = length == 0 ? XHCI_SETUP_TRT_NONE :
                   (data_in ? XHCI_SETUP_TRT_IN : XHCI_SETUP_TRT_OUT);
    uint64_t setup = (uint64_t)request_type | ((uint64_t)request << 8) |
                     ((uint64_t)value << 16) | ((uint64_t)index << 32) |
                     ((uint64_t)length << 48);
    ring_push(&g_ep0_ring, setup, 8,
              (XHCI_TRB_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IDT |
              (trt << 16));
    if (length) {
        ring_push(&g_ep0_ring, g_data_pa, length,
                  (XHCI_TRB_DATA_STAGE << XHCI_TRB_TYPE_SHIFT) |
                  (data_in ? XHCI_TRB_DIR_IN : 0));
    }
    uint64_t status_pa = ring_push(&g_ep0_ring, 0, 0,
                                   (XHCI_TRB_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) |
                                   XHCI_TRB_IOC |
                                   ((length == 0 || !data_in) ? XHCI_TRB_DIR_IN : 0));
    asm volatile("" ::: "memory");
    mmio_write32(g_db_base + (uint32_t)g_slot * 4, 1);
    return event_wait(XHCI_TRB_TRANSFER_EVENT, status_pa, NULL);
}

static int find_controller(uint8_t *out_bus, uint8_t *out_device, uint8_t *out_function) {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t device = 0; device < 32; device++) {
            int64_t vendor = pci_cfg((uint8_t)bus, (uint8_t)device, 0, 0, 2, 0, 0);
            if (vendor < 0) {
                return -1;
            }
            if ((uint16_t)vendor == 0xFFFFU) {
                continue;
            }
            int64_t header = pci_cfg((uint8_t)bus, (uint8_t)device, 0, 0x0E, 1, 0, 0);
            if (header < 0) {
                return -1;
            }
            uint32_t functions = header >= 0 && (header & 0x80) ? 8 : 1;
            for (uint32_t function = 0; function < functions; function++) {
                vendor = pci_cfg((uint8_t)bus, (uint8_t)device, (uint8_t)function, 0, 2, 0, 0);
                if (vendor < 0) {
                    return -1;
                }
                if ((uint16_t)vendor == 0xFFFFU) {
                    continue;
                }
                int64_t class_info = pci_cfg((uint8_t)bus, (uint8_t)device, (uint8_t)function,
                                             8, 4, 0, 0);
                if (class_info < 0) {
                    return -1;
                }
                if (((uint32_t)class_info >> 8) != 0x0C0330U) {
                    continue;
                }
                *out_bus = (uint8_t)bus;
                *out_device = (uint8_t)device;
                *out_function = (uint8_t)function;
                return 0;
            }
        }
    }
    return -1;
}

static int take_ownership(uint32_t hccparams1) {
    uint32_t offset = ((hccparams1 >> 16) & 0xFFFFU) * 4U;
    for (uint32_t i = 0; offset && i < 64; i++) {
        if (offset > XHCI_MMIO_BYTES - 8U) {
            return -1;
        }
        uint32_t cap = mmio_read32(offset);
        uint32_t next = ((cap >> 8) & 0xFFU) * 4U;
        if ((cap & 0xFFU) == 1) {
            mmio_write8(offset + 7, mmio_read8(offset + 7) | 1U);
            if (wait_reg(offset, 1U << 16, 0, XHCI_WAIT_OWNERSHIP_TICKS) != 0) {
                return -1;
            }
            mmio_write32(offset + 4, 0);
            return 0;
        }
        if (!next || next > XHCI_MMIO_BYTES - offset) {
            break;
        }
        offset += next;
    }
    return 0;
}

static int reset_controller(void) {
    uint32_t command = mmio_read32(g_op_base + XHCI_OP_USBCMD);
    mmio_write32(g_op_base + XHCI_OP_USBCMD, command & ~XHCI_USBCMD_RUN);
    if (wait_reg(g_op_base + XHCI_OP_USBSTS, XHCI_USBSTS_HCH, 1,
                 XHCI_WAIT_HALT_TICKS) != 0) {
        return XHCI_FAILURE_HALT;
    }
    command = mmio_read32(g_op_base + XHCI_OP_USBCMD);
    mmio_write32(g_op_base + XHCI_OP_USBCMD, command | XHCI_USBCMD_HCRST);
    if (wait_reg(g_op_base + XHCI_OP_USBCMD, XHCI_USBCMD_HCRST, 0,
                 XHCI_WAIT_RESET_TICKS) != 0) {
        return XHCI_FAILURE_RESET;
    }
    if (wait_reg(g_op_base + XHCI_OP_USBSTS, XHCI_USBSTS_CNR, 0,
                 XHCI_WAIT_READY_TICKS) != 0) {
        return XHCI_FAILURE_READY;
    }
    return 0;
}

static int setup_controller(uint32_t hcsparams1, uint32_t hcsparams2) {
    uint64_t va;
    if (dma_alloc(XHCI_PAGE_SIZE, &va, &g_dcbaa_pa) != 0) {
        return XHCI_FAILURE_DCBAA;
    }
    g_dcbaa = (volatile uint64_t *)va;
    bytes_zero(g_dcbaa, XHCI_PAGE_SIZE);
    uint32_t scratchpads = ((hcsparams2 >> 27) & 0x1FU) << 5;
    scratchpads |= (hcsparams2 >> 21) & 0x1FU;
    if (scratchpads) {
        uint64_t scratch_va;
        uint64_t scratch_pa;
        uint64_t scratch_bytes = (uint64_t)scratchpads * sizeof(uint64_t);
        if (dma_alloc(scratch_bytes, &scratch_va, &scratch_pa) != 0) {
            return XHCI_FAILURE_SCRATCHPAD_ARRAY;
        }
        volatile uint64_t *scratch = (volatile uint64_t *)scratch_va;
        bytes_zero(scratch, (uint32_t)scratch_bytes);
        for (uint32_t i = 0; i < scratchpads; i++) {
            uint64_t page_va;
            uint64_t page_pa;
            if (dma_alloc(XHCI_PAGE_SIZE, &page_va, &page_pa) != 0) {
                return XHCI_FAILURE_SCRATCHPAD_PAGE;
            }
            scratch[i] = page_pa;
        }
        g_dcbaa[0] = scratch_pa;
    }
    if (ring_init(&g_command_ring) != 0 || ring_init(&g_event_ring) != 0 ||
        ring_init(&g_ep0_ring) != 0 || ring_init(&g_bulk_in_ring) != 0 ||
        ring_init(&g_bulk_out_ring) != 0) {
        return XHCI_FAILURE_RING;
    }
    if (dma_alloc(XHCI_PAGE_SIZE, &va, &g_erst_pa) != 0) {
        return XHCI_FAILURE_ERST;
    }
    g_erst = (volatile xhci_erst_entry_t *)va;
    bytes_zero(g_erst, XHCI_PAGE_SIZE);
    g_erst[0].segment_base = g_event_ring.pa;
    g_erst[0].segment_size = XHCI_RING_TRBS;
    if (dma_alloc(XHCI_PAGE_SIZE, &va, &g_input_ctx_pa) != 0) {
        return XHCI_FAILURE_INPUT_CONTEXT;
    }
    g_input_ctx = (volatile uint8_t *)va;
    bytes_zero(g_input_ctx, XHCI_PAGE_SIZE);
    if (dma_alloc(XHCI_PAGE_SIZE, &va, &g_device_ctx_pa) != 0) {
        return XHCI_FAILURE_DEVICE_CONTEXT;
    }
    g_device_ctx = (volatile uint8_t *)va;
    bytes_zero(g_device_ctx, XHCI_PAGE_SIZE);
    if (dma_alloc(XHCI_DATA_BYTES, &va, &g_data_pa) != 0) {
        return XHCI_FAILURE_DATA;
    }
    g_data = (volatile uint8_t *)va;
    bytes_zero(g_data, XHCI_DATA_BYTES);
    mmio_write64(g_op_base + XHCI_OP_DCBAAP, g_dcbaa_pa);
    mmio_write64(g_op_base + XHCI_OP_CRCR, g_command_ring.pa | XHCI_TRB_CYCLE);
    mmio_write32(g_rt_base + XHCI_RT_IR0 + XHCI_IR_ERSTSZ, 1);
    mmio_write64(g_rt_base + XHCI_RT_IR0 + XHCI_IR_ERDP, g_event_ring.pa);
    mmio_write64(g_rt_base + XHCI_RT_IR0 + XHCI_IR_ERSTBA, g_erst_pa);
    mmio_write32(g_rt_base + XHCI_RT_IR0 + XHCI_IR_IMAN, 2);
    uint32_t slots = hcsparams1 & 0xFFU;
    if (slots == 0) {
        return XHCI_FAILURE_SLOTS;
    }
    mmio_write32(g_op_base + XHCI_OP_CONFIG, slots);
    mmio_write32(g_op_base + XHCI_OP_USBCMD,
                 mmio_read32(g_op_base + XHCI_OP_USBCMD) | XHCI_USBCMD_RUN);
    if (wait_reg(g_op_base + XHCI_OP_USBSTS, XHCI_USBSTS_HCH, 0,
                 XHCI_WAIT_HALT_TICKS) != 0) {
        return XHCI_FAILURE_RUN;
    }
    return 0;
}

static void power_ports(uint32_t max_ports) {
    for (uint32_t port = 1; port <= max_ports; port++) {
        uint32_t offset = g_op_base + XHCI_OP_PORTSC + (port - 1) * 16;
        mmio_write32(offset, XHCI_PORTSC_PP);
    }
    ipc_sleep(2);
}

static int reset_port(uint32_t port, uint8_t *out_speed) {
    uint32_t offset = g_op_base + XHCI_OP_PORTSC + (port - 1) * 16;
    uint32_t portsc = mmio_read32(offset);
    if (!(portsc & XHCI_PORTSC_CCS)) {
        return -1;
    }
    uint32_t speed = (portsc >> XHCI_PORTSC_SPEED_SHIFT) & 0xFU;
    uint32_t reset = XHCI_PORTSC_PP | (speed >= 4 ? XHCI_PORTSC_WPR : XHCI_PORTSC_PR);
    uint32_t reset_bit = speed >= 4 ? XHCI_PORTSC_WPR : XHCI_PORTSC_PR;
    mmio_write32(offset, reset);
    if (wait_reg(offset, reset_bit, 0, XHCI_WAIT_PORT_TICKS) != 0) {
        return -2;
    }
    portsc = mmio_read32(offset);
    if (!(portsc & XHCI_PORTSC_CCS) || !(portsc & XHCI_PORTSC_PED)) {
        return -3;
    }
    *out_speed = (uint8_t)((portsc >> XHCI_PORTSC_SPEED_SHIFT) & 0xFU);
    return 0;
}

static void input_context_begin(uint32_t add_flags) {
    bytes_zero(g_input_ctx, XHCI_PAGE_SIZE);
    *(volatile uint32_t *)(g_input_ctx + 4) = add_flags;
}

static volatile uint32_t *input_context(uint32_t index) {
    return (volatile uint32_t *)(g_input_ctx + (uint64_t)index * g_context_size);
}

static void endpoint_context(uint32_t index, uint16_t mps, uint8_t endpoint_type,
                             uint64_t ring_pa) {
    volatile uint32_t *ctx = input_context(index);
    ctx[0] = 0;
    ctx[1] = ((uint32_t)mps << 16) | ((uint32_t)endpoint_type << 3) | (3U << 1);
    ctx[2] = (uint32_t)(ring_pa | XHCI_TRB_CYCLE);
    ctx[3] = (uint32_t)(ring_pa >> 32);
    ctx[4] = 1024U << 16;
}

static uint16_t initial_ep0_mps(uint8_t speed) {
    if (speed == 3) {
        return 64;
    }
    if (speed >= 4) {
        return 512;
    }
    return 8;
}

static int address_device(uint8_t speed) {
    uint32_t completion;
    g_failure = XHCI_FAILURE_ENABLE_SLOT;
    if (command_submit(0, XHCI_TRB_ENABLE_SLOT_CMD << XHCI_TRB_TYPE_SHIFT, &completion) != 0) {
        return -1;
    }
    g_slot = (uint8_t)(completion >> 24);
    if (g_slot == 0) {
        return -1;
    }
    bytes_zero(g_device_ctx, XHCI_PAGE_SIZE);
    g_dcbaa[g_slot] = g_device_ctx_pa;
    input_context_begin((1U << 0) | (1U << 1));
    volatile uint32_t *slot = input_context(1);
    slot[0] = ((uint32_t)speed << 20) | (1U << 27);
    slot[1] = (uint32_t)g_port << 16;
    endpoint_context(2, initial_ep0_mps(speed), 4, g_ep0_ring.pa);
    g_failure = XHCI_FAILURE_ADDRESS_DEVICE;
    if (command_submit(g_input_ctx_pa,
                       (XHCI_TRB_ADDRESS_DEVICE_CMD << XHCI_TRB_TYPE_SHIFT) |
                       ((uint32_t)g_slot << 24), NULL) != 0) {
        return -1;
    }
    return 0;
}

static void disable_slot(void) {
    if (!g_slot) {
        return;
    }
    command_submit(0, (XHCI_TRB_DISABLE_SLOT_CMD << XHCI_TRB_TYPE_SHIFT) |
                   ((uint32_t)g_slot << 24), NULL);
    g_dcbaa[g_slot] = 0;
    g_slot = 0;
}

static int update_ep0_mps(uint16_t mps) {
    input_context_begin(1U << 1);
    endpoint_context(2, mps, 4, g_ep0_ring.pa);
    return command_submit(g_input_ctx_pa,
                          (XHCI_TRB_EVALUATE_CONTEXT_CMD << XHCI_TRB_TYPE_SHIFT) |
                          ((uint32_t)g_slot << 24), NULL);
}

static int configure_bulk(uint8_t in_address, uint16_t in_mps,
                          uint8_t out_address, uint16_t out_mps) {
    g_bulk_in_ep = (uint8_t)(((in_address & 0x0FU) * 2U) + 1U);
    g_bulk_out_ep = (uint8_t)((out_address & 0x0FU) * 2U);
    if (g_bulk_in_ep < 2 || g_bulk_out_ep < 2) {
        return -1;
    }
    uint32_t max_ep = g_bulk_in_ep > g_bulk_out_ep ? g_bulk_in_ep : g_bulk_out_ep;
    input_context_begin((1U << 0) | (1U << g_bulk_in_ep) | (1U << g_bulk_out_ep));
    volatile uint32_t *slot = input_context(1);
    slot[0] = max_ep << 27;
    endpoint_context(g_bulk_in_ep + 1, in_mps, 6, g_bulk_in_ring.pa);
    endpoint_context(g_bulk_out_ep + 1, out_mps, 2, g_bulk_out_ring.pa);
    return command_submit(g_input_ctx_pa,
                          (XHCI_TRB_CONFIGURE_ENDPOINT_CMD << XHCI_TRB_TYPE_SHIFT) |
                          ((uint32_t)g_slot << 24), NULL);
}

static int enumerate_device(uint8_t speed) {
    if (address_device(speed) != 0) {
        return -1;
    }
    ipc_sleep(1);
    g_failure = XHCI_FAILURE_DEVICE_DESCRIPTOR;
    if (control_transfer(0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_DEVICE << 8, 0, 8, 1) != 0) {
        return -1;
    }
    uint16_t mps = g_data[7];
    if (speed >= 4) {
        if (mps > 9) {
            g_failure = XHCI_FAILURE_EP0_MPS;
            return -1;
        }
        mps = (uint16_t)(1U << mps);
    }
    if (mps == 0) {
        g_failure = XHCI_FAILURE_EP0_MPS;
        return -1;
    }
    g_failure = XHCI_FAILURE_EVALUATE_EP0;
    if (update_ep0_mps(mps) != 0) {
        return -1;
    }
    g_failure = XHCI_FAILURE_CONFIG_HEADER;
    if (control_transfer(0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_CONFIG << 8, 0, 9, 1) != 0) {
        return -1;
    }
    uint16_t config_length = load_le16(g_data + 2);
    if (config_length < 9) {
        g_failure = XHCI_FAILURE_CONFIG_LENGTH;
        return -1;
    }
    g_failure = XHCI_FAILURE_CONFIG_BODY;
    if (control_transfer(0x80, USB_REQ_GET_DESCRIPTOR, USB_DESC_CONFIG << 8, 0, config_length, 1) != 0) {
        return -1;
    }
    uint8_t config_value = g_data[5];
    uint8_t interface_number = 0;
    uint8_t interface_ok = 0;
    uint8_t in_address = 0;
    uint8_t out_address = 0;
    uint16_t in_mps = 0;
    uint16_t out_mps = 0;
    for (uint32_t offset = 0; offset + 2 <= config_length;) {
        uint8_t length = g_data[offset];
        uint8_t type = g_data[offset + 1];
        if (length < 2 || offset + length > config_length) {
            g_failure = XHCI_FAILURE_CONFIG_BODY;
            return -1;
        }
        if (type == USB_DESC_INTERFACE && length >= 9) {
            interface_number = g_data[offset + 2];
            interface_ok = g_data[offset + 5] == USB_CLASS_MASS_STORAGE &&
                           g_data[offset + 6] == USB_SUBCLASS_SCSI &&
                           g_data[offset + 7] == USB_PROTOCOL_BULK_ONLY;
        } else if (type == USB_DESC_ENDPOINT && interface_ok && length >= 7 &&
                   (g_data[offset + 3] & 3U) == 2U) {
            uint8_t address = g_data[offset + 2];
            uint16_t endpoint_mps = load_le16(g_data + offset + 4) & 0x7FFU;
            if (address & 0x80U) {
                in_address = address;
                in_mps = endpoint_mps;
            } else {
                out_address = address;
                out_mps = endpoint_mps;
            }
        }
        offset += length;
    }
    if (!in_address || !out_address || !in_mps || !out_mps) {
        g_failure = XHCI_FAILURE_MASS_STORAGE;
        return -1;
    }
    g_failure = XHCI_FAILURE_SET_CONFIGURATION;
    if (control_transfer(0x00, USB_REQ_SET_CONFIGURATION, config_value, 0, 0, 0) != 0) {
        return -1;
    }
    (void)interface_number;
    g_failure = XHCI_FAILURE_CONFIGURE_ENDPOINT;
    return configure_bulk(in_address, in_mps, out_address, out_mps);
}

static int bot_command(const uint8_t *cdb, uint8_t cdb_length,
                       int data_in, uint32_t bytes) {
    if (bytes > XHCI_DATA_BYTES - XHCI_BOT_DATA_OFFSET - sizeof(usb_csw_t)) {
        return -1;
    }
    usb_cbw_t *cbw = (usb_cbw_t *)g_data;
    bytes_zero(g_data, sizeof(usb_cbw_t));
    cbw->signature = 0x43425355U;
    cbw->tag = ++g_tag;
    uint32_t tag = cbw->tag;
    cbw->transfer_length = bytes;
    cbw->flags = data_in ? 0x80U : 0;
    cbw->lun = 0;
    cbw->cdb_length = cdb_length;
    for (uint32_t i = 0; i < cdb_length; i++) {
        cbw->cdb[i] = cdb[i];
    }
    if (transfer_submit(&g_bulk_out_ring, g_bulk_out_ep, g_data_pa, sizeof(usb_cbw_t)) != 0) {
        return -1;
    }
    if (bytes) {
        if (data_in) {
            if (transfer_submit(&g_bulk_in_ring, g_bulk_in_ep,
                                g_data_pa + XHCI_BOT_DATA_OFFSET, bytes) != 0) {
                return -1;
            }
        } else if (transfer_submit(&g_bulk_out_ring, g_bulk_out_ep,
                                   g_data_pa + XHCI_BOT_DATA_OFFSET, bytes) != 0) {
            return -1;
        }
    }
    uint64_t csw_pa = g_data_pa + XHCI_DATA_BYTES - 16;
    if (transfer_submit(&g_bulk_in_ring, g_bulk_in_ep, csw_pa, sizeof(usb_csw_t)) != 0) {
        return -1;
    }
    usb_csw_t *csw = (usb_csw_t *)(g_data + XHCI_DATA_BYTES - 16);
    if (csw->signature != 0x53425355U || csw->tag != tag || csw->status != 0) {
        return -1;
    }
    return 0;
}

static int read_capacity(void) {
    uint8_t cdb[10] = {0};
    cdb[0] = SCSI_READ_CAPACITY_10;
    if (bot_command(cdb, sizeof(cdb), 1, 8) != 0) {
        return -1;
    }
    uint32_t last_lba = load_be32(g_data + XHCI_BOT_DATA_OFFSET);
    uint32_t block_size = load_be32(g_data + XHCI_BOT_DATA_OFFSET + 4);
    if (block_size != 512 || last_lba == 0xFFFFFFFFU) {
        return -1;
    }
    g_capacity = (uint64_t)last_lba + 1;
    return 0;
}

int xhci_probe(void) {
    if (g_capacity) {
        return 0;
    }
    g_failure = XHCI_FAILURE_NO_CONTROLLER;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    if (find_controller(&bus, &device, &function) != 0) {
        return -1;
    }
    g_failure = XHCI_FAILURE_PCI;
    int64_t command = pci_cfg(bus, device, function, 4, 2, 0, 0);
    int64_t bar_low = pci_cfg(bus, device, function, 0x10, 4, 0, 0);
    if (command < 0 || bar_low < 0 || ((uint32_t)bar_low & 1U)) {
        return -1;
    }
    uint64_t bar = (uint32_t)bar_low & ~0xFULL;
    if (((uint32_t)bar_low & 6U) == 4U) {
        int64_t bar_high = pci_cfg(bus, device, function, 0x14, 4, 0, 0);
        if (bar_high < 0) {
            return -1;
        }
        bar |= (uint64_t)(uint32_t)bar_high << 32;
    }
    if (pci_cfg(bus, device, function, 4, 2, 1, (uint16_t)command | 6U) < 0) {
        return -1;
    }
    g_failure = XHCI_FAILURE_MMIO;
    uint64_t mapped;
    if (map_mmio(bar, XHCI_MMIO_BYTES, &mapped) != 0) {
        return -1;
    }
    g_mmio = (volatile uint8_t *)mapped;
    uint32_t caplength = g_mmio[0];
    uint32_t hcsparams1 = mmio_read32(XHCI_CAP_HCSPARAMS1);
    uint32_t hcsparams2 = mmio_read32(XHCI_CAP_HCSPARAMS2);
    uint32_t hccparams1 = mmio_read32(XHCI_CAP_HCCPARAMS1);
    g_op_base = caplength;
    g_db_base = mmio_read32(XHCI_CAP_DBOFF) & ~3U;
    g_rt_base = mmio_read32(XHCI_CAP_RTSOFF) & ~0x1FU;
    g_context_size = (hccparams1 & 4U) ? 64U : 32U;
    if (caplength < 0x20 || g_op_base >= XHCI_MMIO_BYTES ||
        g_db_base >= XHCI_MMIO_BYTES || g_rt_base >= XHCI_MMIO_BYTES) {
        g_failure = XHCI_FAILURE_LAYOUT;
        return -1;
    }
    if (take_ownership(hccparams1) != 0) {
        g_failure = XHCI_FAILURE_OWNERSHIP;
        return -1;
    }
    int reset_rc = reset_controller();
    if (reset_rc != 0) {
        g_failure = reset_rc;
        return -1;
    }
    int setup_rc = setup_controller(hcsparams1, hcsparams2);
    if (setup_rc != 0) {
        g_failure = setup_rc;
        return -1;
    }
    uint32_t max_ports = hcsparams1 >> 24;
    power_ports(max_ports);
    int saw_connected = 0;
    int saw_reset_timeout = 0;
    int saw_disabled = 0;
    int found_storage_device = 0;
    for (uint32_t port = 1; port <= max_ports; port++) {
        uint8_t speed;
        int port_rc = reset_port(port, &speed);
        if (port_rc == -1) {
            continue;
        }
        saw_connected = 1;
        if (port_rc == -2) {
            saw_reset_timeout = 1;
            continue;
        }
        if (port_rc == -3) {
            saw_disabled = 1;
            continue;
        }
        g_port = (uint8_t)port;
        if (enumerate_device(speed) == 0) {
            found_storage_device = 1;
            break;
        }
        disable_slot();
    }
    if (!found_storage_device) {
        if (!saw_connected) {
            g_failure = XHCI_FAILURE_NO_PORT;
        } else if (saw_reset_timeout) {
            g_failure = XHCI_FAILURE_PORT_RESET;
        } else if (saw_disabled) {
            g_failure = XHCI_FAILURE_PORT_DISABLED;
        }
        return -1;
    }
    g_failure = XHCI_FAILURE_STORAGE;
    uint8_t tur[6] = {SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0};
    for (uint32_t i = 0; i < 100; i++) {
        if (bot_command(tur, sizeof(tur), 0, 0) == 0) {
            break;
        }
        if (i == 99) {
            return -1;
        }
    }
    if (read_capacity() != 0) {
        return -1;
    }
    g_failure = XHCI_FAILURE_NONE;
    return 0;
}

static int xhci_rw(uint64_t sector, uint32_t count, void *buf, int write) {
    g_failure = XHCI_FAILURE_IO;
    if (!g_capacity || sector >= g_capacity || count > g_capacity - sector) {
        return -1;
    }
    uint8_t *bytes = (uint8_t *)buf;
    while (count) {
        uint32_t chunk = count > XHCI_MAX_RW_SECTORS ? XHCI_MAX_RW_SECTORS : count;
        uint32_t byte_count = chunk * 512U;
        if (sector > 0xFFFFFFFFU) {
            return -1;
        }
        if (write) {
            bytes_copy(g_data + XHCI_BOT_DATA_OFFSET, bytes, byte_count);
        }
        uint8_t cdb[10] = {0};
        cdb[0] = write ? SCSI_WRITE_10 : SCSI_READ_10;
        store_be32(&cdb[2], (uint32_t)sector);
        store_be16(&cdb[7], (uint16_t)chunk);
        if (bot_command(cdb, sizeof(cdb), write ? 0 : 1, byte_count) != 0) {
            return -1;
        }
        if (!write) {
            bytes_copy(bytes, g_data + XHCI_BOT_DATA_OFFSET, byte_count);
        }
        bytes += byte_count;
        sector += chunk;
        count -= chunk;
    }
    g_failure = XHCI_FAILURE_NONE;
    return 0;
}

int xhci_read(uint64_t sector, uint32_t count, void *buf) {
    return xhci_rw(sector, count, buf, 0);
}

int xhci_write(uint64_t sector, uint32_t count, const void *buf) {
    return xhci_rw(sector, count, (void *)buf, 1);
}

uint64_t xhci_capacity_sectors(void) {
    return g_capacity;
}

int xhci_last_failure(void) {
    return g_failure;
}
