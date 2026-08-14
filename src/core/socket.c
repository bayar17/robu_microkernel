#include "robu/socket.h"
#include "robu/ipc.h"
#include "robu/arch.h"

#define MAX_SOCKETS 32
#define SOCK_BACKLOG_MAX 8
#define SOCK_RING_SIZE 4096

typedef enum {
    SOCK_UNBOUND = 0,
    SOCK_LISTENING,
    SOCK_CONNECTING,
    SOCK_CONNECTED,
} sock_state_t;

typedef struct {
    int in_use;
    uint16_t generation;
    sock_state_t state;
    tid_t owner_tid;
    char path[SOCK_PATH_MAX];
    int has_path;
    int backlog_slot[SOCK_BACKLOG_MAX];
    uint16_t backlog_gen[SOCK_BACKLOG_MAX];
    int backlog_head, backlog_tail;
    int peer_idx;
    uint16_t peer_gen;
    int peer_closed;
    uint8_t ring[SOCK_RING_SIZE];
    uint32_t ring_head, ring_count;
} socket_t;

static socket_t sockets[MAX_SOCKETS];

static int path_eq(const char *a, const char *b) {
    for (int i = 0; i < SOCK_PATH_MAX; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
        if (a[i] == '\0') {
            return 1;
        }
    }
    return 1;
}

static int make_sockid(int slot, uint16_t generation) {
    return (int)((uint32_t)slot | ((uint32_t)generation << 8));
}
static int sockid_slot(int sockid) {
    return sockid & 0xFF;
}
static uint16_t sockid_gen(int sockid) {
    return (uint16_t)((uint32_t)sockid >> 8);
}
static socket_t *find_socket(int sockid) {
    int slot = sockid_slot(sockid);
    if (slot < 0 || slot >= MAX_SOCKETS) {
        return NULL;
    }
    socket_t *s = &sockets[slot];
    if (!s->in_use || s->generation != sockid_gen(sockid)) {
        return NULL;
    }
    return s;
}
static int socket_slot_index(socket_t *s) {
    return (int)(s - sockets);
}

int sock_create(tid_t caller, int domain, int type, int *out_id) {
    (void)domain;
    (void)type;
    int slot = -1;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return IPC_ERR_NO_SPACE;
    }
    socket_t *s = &sockets[slot];
    s->in_use = 1;
    s->state = SOCK_UNBOUND;
    s->owner_tid = caller;
    s->has_path = 0;
    s->backlog_head = s->backlog_tail = 0;
    s->peer_idx = -1;
    s->peer_closed = 0;
    s->ring_head = s->ring_count = 0;
    *out_id = make_sockid(slot, s->generation);
    return IPC_ERR_NONE;
}

int sock_bind(tid_t caller, int sockid, const char *path) {
    socket_t *s = find_socket(sockid);
    if (!s || s->owner_tid != caller) {
        return IPC_ERR_NOT_FOUND;
    }
    if (s->state != SOCK_UNBOUND) {
        return IPC_ERR_INVALID;
    }
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].in_use && sockets[i].has_path && path_eq(sockets[i].path, path)) {
            return IPC_ERR_EXISTS;
        }
    }
    for (int i = 0; i < SOCK_PATH_MAX; i++) {
        s->path[i] = path[i];
        if (path[i] == '\0') {
            break;
        }
    }
    s->path[SOCK_PATH_MAX - 1] = '\0';
    s->has_path = 1;
    return IPC_ERR_NONE;
}

int sock_listen(tid_t caller, int sockid, int backlog) {
    socket_t *s = find_socket(sockid);
    if (!s || s->owner_tid != caller) {
        return IPC_ERR_NOT_FOUND;
    }
    if (!s->has_path || s->state != SOCK_UNBOUND) {
        return IPC_ERR_INVALID;
    }
    (void)backlog;
    s->state = SOCK_LISTENING;
    s->backlog_head = s->backlog_tail = 0;
    return IPC_ERR_NONE;
}

int sock_connect(tid_t caller, int sockid, const char *path) {
    socket_t *s = find_socket(sockid);
    if (!s || s->owner_tid != caller) {
        return IPC_ERR_NOT_FOUND;
    }
    if (s->state == SOCK_CONNECTED) {
        return IPC_ERR_NONE;
    }
    if (s->state == SOCK_CONNECTING) {
        if (s->peer_idx < 0) {
            return IPC_ERR_WOULDBLOCK;
        }
        return IPC_ERR_NONE;
    }
    if (s->state != SOCK_UNBOUND) {
        return IPC_ERR_INVALID;
    }
    socket_t *listener = NULL;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].in_use && sockets[i].state == SOCK_LISTENING &&
            sockets[i].has_path && path_eq(sockets[i].path, path)) {
            listener = &sockets[i];
            break;
        }
    }
    if (!listener) {
        return IPC_ERR_NOT_FOUND;
    }
    int next = (listener->backlog_tail + 1) % SOCK_BACKLOG_MAX;
    if (next == listener->backlog_head) {
        return IPC_ERR_NO_SPACE;
    }
    int my_slot = socket_slot_index(s);
    listener->backlog_slot[listener->backlog_tail] = my_slot;
    listener->backlog_gen[listener->backlog_tail] = s->generation;
    listener->backlog_tail = next;
    s->state = SOCK_CONNECTING;
    s->peer_idx = -1;
    return IPC_ERR_WOULDBLOCK;
}

int sock_accept(tid_t caller, int sockid, int *out_new_id) {
    socket_t *listener = find_socket(sockid);
    if (!listener || listener->owner_tid != caller) {
        return IPC_ERR_NOT_FOUND;
    }
    if (listener->state != SOCK_LISTENING) {
        return IPC_ERR_INVALID;
    }
    socket_t *client = NULL;
    while (listener->backlog_head != listener->backlog_tail) {
        int slot = listener->backlog_slot[listener->backlog_head];
        uint16_t gen = listener->backlog_gen[listener->backlog_head];
        listener->backlog_head = (listener->backlog_head + 1) % SOCK_BACKLOG_MAX;
        if (sockets[slot].in_use && sockets[slot].generation == gen &&
            sockets[slot].state == SOCK_CONNECTING) {
            client = &sockets[slot];
            break;
        }
    }
    if (!client) {
        return IPC_ERR_WOULDBLOCK;
    }
    int new_slot = -1;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].in_use) {
            new_slot = i;
            break;
        }
    }
    if (new_slot < 0) {
        return IPC_ERR_NO_SPACE;
    }
    socket_t *srv = &sockets[new_slot];
    srv->in_use = 1;
    srv->state = SOCK_CONNECTED;
    srv->owner_tid = caller;
    srv->has_path = 0;
    srv->peer_closed = 0;
    srv->ring_head = srv->ring_count = 0;
    int client_slot = socket_slot_index(client);
    srv->peer_idx = client_slot;
    srv->peer_gen = client->generation;
    client->peer_idx = new_slot;
    client->peer_gen = srv->generation;
    client->state = SOCK_CONNECTED;
    *out_new_id = make_sockid(new_slot, srv->generation);
    return IPC_ERR_NONE;
}

int sock_read(int sockid, uint8_t *out, int max, int *out_n) {
    socket_t *s = find_socket(sockid);
    if (!s) {
        return IPC_ERR_NOT_FOUND;
    }
    if (s->state != SOCK_CONNECTED) {
        return IPC_ERR_INVALID;
    }
    if (s->ring_count == 0) {
        *out_n = 0;
        return s->peer_closed ? IPC_ERR_NONE : IPC_ERR_WOULDBLOCK;
    }
    int n = 0;
    while (n < max && s->ring_count > 0) {
        out[n++] = s->ring[s->ring_head];
        s->ring_head = (s->ring_head + 1) % SOCK_RING_SIZE;
        s->ring_count--;
    }
    *out_n = n;
    return IPC_ERR_NONE;
}

int sock_write(int sockid, const uint8_t *buf, int len, int *out_n) {
    socket_t *s = find_socket(sockid);
    if (!s) {
        return IPC_ERR_NOT_FOUND;
    }
    if (s->state != SOCK_CONNECTED) {
        return IPC_ERR_INVALID;
    }
    if (s->peer_idx < 0 || !sockets[s->peer_idx].in_use ||
        sockets[s->peer_idx].generation != s->peer_gen) {
        return IPC_ERR_NOT_FOUND;
    }
    socket_t *peer = &sockets[s->peer_idx];
    int space = (int)(SOCK_RING_SIZE - peer->ring_count);
    int n = len < space ? len : space;
    uint32_t tail = (peer->ring_head + peer->ring_count) % SOCK_RING_SIZE;
    for (int i = 0; i < n; i++) {
        peer->ring[tail] = buf[i];
        tail = (tail + 1) % SOCK_RING_SIZE;
    }
    peer->ring_count += (uint32_t)n;
    *out_n = n;
    return IPC_ERR_NONE;
}

int sock_close(tid_t caller, int sockid) {
    socket_t *s = find_socket(sockid);
    if (!s || s->owner_tid != caller) {
        return IPC_ERR_NOT_FOUND;
    }
    if (s->state == SOCK_CONNECTED && s->peer_idx >= 0 &&
        sockets[s->peer_idx].in_use && sockets[s->peer_idx].generation == s->peer_gen) {
        sockets[s->peer_idx].peer_closed = 1;
    }
    s->in_use = 0;
    s->generation++;
    return IPC_ERR_NONE;
}

void sock_cleanup_for_process(tid_t tid) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        socket_t *s = &sockets[i];
        if (!s->in_use || s->owner_tid != tid) {
            continue;
        }
        if (s->state == SOCK_CONNECTED && s->peer_idx >= 0 &&
            sockets[s->peer_idx].in_use && sockets[s->peer_idx].generation == s->peer_gen) {
            sockets[s->peer_idx].peer_closed = 1;
        }
        s->in_use = 0;
        s->generation++;
    }
}
