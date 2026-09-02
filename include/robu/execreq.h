#ifndef ROBU_EXECREQ_H
#define ROBU_EXECREQ_H
#include "robu/types.h"
#include "robu/tcb.h"
#include "robu/spawn.h"

#define EXECREQ_MAX_SLOTS 8
#define EXECREQ_NAME_MAX 64
#define EXECREQ_STAGING_SIZE (16u * 1024u * 1024u)
#define SYS_INFO_CAT_EXECREQ_REPLY 42

typedef struct {
    tid_t requester_tid;
    int is_spawn_create;
    char name[EXECREQ_NAME_MAX];
    uint8_t strbuf[SPAWN_REQ_MAX_LEN];
    uint32_t strbuf_used;
    uint32_t argc;
    uint32_t argv_off[SPAWN_MAX_ARGS];
    uint32_t envc;
    uint32_t envp_off[SPAWN_MAX_ARGS];
    uint32_t nfds;
    robu_spawn_fd_t fds[SPAWN_FD_INFO_MAX];
    uint8_t prio;
    tid_t pager_tid;
} execreq_params_t;

int execreq_submit(tcb_t *cur, const execreq_params_t *params, uint32_t *out_slot);
int execreq_submit_poll(const execreq_params_t *params, uint32_t *out_slot, uint16_t *out_generation);
int execreq_poll_ready(uint32_t slot, uint16_t generation);
tcb_t *execreq_finish_poll_spawn(uint32_t slot, uint16_t generation);
int execreq_try_deliver_queued(tcb_t *ext2fs, uint32_t *out_slot, uint16_t *out_generation,
                                char *out_name, uint32_t out_name_max);
void execreq_complete(uint32_t slot, uint16_t generation, int status, int shmid, uint64_t size);
void execreq_cancel_requester(tid_t tid);
void execreq_fail_all(void);

#endif
