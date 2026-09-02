#ifndef ROBU_BOOT_H
#define ROBU_BOOT_H

#include "robu/types.h"
#include "robu/tcb.h"
#include "robu/kprintf.h"

#define PAGER_TID   1
#define MONITOR_TID 2

static inline uint64_t irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    asm volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#define SAFE_PRINT(...) do {              \
        if (!quiet_mode) {                \
            uint64_t _f = irq_save();     \
            kprintf(__VA_ARGS__);         \
            irq_restore(_f);              \
        }                                 \
    } while (0)

void rootfs_init(void);
void root_task_init(paddr_t untyped_base, uint64_t untyped_size);

tid_t pager_init(void);
tid_t devfs_init(void);
tid_t console_driver_init(void);
tid_t ramfs_init(void);
tid_t procfs_init(void);
tid_t sysfs_init(void);
tid_t diskfs_init(void);
tid_t ext2fs_init(void);
void bench_init(void);

void testing_set_untyped(paddr_t untyped_base, uint64_t untyped_size);
void run_deferred_boot_inits(void);

void abitest_init(void);
void argvtest_init(void);
void panic_test_init(void);
void test_exit_init(void);
void libctest_init(void);
void ramfstest_init(void);
void spawntest_init(void);
void consoletest_init(void);
void mousetest_init(void);
void fbtest_init(void);
void shmtest_init(void);
void socktest_init(void);
void pcitest_init(void);
tid_t blockdrv_init(void);
void diskfstest_init(void);
void fat16fstest_init(void);
void fat32fstest_init(void);
void ext2fstest_init(void);
void linuxvfstest_init(void);
void sigtest_init(void);
void readlinetest_init(void);
void toybox_sh_c_init(void);
tid_t test_report_init(void);

tcb_t *toybox_spawn(const char *name, int argc, const char *const *argv, uint8_t prio);
void toybox_cmd_init(const char *name, uint8_t prio);
void toybox_cmd_init_arg(const char *name, uint8_t prio, const char *arg1);
void toybox_pipeline_advance(void);
void toybox_pipeline_init(void);

#endif
