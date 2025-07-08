#ifndef DAEMON_RAFT_H
#define DAEMON_RAFT_H

#include "raft.h"

extern char *opt_raft_log;
extern int opt_raft_id;
extern char *opt_raft_peers;

extern raft_server_t *raft_srv;

void raft_init(void);
void raft_net_receive(void);

#endif /* DAEMON_RAFT_H */
