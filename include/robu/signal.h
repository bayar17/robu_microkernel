#ifndef ROBU_SIGNAL_H
#define ROBU_SIGNAL_H
#include "robu/types.h"
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPWR    30
#define SIGSYS    31
#define ROBU_NSIG 64
#define ROBU_SIG_DFL 0
#define ROBU_SIG_IGN 1
#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO   4
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
typedef struct {
    uint64_t handler;
    uint64_t flags;
    uint64_t mask;
} sig_action_t;
#define SIGTRAMPOLINE_VA 0x0000000080001000ULL
#define SYS_INFO_CAT_SIGACTION   5
#define SYS_INFO_CAT_KILL        6
#define SYS_INFO_CAT_SIGPROCMASK 7
#define SYS_INFO_CAT_SIGRETURN   8
#define SYS_INFO_CAT_SIGPENDING  9
struct tcb;
void sig_trampoline_init(void);
void sig_try_deliver(struct tcb *t);
int sig_send(tid_t target, int signum);
#endif
