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


// some helper functions for debugging
const char *nfs3_proc_name(u_long proc); 
const char *mountproc_name(u_long proc); 
void print_buffer_hex(const void *buf, size_t len, const char *label);

/* Wait for leader election before starting NFS services */
void wait_for_leader(void);

// Implementation for raft_serialize_and_replicate_nfs_op
    
void raft_serialize_and_replicate_nfs_op(struct svc_req *rqstp, 
                                         struct in6_addr remote_addr, 
                                         xdrproc_t _xdr_argument, 
                                         void *argument);

#endif /* DAEMON_RAFT_H */
