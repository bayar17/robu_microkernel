#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <bits/winsize.h>
#include <unistd.h>
#include <pwd.h>
#define ROBU_TIOCGPGRP 0x540F
#define ROBU_TIOCSPGRP 0x5410
#define ROBU_TIOCGWINSZ 0x5413

static void fork_and_pipe_test(void) {
    int fds[2];
    if (pipe(fds) != 0) {
        printf("pipe() failed: %s\n", strerror(errno));
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        printf("fork() failed: %s\n", strerror(errno));
        return;
    }

    if (pid == 0) {
        close(fds[0]);
        char msg[64];
        int len = snprintf(msg, sizeof(msg), "hello from child pid=%d\n", (int)getpid());
        write(fds[1], msg, (size_t)len);
        close(fds[1]);
        _exit(42);
    }

    close(fds[1]);
    char buf[128];
    ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
    if (n < 0) {
        printf("read from pipe failed: %s\n", strerror(errno));
    } else {
        buf[n] = '\0';
        printf("parent read from child: %s", buf);
    }
    close(fds[0]);

    int status;
    pid_t reaped = waitpid(pid, &status, 0);
    printf("waitpid returned %d, child exit status=%d\n", (int)reaped,
           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

extern int __libc_spawn(const char *name, char *const argv[], char *const envp[]);
extern char **environ;

static void exec_test(void) {
    const char *exec_path = "/var/tmp/exec-test.txt";
    remove(exec_path);

    pid_t pid = fork();
    if (pid < 0) {
        printf("exec test: fork() failed: %s\n", strerror(errno));
        return;
    }

    if (pid == 0) {
        char *const argv[] = { "touch", (char *)exec_path, 0 };
        execve("touch", argv, environ);

        printf("exec test (child): execve() failed: %s\n", strerror(errno));
        fflush(stdout);
        _exit(123);
    }

    int status;
    pid_t reaped = waitpid(pid, &status, 0);
    printf("exec test: child (tid=%d) reaped=%d exit status=%d (expect 0)\n", (int)pid,
           (int)reaped, WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    struct stat st;
    printf("exec test: %s %s\n", exec_path,
           stat(exec_path, &st) == 0 ? "exists (execve really replaced the image)"
                                      : "MISSING (execve did not actually run touch)");
}

static void spawn_pipeline_test(void) {
    int fds[2];
    if (pipe(fds) != 0) {
        printf("pipeline: pipe() failed: %s\n", strerror(errno));
        return;
    }

    const char *msg = "piped through a real pipe\n";
    write(fds[1], msg, strlen(msg));
    close(fds[1]);

    int saved_stdin = dup(0);
    if (dup2(fds[0], 0) != 0) {
        printf("pipeline: dup2 failed: %s\n", strerror(errno));
        close(fds[0]);
        return;
    }
    close(fds[0]);

    char *const cat_argv[] = { "cat", 0 };
    int child = __libc_spawn("cat", cat_argv, environ);

    dup2(saved_stdin, 0);
    close(saved_stdin);

    if (child < 0) {
        printf("pipeline: spawn(cat) failed\n");
        return;
    }

    int status;
    pid_t reaped = waitpid(child, &status, 0);
    printf("pipeline: cat (tid=%d) reaped=%d status=%d\n", child, (int)reaped,
           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

static volatile sig_atomic_t sig_handler_hits = 0;

static void test_sig_handler(int signum) {
    if (signum == SIGUSR1) {
        sig_handler_hits++;
    }
}

static void sigaction_test(void) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = test_sig_handler;
    if (sigaction(SIGUSR1, &act, NULL) != 0) {
        printf("sigaction test: sigaction() failed: %s\n", strerror(errno));
        return;
    }

    raise(SIGUSR1);
    printf("sigaction test: after raise(), handler hits=%d (expect 1)\n", sig_handler_hits);

    sigset_t block_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block_set, NULL);

    raise(SIGUSR1);
    printf("sigaction test: after raise() while blocked, handler hits=%d (expect still 1)\n",
           sig_handler_hits);

    sigset_t pending;
    sigpending(&pending);
    printf("sigaction test: sigismember(pending, SIGUSR1)=%d (expect 1)\n",
           sigismember(&pending, SIGUSR1));

    sigprocmask(SIG_UNBLOCK, &block_set, NULL);
    printf("sigaction test: after unblock, handler hits=%d (expect 2)\n", sig_handler_hits);
}

static void termios_pgrp_test(void) {
    struct termios t;
    if (tcgetattr(0, &t) != 0) {
        printf("termios test: tcgetattr failed: %s\n", strerror(errno));
        return;
    }
    printf("termios test: initial ICANON=%d ECHO=%d (expect both 1, cooked by default)\n",
           (t.c_lflag & ICANON) != 0, (t.c_lflag & ECHO) != 0);

    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    if (tcsetattr(0, TCSANOW, &t) != 0) {
        printf("termios test: tcsetattr(raw) failed: %s\n", strerror(errno));
        return;
    }
    struct termios t2;
    tcgetattr(0, &t2);
    printf("termios test: after raw tcsetattr, ICANON=%d (expect 0)\n", (t2.c_lflag & ICANON) != 0);

    t.c_lflag |= ICANON | ECHO;
    tcsetattr(0, TCSANOW, &t);
    tcgetattr(0, &t2);
    printf("termios test: after restoring cooked, ICANON=%d (expect 1)\n", (t2.c_lflag & ICANON) != 0);

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    if (ioctl(0, ROBU_TIOCGWINSZ, &ws) != 0) {
        printf("termios test: ioctl(TIOCGWINSZ) failed: %s\n", strerror(errno));
    } else {
        printf("termios test: winsize row=%d col=%d (expect 25x80)\n", ws.ws_row, ws.ws_col);
    }

    pid_t me = getpid();
    if (setpgid(0, 0) != 0) {
        printf("pgid test: setpgid failed: %s\n", strerror(errno));
    }
    pid_t pg = getpgid(0);
    printf("pgid test: getpgid=%d self=%d (expect equal)\n", (int)pg, (int)me);

    pid_t sid = setsid();
    printf("sid test: setsid returned %d self=%d (expect equal)\n", (int)sid, (int)me);

    int fg_arg = (int)me;
    if (ioctl(0, ROBU_TIOCSPGRP, &fg_arg) != 0) {
        printf("pgrp test: ioctl(TIOCSPGRP) failed: %s\n", strerror(errno));
    }
    int fg_read = -1;
    ioctl(0, ROBU_TIOCGPGRP, &fg_read);
    printf("pgrp test: tcgetpgrp=%d self=%d (expect equal)\n", fg_read, (int)me);
}

static void root_account_test(void) {
    printf("root account test: getuid=%d geteuid=%d (expect both 0)\n",
           (int)getuid(), (int)geteuid());

    const char *home = getenv("HOME");
    const char *user = getenv("USER");
    printf("root account test: HOME=%s USER=%s (expect /var/root, root)\n",
           home ? home : "(null)", user ? user : "(null)");

    struct passwd *pw = getpwuid(getuid());
    if (!pw) {
        printf("root account test: getpwuid(0) failed: %s\n", strerror(errno));
    } else {
        printf("root account test: pw_name=%s pw_dir=%s pw_shell=%s (expect root, /var/root, /bin/sh)\n",
               pw->pw_name, pw->pw_dir, pw->pw_shell);
    }

    struct stat st;
    if (stat("/var/root", &st) != 0) {
        printf("root account test: stat(/var/root) failed: %s\n", strerror(errno));
    } else {
        printf("root account test: /var/root exists, is_dir=%d\n", S_ISDIR(st.st_mode));
    }
}

int main(int argc, char **argv) {
    printf("hello from mlibc on robu\n");
    for (int i = 0; i < argc; i++) {
        printf("argv[%d] = %s\n", i, argv[i]);
    }

    const char *path = "/var/tmp/mlibc-hello.txt";
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("fopen(w) failed: %s\n", strerror(errno));
        return 1;
    }
    fputs("hello from mlibc file io\n", f);
    fclose(f);

    struct stat st;
    if (stat(path, &st) != 0) {
        printf("stat failed: %s\n", strerror(errno));
        return 1;
    }
    printf("stat: size=%ld mode=0%o\n", (long)st.st_size, (unsigned)st.st_mode & 0777);

    f = fopen(path, "r");
    if (!f) {
        printf("fopen(r) failed: %s\n", strerror(errno));
        return 1;
    }
    char line[128];
    if (fgets(line, sizeof(line), f)) {
        printf("read back: %s", line);
    }
    fclose(f);

    DIR *d = opendir("/var/tmp");
    if (!d) {
        printf("opendir failed: %s\n", strerror(errno));
        return 1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        printf("entry: %s\n", de->d_name);
    }
    closedir(d);

    char cwd[64];
    if (getcwd(cwd, sizeof(cwd))) {
        printf("cwd: %s\n", cwd);
    }

    fork_and_pipe_test();
    spawn_pipeline_test();
    exec_test();
    sigaction_test();
    termios_pgrp_test();
    root_account_test();

    printf("mlibc-hello: all checks done\n");
    return 0;
}
