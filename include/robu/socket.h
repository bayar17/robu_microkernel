#ifndef ROBU_SOCKET_H
#define ROBU_SOCKET_H
#include "robu/types.h"

#define SOCK_PATH_MAX 32

int sock_create(tid_t caller, int domain, int type, int *out_id);
int sock_bind(tid_t caller, int sockid, const char *path);
int sock_listen(tid_t caller, int sockid, int backlog);
int sock_connect(tid_t caller, int sockid, const char *path);
int sock_accept(tid_t caller, int sockid, int *out_new_id);
int sock_read(int sockid, uint8_t *out, int max, int *out_n);
int sock_write(int sockid, const uint8_t *buf, int len, int *out_n);
int sock_close(tid_t caller, int sockid);
void sock_cleanup_for_process(tid_t tid);

#endif
