#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <confuse.h>

#define RC_CONF_PATH "/etc/rc.conf"

static int try_open(const char *name) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/%s", name);
    return open(path, O_RDWR);
}

int main(int argc, char **rc_argv) {
    static char rc_shell[64] = "sh";
    static char rc_tty[32] = "auto";

    static cfg_opt_t rc_opts[] = {
        CFG_STR("shell", "sh", CFGF_NONE),
        CFG_BOOL("respawn", cfg_true, CFGF_NONE),
        CFG_BOOL("mount_services", cfg_true, CFGF_NONE),
        CFG_STR("tty", "auto", CFGF_NONE),
        CFG_END()
    };
    cfg_t *cfg = cfg_init(rc_opts, CFGF_NONE);
    if (cfg) {
        if (cfg_parse(cfg, RC_CONF_PATH) == CFG_SUCCESS) {
            const char *s = cfg_getstr(cfg, "shell");
            if (s) {
                strncpy(rc_shell, s, sizeof(rc_shell) - 1);
                rc_shell[sizeof(rc_shell) - 1] = '\0';
            }
            const char *t = cfg_getstr(cfg, "tty");
            if (t) {
                strncpy(rc_tty, t, sizeof(rc_tty) - 1);
                rc_tty[sizeof(rc_tty) - 1] = '\0';
            }
        }
    }
    if (argc > 1) {
        strncpy(rc_tty, rc_argv[1], sizeof(rc_tty) - 1);
        rc_tty[sizeof(rc_tty) - 1] = '\0';
    }

    char chosen[32] = "";
    int tty_fd = -1;
    if (strcmp(rc_tty, "auto") == 0) {
        static const char *const candidates[] = {
            "rstty1", "rstty2", "rstty3", "rstty4", "rstty5", "rstty6"
        };
        for (int i = 0; i < 6; i++) {
            int fd = try_open(candidates[i]);
            if (fd >= 0) {
                strncpy(chosen, candidates[i], sizeof(chosen) - 1);
                tty_fd = fd;
                break;
            }
        }
    } else {
        int fd = try_open(rc_tty);
        if (fd >= 0) {
            strncpy(chosen, rc_tty, sizeof(chosen) - 1);
            tty_fd = fd;
        }
    }

    if (chosen[0]) {
        setenv("ROBU_TTY_DEV", chosen, 1);
        printf("[tty_service] using /dev/%s\n", chosen);
    } else {
        printf("[tty_service] no tty interface available, running without a tty\n");
    }

    if (tty_fd >= 0) {
        setpgid(0, 0);
        dup2(tty_fd, 0);
        dup2(tty_fd, 1);
        dup2(tty_fd, 2);
        if (tty_fd > 2) {
            close(tty_fd);
        }
        tcsetpgrp(0, getpgrp());
    }

    char shell_path[80];
    snprintf(shell_path, sizeof(shell_path), "/bin/%s", rc_shell);
    setenv("SHELL", shell_path, 1);

    char *const argv[] = { rc_shell, NULL };
    execve(rc_shell, argv, environ);
    printf("[tty_service] failed to exec '%s'\n", rc_shell);
    return 1;
}
