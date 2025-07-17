#ifndef DAEMON_RAFT_H
#define DAEMON_RAFT_H

#include "raft.h"
#include <netinet/in.h>
#include <rpc/auth_unix.h>

extern char *opt_raft_log;
extern int opt_raft_id;
extern char *opt_raft_peers;

extern raft_server_t *raft_srv;

typedef struct raft_client_info {
    struct in6_addr addr;
    uint32_t uid;
    uint32_t gid;
    uint32_t gid_len;
    uint32_t gids[NGRPS];
} raft_client_info_t;

void raft_init(void);
void raft_net_receive(void);

#endif /* DAEMON_RAFT_H */
