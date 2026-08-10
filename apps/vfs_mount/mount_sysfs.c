#include "robu/types.h"
#include "robu/uipc.h"
#include "robu/ipc.h"
#include "robu/kinfo.h"
#include "robu/vfs.h"

#define DEV_CONSOLE 0

static void console_msg(tid_t devfs_tid, const char *s) {
    if (!devfs_tid) {
        return;
    }
    uint64_t len = 0;
    while (s[len]) {
        len++;
    }
    uint64_t off = 0;
    while (off < len) {
        uint64_t chunk = len - off;
        if (chunk > VFS_WRITE_MAX) {
            chunk = VFS_WRITE_MAX;
        }
        int64_t n = vfs_write(devfs_tid, DEV_CONSOLE, s + off, chunk);
        if (n <= 0) {
            return;
        }
        off += (uint64_t)n;
    }
}

void _start(void) {
    tid_t devfs_tid = (tid_t)kinfo_user()->devfs_tid;
    int ok = kinfo_user()->sysfs_tid != 0;
    console_msg(devfs_tid, ok ? "[mount_sysfs] /var/sys ready\n" : "[mount_sysfs] FATAL: sysfs not running\n");
    ipc_exit(ok ? 0 : 1);
}
