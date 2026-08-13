#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <confuse.h>

extern int __libc_spawn(const char *name, char *const argv[], char *const envp[]);

#define RC_CONF_PATH "/etc/rc.conf"

#define GETTY_COUNT 6
static const char *const getty_ttys[GETTY_COUNT] = {
    "rstty1", "rstty2", "rstty3", "rstty4", "rstty5", "rstty6"
};
static pid_t getty_pid[GETTY_COUNT];

static void spawn_getty(int i) {
    char *const svc_argv[] = { (char *)"tty_service", (char *)getty_ttys[i], 0 };
    int rc = __libc_spawn("tty_service", svc_argv, environ);
    if (rc < 0) {
        printf("[hello_initsys] failed to start tty_service for %s\n", getty_ttys[i]);
        getty_pid[i] = -1;
        return;
    }
    getty_pid[i] = rc;
    printf("[hello_initsys] started 'tty_service' for %s as tid=%d\n", getty_ttys[i], rc);
}

static void run_service(const char *name) {
    char *const svc_argv[] = { (char *)name, 0 };
    int rc = __libc_spawn(name, svc_argv, environ);
    if (rc < 0) {
        printf("[hello_initsys] service '%s' failed to start\n", name);
        return;
    }
    int status;
    pid_t child = waitpid(rc, &status, 0);
    if (child < 0) {
        printf("[hello_initsys] service '%s' (tid=%d) wait failed\n", name, rc);
        return;
    }
    printf("[hello_initsys] service '%s' (tid=%d) exited status=%d\n",
           name, rc, WEXITSTATUS(status));
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[hello_initsys] init starting\n");

    setenv("HOME", "/var/root", 1);
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);

    static char rc_shell[64] = "sh";
    int rc_respawn = 1;
    int rc_mount_services = 1;

    static cfg_opt_t rc_opts[] = {
        CFG_STR("shell", "sh", CFGF_NONE),
        CFG_BOOL("respawn", cfg_true, CFGF_NONE),
        CFG_BOOL("mount_services", cfg_true, CFGF_NONE),
        CFG_STR("tty", "auto", CFGF_NONE),
        CFG_END()
    };
    cfg_t *cfg = cfg_init(rc_opts, CFGF_NONE);
    if (cfg) {
        int cfg_rc = cfg_parse(cfg, RC_CONF_PATH);
        if (cfg_rc == CFG_SUCCESS) {
            const char *s = cfg_getstr(cfg, "shell");
            if (s) {
                strncpy(rc_shell, s, sizeof(rc_shell) - 1);
                rc_shell[sizeof(rc_shell) - 1] = '\0';
            }
            rc_respawn = (cfg_getbool(cfg, "respawn") == cfg_true);
            rc_mount_services = (cfg_getbool(cfg, "mount_services") == cfg_true);
            printf("[hello_initsys] loaded " RC_CONF_PATH ": shell='%s' respawn=%d mount_services=%d\n",
                   rc_shell, rc_respawn, rc_mount_services);
        } else {
            printf("[hello_initsys] no usable " RC_CONF_PATH ", using defaults: shell='%s' respawn=%d mount_services=%d\n",
                   rc_shell, rc_respawn, rc_mount_services);
        }
    }

    if (rc_mount_services) {
        run_service("mount_devfs");
        run_service("mount_procfs");
        run_service("mount_sysfs");
        run_service("mount_tmpfs");
    }

    if (argc > 1) {
        const char *name = argv[1];
        char **cmd_argv = argv + 1;
        do {
            int rc = __libc_spawn(name, cmd_argv, environ);
            if (rc < 0) {
                printf("[hello_initsys] failed to start '%s'\n", name);
                sleep(1);
                continue;
            }
            printf("[hello_initsys] started '%s' as tid=%d\n", name, rc);
            for (;;) {
                int status;
                pid_t child = wait(&status);
                if (child < 0) {
                    break;
                }
                printf("[hello_initsys] tid=%d exited status=%d\n", child, WEXITSTATUS(status));
                if (child == rc) {
                    break;
                }
            }
        } while (rc_respawn);
    } else {
        for (int i = 0; i < GETTY_COUNT; i++) {
            spawn_getty(i);
        }
        for (;;) {
            int status;
            pid_t child = wait(&status);
            if (child < 0) {
                break;
            }
            int idx = -1;
            for (int i = 0; i < GETTY_COUNT; i++) {
                if (getty_pid[i] == child) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0) {
                printf("[hello_initsys] tid=%d exited status=%d\n", child, WEXITSTATUS(status));
                continue;
            }
            printf("[hello_initsys] tty_service for %s (tid=%d) exited status=%d\n",
                   getty_ttys[idx], child, WEXITSTATUS(status));
            if (rc_respawn) {
                spawn_getty(idx);
            } else {
                getty_pid[idx] = -1;
            }
        }
    }

    printf("[hello_initsys] respawn disabled, init exiting\n");
    return 0;
}
