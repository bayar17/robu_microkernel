#include "robu/types.h"
#include "robu/kprintf.h"
#include "robu/sched.h"
#include "robu/elf.h"
#include "robu/cmdline.h"
#include "robu/vfs.h"
#include "robu/kinfo.h"
#include "robu/rootfs.h"
#include "robu/execreq.h"
#include "../boot.h"

static tid_t ramfs_tid(void) {
    return (tid_t)kinfo_user()->ramfs_tid;
}

tcb_t *toybox_spawn(const char *name, int argc, const char *const *argv, uint8_t prio) {
    const uint8_t *elf_start, *elf_end;
    if (rootfs_lookup(name, &elf_start, &elf_end) == 0) {
        tcb_t *t = elf_load_and_spawn_argv(name, elf_start, elf_end, prio, PAGER_TID, argc, argv);
        if (!t) {
            kprintf("[boot] FATAL: %s failed to load\n", name);
            for (;;) { asm volatile("cli; hlt"); }
        }
        return t;
    }

    execreq_params_t params = {0};
    params.is_spawn_create = 1;
    uint32_t namelen = 0;
    while (name[namelen] && namelen + 1 < sizeof(params.name)) {
        params.name[namelen] = name[namelen];
        namelen++;
    }
    params.name[namelen] = '\0';
    uint32_t used = 0;
    for (int i = 0; i < argc && (uint32_t)i < SPAWN_MAX_ARGS; i++) {
        uint32_t len = 0;
        while (argv[i][len]) {
            len++;
        }
        if (used + len + 1 > SPAWN_REQ_MAX_LEN) {
            break;
        }
        for (uint32_t j = 0; j < len; j++) {
            params.strbuf[used + j] = (uint8_t)argv[i][j];
        }
        params.strbuf[used + len] = '\0';
        params.argv_off[i] = used;
        used += len + 1;
        params.argc++;
    }
    params.strbuf_used = used;
    params.prio = prio;
    params.pager_tid = PAGER_TID;

    uint32_t slot;
    uint16_t generation;
    if (execreq_submit_poll(&params, &slot, &generation) != 0) {
        kprintf("[boot] FATAL: rootfs has no entry named '%s'\n", name);
        for (;;) { asm volatile("cli; hlt"); }
    }
    int rc;
    while ((rc = execreq_poll_ready(slot, generation)) == 0) {
        sched_yield();
    }
    (void)rc;
    tcb_t *t = execreq_finish_poll_spawn(slot, generation);
    if (!t) {
        kprintf("[boot] FATAL: %s failed to load\n", name);
        for (;;) { asm volatile("cli; hlt"); }
    }
    return t;
}

void toybox_cmd_init_arg(const char *name, uint8_t prio, const char *arg1) {
    char flag[32] = "toybox_";
    size_t i = 0;
    while (name[i] && i < sizeof(flag) - 8) {
        flag[7 + i] = name[i];
        i++;
    }
    flag[7 + i] = '\0';
    if (!cmdline_get(flag)) {
        return;
    }
    if (arg1) {
        const char *argv[2];
        argv[0] = name;
        argv[1] = arg1;
        toybox_spawn(name, 2, argv, prio);
        return;
    }
    const char *argv[1];
    argv[0] = name;
    toybox_spawn(name, 1, argv, prio);
}

void toybox_cmd_init(const char *name, uint8_t prio) {
    toybox_cmd_init_arg(name, prio, NULL);
}

typedef struct {
    const char *name;
    const char *argv[4];
    int argc;
} pipeline_step_t;

static const pipeline_step_t toybox_pipeline_steps[] = {
    { "touch", { "touch", "/touched.txt" }, 2 },
    { "cat",   { "cat", "/greeting.txt" }, 2 },
    { "ls",    { "ls", "/" }, 2 },
    { "cp",    { "cp", "/greeting.txt", "/copy.txt" }, 3 },
    { "mv",    { "mv", "/copy.txt", "/renamed.txt" }, 3 },
    { "tail",  { "tail", "/renamed.txt" }, 2 },
    { "find",  { "find", "/", "-name", "renamed.txt" }, 4 },
};
#define TOYBOX_PIPELINE_STEP_COUNT \
    (sizeof(toybox_pipeline_steps) / sizeof(toybox_pipeline_steps[0]))

static int toybox_pipeline_active;
static uint64_t toybox_pipeline_index;

void toybox_pipeline_advance(void) {
    if (!toybox_pipeline_active) {
        return;
    }
    if (toybox_pipeline_index >= TOYBOX_PIPELINE_STEP_COUNT) {
        toybox_pipeline_active = 0;
        return;
    }
    const pipeline_step_t *step = &toybox_pipeline_steps[toybox_pipeline_index];
    toybox_spawn(step->name, step->argc, step->argv, 9);
    toybox_pipeline_index++;
}

void toybox_pipeline_init(void) {
    if (!cmdline_get("toybox_pipeline")) {
        return;
    }
    static const char greeting[] = "hello from the toybox port\n";
    int64_t h = vfs_open(ramfs_tid(), "greeting.txt", VFS_O_CREAT | VFS_O_TRUNC);
    if (h < 0) {
        kprintf("[boot] FATAL: could not seed /greeting.txt in ramfs\n");
        for (;;) { asm volatile("cli; hlt"); }
    }
    vfs_write(ramfs_tid(), (uint64_t)h, greeting, sizeof(greeting) - 1);
    vfs_close(ramfs_tid(), (uint64_t)h);
    toybox_pipeline_active = 1;
    toybox_pipeline_index = 0;
    toybox_pipeline_advance();
}
