#include "config.h"

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <syslog.h>
#include <arpa/inet.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <endian.h>
#include <stdint.h>

#include "daemon.h"
#include "raft_log.h"
#include "raft.h"
#include "daemon_raft.h"
#include "nfs.h"
#include "mount.h"
#include "fh.h"
#include "Config/exports.h"
#include "xdr.h"
#include <rpc/rpc.h>
#include <rpc/auth_unix.h>

char *opt_raft_log = "raft.log";
int opt_raft_id = 1;
char *opt_raft_peers = NULL;

static raft_cbs_t raft_cbs;
raft_server_t *raft_srv = NULL;

#define RAFT_PORT_BASE 7000
#define RAFT_MAX_PKT 20480
#define FH_MAXBUF2 64

static int raft_sock = -1;

struct raft_peer {
    int id;
    struct sockaddr_in addr;
    raft_node_t *node;
};

static struct raft_peer raft_peers[16];
static int raft_peer_count = 0;



static struct raft_peer* raft_peer_from_addr(struct sockaddr_in* addr) {
    for (int i = 0; i < raft_peer_count; i++)
        if (addr->sin_port == raft_peers[i].addr.sin_port && addr->sin_addr.s_addr == raft_peers[i].addr.sin_addr.s_addr)
            return &raft_peers[i];
    return NULL;
}


const char *nfs3_proc_name(u_long proc)
{
    switch (proc) {
        case NFSPROC3_NULL: return "NULL";
        case NFSPROC3_GETATTR: return "GETATTR";
        case NFSPROC3_SETATTR: return "SETATTR";
        case NFSPROC3_LOOKUP: return "LOOKUP";
        case NFSPROC3_ACCESS: return "ACCESS";
        case NFSPROC3_READLINK: return "READLINK";
        case NFSPROC3_READ: return "READ";
        case NFSPROC3_WRITE: return "WRITE";
        case NFSPROC3_CREATE: return "CREATE";
        case NFSPROC3_MKDIR: return "MKDIR";
        case NFSPROC3_SYMLINK: return "SYMLINK";
        case NFSPROC3_MKNOD: return "MKNOD";
        case NFSPROC3_REMOVE: return "REMOVE";
        case NFSPROC3_RMDIR: return "RMDIR";
        case NFSPROC3_RENAME: return "RENAME";
        case NFSPROC3_LINK: return "LINK";
        case NFSPROC3_READDIR: return "READDIR";
        case NFSPROC3_READDIRPLUS: return "READDIRPLUS";
        case NFSPROC3_FSSTAT: return "FSSTAT";
        case NFSPROC3_FSINFO: return "FSINFO";
        case NFSPROC3_PATHCONF: return "PATHCONF";
        case NFSPROC3_COMMIT: return "COMMIT";
        default: return "UNKNOWN";
    }
}

const char *mountproc_name(u_long proc)
{
    switch (proc) {
        case MOUNTPROC_NULL: return "NULL";
        case MOUNTPROC_MNT: return "MNT";
        case MOUNTPROC_DUMP: return "DUMP";
        case MOUNTPROC_UMNT: return "UMNT";
        case MOUNTPROC_UMNTALL: return "UMNTALL";
        case MOUNTPROC_EXPORT: return "EXPORT";
        default: return "UNKNOWN";
    }
}

// Prints the contents of 'buf' as hex, 'len' bytes, with 'bytes_per_line' bytes per line.
void print_buffer_hex(const void *buf, size_t len, const char *label)
{
    return;


    const int bytes_per_line = 16; // Change this to adjust how many bytes per line

    const unsigned char *p = (const unsigned char *)buf;
    if (label)
        fprintf(stderr, "%s (len=%zu):\n", label, len);

    for (size_t i = 0; i < len; ++i) {
        fprintf(stderr, "%02x ", p[i]);
        if ((i + 1) % bytes_per_line == 0) {
            // Optionally print ASCII representation
            fprintf(stderr, " | ");
            for (size_t j = i + 1 - bytes_per_line; j <= i; ++j)
                fprintf(stderr, "%c", isprint(p[j]) ? p[j] : '.');
            fprintf(stderr, "\n");
        }
    }

    // Handle last line if it's not a full line
    if (len % bytes_per_line) {
        int pad = (bytes_per_line - (len % bytes_per_line)) * 3;
        for (int i = 0; i < pad; ++i)
            fprintf(stderr, " ");
        fprintf(stderr, " | ");
        for (size_t j = len - (len % bytes_per_line); j < len; ++j)
            fprintf(stderr, "%c", isprint(p[j]) ? p[j] : '.');
        fprintf(stderr, "\n");
    }

    fflush(stderr); 
}

/* Wait for leader election before starting NFS services */
void wait_for_leader(void)
{
    logmsg(LOG_INFO, "waiting for Raft leader election");
    
    while (raft_get_current_leader(raft_srv) == -1) {
        raft_periodic(raft_srv, mytimout);
        raft_net_receive();
        usleep(mytimout * 1000); 
    }

    int leader = raft_get_current_leader(raft_srv);
    logmsg(LOG_INFO, "Raft leader elected: %d", leader);
    
    if (raft_is_leader(raft_srv)) {
        logmsg(LOG_INFO, "This node is the leader; binding to port 2049");
    } else {
        logmsg(LOG_INFO, "This node is a follower; waiting for leader to send requests");
    } 
}

void print_leader_info(void){
    if (raft_is_leader(raft_srv)) {
        logmsg(LOG_INFO, "I am the leader, processing raft events");
    } else if (raft_is_follower(raft_srv)) {
        logmsg(LOG_INFO, "I am a follower, the current leader is %d",
            raft_get_current_leader(raft_srv));
    } else {
        logmsg(LOG_INFO, "I am a candidate, waiting for votes");
        int voted_for = raft_get_voted_for(raft_srv);

        if (voted_for != -1)
            logmsg(LOG_INFO, "I voted for %d", voted_for);
        else
            logmsg(LOG_INFO, "I have not voted yet");
    }
}


int raft_serialize_and_replicate_nfs_op(struct svc_req *rqstp, 
                                         struct in6_addr remote_addr, 
                                         xdrproc_t _xdr_argument, 
                                         void *argument) {
    if (raft_is_leader(raft_srv)) {
        // flush any pending logs
        raft_apply_all(raft_srv);


        u_int arg_len = xdr_sizeof(_xdr_argument, (char *)argument);
        if (arg_len > 0) {
            size_t info_size = sizeof(raft_client_info_t);
            size_t proc_size = sizeof(uint32_t);
            size_t total_size = proc_size + info_size + arg_len;

            char *buf = malloc(total_size);
            if (buf) {
                size_t offset = 0;

                uint32_t proc = htonl(rqstp->rq_proc);
                memcpy(buf + offset, &proc, proc_size);
                offset += proc_size;

                raft_client_info_t info = {0};
                info.addr = remote_addr;
                if (rqstp->rq_cred.oa_flavor == AUTH_UNIX) {
                    struct authunix_parms *auth = (struct authunix_parms *)rqstp->rq_clntcred;
                    info.uid = auth->aup_uid;
                    info.gid = auth->aup_gid;
                    info.gid_len = auth->aup_len;
                    for (u_int i = 0; i < auth->aup_len && i < NGRPS; i++)
                        info.gids[i] = auth->aup_gids[i];
                }

                int written = raft_client_info_serialize(&info, (uint8_t *)(buf + offset), info_size);
                if (written != info_size) {
                    logmsg(LOG_ERR, "Serialization of raft_client_info_t failed");
                    free(buf);
                    return;
                }


                offset += info_size;

                XDR xdrs;
                xdrmem_create(&xdrs, buf + offset, arg_len, XDR_ENCODE);
                if (((xdrproc_t)_xdr_argument)(&xdrs, (char *)argument)) {
                    msg_entry_t ety = {0};
                    ety.data.buf = buf;
                    ety.data.len = offset + arg_len;
                    ety.id = raft_get_current_idx(raft_srv) + 1;
                    ety.type = RAFT_LOGTYPE_NORMAL;

                    msg_entry_response_t resp;
                    if (0 == raft_recv_entry(raft_srv, &ety, &resp)) {
                        logmsg(LOG_CRIT, "Logged operation %s as log idx %ld",
                               nfs3_proc_name(rqstp->rq_proc), resp.idx);
                        
                        while (raft_get_commit_idx(raft_srv) < resp.idx) {
                            if (raft_disabled){
                                return 1; 
                            }
                            sleep(mytimout / 1000);
                            raft_periodic(raft_srv, mytimout);
                            raft_net_receive();
                        }
                    }
                }
                xdr_destroy(&xdrs);
                
                // NOTE: Freeing the buffer at this point will cause the operations 
                // to be received incorrectly by the followers. Can't fully understand why. 
                 
                // free(buf); 
            }
        }
        raft_apply_all(raft_srv);
    } else {
        logmsg(LOG_CRIT, "Not the leader, cannot replicate operation %s",
                nfs3_proc_name(rqstp->rq_proc));
        return 1;
    }
}



////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////

// --- Serialization ---
size_t serialize_requestvote(const msg_requestvote_t* msg, uint8_t* buf) {
    size_t offset = 0;

    int64_t net_term = htobe64((int64_t)msg->term);
    memcpy(buf + offset, &net_term, sizeof(net_term));
    offset += sizeof(net_term);

    int32_t net_candidate_id = htonl((int32_t)msg->candidate_id);
    memcpy(buf + offset, &net_candidate_id, sizeof(net_candidate_id));
    offset += sizeof(net_candidate_id);

    int64_t net_last_log_idx = htobe64((int64_t)msg->last_log_idx);
    memcpy(buf + offset, &net_last_log_idx, sizeof(net_last_log_idx));
    offset += sizeof(net_last_log_idx);

    int64_t net_last_log_term = htobe64((int64_t)msg->last_log_term);
    memcpy(buf + offset, &net_last_log_term, sizeof(net_last_log_term));
    offset += sizeof(net_last_log_term);

    return offset;
}

// --- Deserialization ---
size_t deserialize_requestvote(const uint8_t* buf, msg_requestvote_t* msg) {
    size_t offset = 0;

    int64_t net_term;
    memcpy(&net_term, buf + offset, sizeof(net_term));
    msg->term = (raft_term_t)be64toh(net_term);
    offset += sizeof(net_term);

    int32_t net_candidate_id;
    memcpy(&net_candidate_id, buf + offset, sizeof(net_candidate_id));
    msg->candidate_id = (raft_node_id_t)ntohl(net_candidate_id);
    offset += sizeof(net_candidate_id);

    int64_t net_last_log_idx;
    memcpy(&net_last_log_idx, buf + offset, sizeof(net_last_log_idx));
    msg->last_log_idx = (raft_index_t)be64toh(net_last_log_idx);
    offset += sizeof(net_last_log_idx);

    int64_t net_last_log_term;
    memcpy(&net_last_log_term, buf + offset, sizeof(net_last_log_term));
    msg->last_log_term = (raft_term_t)be64toh(net_last_log_term);
    offset += sizeof(net_last_log_term);

    return offset;
}

size_t serialize_requestvote_response(const msg_requestvote_response_t* msg, uint8_t* buf) {
    size_t offset = 0;

    int64_t net_term = htobe64((int64_t)msg->term);
    memcpy(buf + offset, &net_term, sizeof(net_term));
    offset += sizeof(net_term);

    int32_t net_vote_granted = htonl((int32_t)msg->vote_granted);
    memcpy(buf + offset, &net_vote_granted, sizeof(net_vote_granted));
    offset += sizeof(net_vote_granted);

    return offset;
}

size_t deserialize_requestvote_response(const uint8_t* buf, msg_requestvote_response_t* msg) {
    size_t offset = 0;

    int64_t net_term;
    memcpy(&net_term, buf + offset, sizeof(net_term));
    msg->term = (raft_term_t)be64toh(net_term);
    offset += sizeof(net_term);

    int32_t net_vote_granted;
    memcpy(&net_vote_granted, buf + offset, sizeof(net_vote_granted));
    msg->vote_granted = (int)ntohl(net_vote_granted);
    offset += sizeof(net_vote_granted);

    return offset;
}

size_t serialize_appendentries(const msg_appendentries_t* msg, uint8_t* buf) {
    size_t offset = 0;

    int64_t net_term = htobe64((int64_t)msg->term);
    memcpy(buf + offset, &net_term, sizeof(net_term));
    offset += sizeof(net_term);

    int64_t net_prev_log_idx = htobe64((int64_t)msg->prev_log_idx);
    memcpy(buf + offset, &net_prev_log_idx, sizeof(net_prev_log_idx));
    offset += sizeof(net_prev_log_idx);

    int64_t net_prev_log_term = htobe64((int64_t)msg->prev_log_term);
    memcpy(buf + offset, &net_prev_log_term, sizeof(net_prev_log_term));
    offset += sizeof(net_prev_log_term);

    int64_t net_leader_commit = htobe64((int64_t)msg->leader_commit);
    memcpy(buf + offset, &net_leader_commit, sizeof(net_leader_commit));
    offset += sizeof(net_leader_commit);

    int32_t net_n_entries = htonl((int32_t)msg->n_entries);
    memcpy(buf + offset, &net_n_entries, sizeof(net_n_entries));
    offset += sizeof(net_n_entries);

    return offset;
}

size_t deserialize_appendentries(const uint8_t* buf, msg_appendentries_t* msg) {
    size_t offset = 0;

    int64_t net_term;
    memcpy(&net_term, buf + offset, sizeof(net_term));
    msg->term = (raft_term_t)be64toh(net_term);
    offset += sizeof(net_term);

    int64_t net_prev_log_idx;
    memcpy(&net_prev_log_idx, buf + offset, sizeof(net_prev_log_idx));
    msg->prev_log_idx = (raft_index_t)be64toh(net_prev_log_idx);
    offset += sizeof(net_prev_log_idx);

    int64_t net_prev_log_term;
    memcpy(&net_prev_log_term, buf + offset, sizeof(net_prev_log_term));
    msg->prev_log_term = (raft_term_t)be64toh(net_prev_log_term);
    offset += sizeof(net_prev_log_term);

    int64_t net_leader_commit;
    memcpy(&net_leader_commit, buf + offset, sizeof(net_leader_commit));
    msg->leader_commit = (raft_index_t)be64toh(net_leader_commit);
    offset += sizeof(net_leader_commit);

    int32_t net_n_entries;
    memcpy(&net_n_entries, buf + offset, sizeof(net_n_entries));
    msg->n_entries = (int)ntohl(net_n_entries);
    offset += sizeof(net_n_entries);

    msg->entries = NULL;

    return offset;
}

size_t serialize_appendentries_response(const msg_appendentries_response_t* msg, uint8_t* buf) {
    size_t offset = 0;

    int64_t net_term = htobe64((int64_t)msg->term);
    memcpy(buf + offset, &net_term, sizeof(net_term));
    offset += sizeof(net_term);

    int32_t net_success = htonl((int32_t)msg->success);
    memcpy(buf + offset, &net_success, sizeof(net_success));
    offset += sizeof(net_success);

    int64_t net_current_idx = htobe64((int64_t)msg->current_idx);
    memcpy(buf + offset, &net_current_idx, sizeof(net_current_idx));
    offset += sizeof(net_current_idx);

    int64_t net_first_idx = htobe64((int64_t)msg->first_idx);
    memcpy(buf + offset, &net_first_idx, sizeof(net_first_idx));
    offset += sizeof(net_first_idx);

    return offset;
}

size_t deserialize_appendentries_response(const uint8_t* buf, msg_appendentries_response_t* msg) {
    size_t offset = 0;

    int64_t net_term;
    memcpy(&net_term, buf + offset, sizeof(net_term));
    msg->term = (raft_term_t)be64toh(net_term);
    offset += sizeof(net_term);

    int32_t net_success;
    memcpy(&net_success, buf + offset, sizeof(net_success));
    msg->success = (int)ntohl(net_success);
    offset += sizeof(net_success);

    int64_t net_current_idx;
    memcpy(&net_current_idx, buf + offset, sizeof(net_current_idx));
    msg->current_idx = (raft_index_t)be64toh(net_current_idx);
    offset += sizeof(net_current_idx);

    int64_t net_first_idx;
    memcpy(&net_first_idx, buf + offset, sizeof(net_first_idx));
    msg->first_idx = (raft_index_t)be64toh(net_first_idx);
    offset += sizeof(net_first_idx);

    return offset;
}

size_t serialize_msg_entry(const msg_entry_t* entry, uint8_t* buf) {
    size_t offset = 0;

    int64_t net_term = htobe64((int64_t)entry->term);
    memcpy(buf + offset, &net_term, sizeof(net_term));
    offset += sizeof(net_term);

    int64_t net_id = htobe64((int64_t)entry->id);
    memcpy(buf + offset, &net_id, sizeof(net_id));
    offset += sizeof(net_id);

    int32_t net_type = htonl(entry->type);
    memcpy(buf + offset, &net_type, sizeof(net_type));
    offset += sizeof(net_type);

    int32_t net_len = htonl((int32_t)entry->data.len);
    memcpy(buf + offset, &net_len, sizeof(net_len));
    offset += sizeof(net_len);

    if (entry->data.len > 0 && entry->data.buf) {
        memcpy(buf + offset, entry->data.buf, entry->data.len);
        offset += entry->data.len;
    }

    return offset;
}

size_t deserialize_msg_entry(const uint8_t* buf, msg_entry_t* entry) {
    size_t offset = 0;

    int64_t net_term;
    memcpy(&net_term, buf + offset, sizeof(net_term));
    entry->term = (raft_term_t)be64toh(net_term);
    offset += sizeof(net_term);

    int64_t net_id;
    memcpy(&net_id, buf + offset, sizeof(net_id));
    entry->id = (raft_entry_id_t)be64toh(net_id);
    offset += sizeof(net_id);

    int32_t net_type;
    memcpy(&net_type, buf + offset, sizeof(net_type));
    entry->type = (int)ntohl(net_type);
    offset += sizeof(net_type);

    int32_t net_len;
    memcpy(&net_len, buf + offset, sizeof(net_len));
    entry->data.len = (unsigned int)ntohl(net_len);
    offset += sizeof(net_len);

    // print_buffer_hex(buf + offset, entry->data.len, "Deserializing entry data");

    if (entry->data.len > 0) {
        entry->data.buf = malloc(entry->data.len);
        if (!entry->data.buf) {
            entry->data.len = 0;
            return offset;
        }
        memcpy(entry->data.buf, buf + offset, entry->data.len);
        offset += entry->data.len;
    } else {
        entry->data.buf = NULL;
    }

    return offset;
}

size_t serialize_msg_entry_array(const msg_entry_t* entries, int n, uint8_t* buf) {
    size_t offset = 0;
    for (int i = 0; i < n; ++i) {
        offset += serialize_msg_entry(&entries[i], buf + offset);
    }
    return offset;
}

size_t deserialize_msg_entry_array(const uint8_t* buf, int n, msg_entry_t* entries) {
    size_t offset = 0;
    for (int i = 0; i < n; ++i) {
        // logmsg(LOG_DEBUG, "Deserializing entry %d", i);

        offset += deserialize_msg_entry(buf + offset, &entries[i]);
    }
    return offset;
}


int raft_client_info_serialize(const raft_client_info_t* info, uint8_t* buf, size_t bufsize) {
    size_t offset = 0;

    memcpy(buf + offset, &info->addr, sizeof(info->addr));
    offset += sizeof(info->addr);

    uint32_t net_uid = htonl(info->uid);
    uint32_t net_gid = htonl(info->gid);
    uint32_t net_gid_len = htonl(info->gid_len);

    memcpy(buf + offset, &net_uid, sizeof(net_uid));
    offset += sizeof(net_uid);
    memcpy(buf + offset, &net_gid, sizeof(net_gid));
    offset += sizeof(net_gid);
    memcpy(buf + offset, &net_gid_len, sizeof(net_gid_len));
    offset += sizeof(net_gid_len);

    for (size_t i = 0; i < NGRPS; ++i) {
        uint32_t net_gid_i = htonl(info->gids[i]);
        memcpy(buf + offset, &net_gid_i, sizeof(net_gid_i));
        offset += sizeof(net_gid_i);
    }

    return (int)offset; // Number of bytes written
}


int raft_client_info_deserialize(raft_client_info_t* info, const uint8_t* buf, size_t bufsize) {
    size_t offset = 0;

    memcpy(&info->addr, buf + offset, sizeof(info->addr));
    offset += sizeof(info->addr);

    uint32_t net_uid, net_gid, net_gid_len;
    memcpy(&net_uid, buf + offset, sizeof(net_uid));
    offset += sizeof(net_uid);
    memcpy(&net_gid, buf + offset, sizeof(net_gid));
    offset += sizeof(net_gid);
    memcpy(&net_gid_len, buf + offset, sizeof(net_gid_len));
    offset += sizeof(net_gid_len);

    info->uid = ntohl(net_uid);
    info->gid = ntohl(net_gid);
    info->gid_len = ntohl(net_gid_len);

    for (size_t i = 0; i < NGRPS; ++i) {
        uint32_t net_gid_i;
        memcpy(&net_gid_i, buf + offset, sizeof(net_gid_i));
        offset += sizeof(net_gid_i);
        info->gids[i] = ntohl(net_gid_i);
    }

    return (int)offset; // Number of bytes read
}

///////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////
//// Network communication for Raft 
//// receiving messages between peers.
//// sends back proper responses.
///////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////


static void handle_requestvote(const struct raft_peer* peer, 
                          char* ptr, 
                          struct sockaddr_in* src, 
                          socklen_t slen);

static void handle_requestvote_response(const struct raft_peer* peer, 
                                   char* ptr);

static void handle_appendentries(const struct raft_peer* peer, 
                            char* ptr, 
                            struct sockaddr_in* src, 
                            socklen_t slen);   

static void handle_appendentries_response(const struct raft_peer* peer, 
                                          char* ptr);   

// Main dispatcher:
void raft_net_receive(void) {
    if (raft_disabled)
        return;

    char buf[RAFT_MAX_PKT];
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    ssize_t n;
    while ((n = recvfrom(raft_sock, buf, sizeof(buf), MSG_DONTWAIT, 
                         (struct sockaddr*)&src, &slen)) > 0) {
        logmsg(LOG_DEBUG, "raft: received %zd bytes from %s:%d", 
                          n, inet_ntoa(src.sin_addr), ntohs(src.sin_port));

        struct raft_peer* peer = raft_peer_from_addr(&src);
        if (!peer)
            continue;

        uint8_t type = buf[0];
        char* ptr = buf + 1;

        switch (type) {
            case 1:
                handle_requestvote(peer, ptr, &src, slen);
                break;
            case 2:
                handle_requestvote_response(peer, ptr);
                break;
            case 3:
                handle_appendentries(peer, ptr, &src, slen);
                break;
            case 4:
                handle_appendentries_response(peer, ptr);
                break;
            default:
                logmsg(LOG_WARNING, "raft: received unknown message type %d from %s:%d", 
                                    type, inet_ntoa(src.sin_addr), ntohs(src.sin_port));
                break;
        }
    }
}

// ---- Type handlers ----

static void handle_requestvote(const struct raft_peer* peer, 
                               char* ptr, 
                               struct sockaddr_in* src, 
                               socklen_t slen) {

    logmsg(LOG_DEBUG, "raft: received requestvote from peerid=%d, %s:%d", peer->id, inet_ntoa(src->sin_addr), ntohs(src->sin_port));

    msg_requestvote_t req;
    deserialize_requestvote(ptr, &req);

    logmsg(LOG_DEBUG, "raft: received requestvote from peerid=%d, term %ld, candidate %d, last_log_idx %ld, last_log_term %ld", peer->id, req.term, req.candidate_id, req.last_log_idx, req.last_log_term);

    msg_requestvote_response_t resp;
    raft_recv_requestvote(raft_srv, peer->node, &req, &resp);

    char resp_buf[RAFT_MAX_PKT];
    size_t len = 0;

    resp_buf[len++] = 2;
    len += serialize_requestvote_response(&resp, resp_buf + len);

    sendto(raft_sock, resp_buf, len, 0, (struct sockaddr*)src, slen);

    logmsg(LOG_DEBUG, "raft: sent requestvote response to %d, term %ld, vote %d", peer->id, resp.term, resp.vote_granted);
}


static void handle_requestvote_response(const struct raft_peer* peer, 
                                        char* ptr) {
    logmsg(LOG_DEBUG, "raft: received requestvote response from peerid=%d", peer->id);

    msg_requestvote_response_t r;
    deserialize_requestvote_response(ptr, &r);

    logmsg(LOG_DEBUG, "raft: received requestvote response from peerid=%d, term %ld, vote %d", peer->id, r.term, r.vote_granted);

    raft_recv_requestvote_response(raft_srv, peer->node, &r);
}


static void handle_appendentries(const struct raft_peer* peer, 
                                 char* ptr, 
                                 struct sockaddr_in* src, 
                                 socklen_t slen) {

    logmsg(LOG_DEBUG, "raft: received appendentries from %s:%d", inet_ntoa(src->sin_addr), ntohs(src->sin_port));

    msg_appendentries_t ae;
    size_t offset = 0;

    // print_buffer_hex(ptr, 200, "raft: received appendentries");

    offset += deserialize_appendentries(ptr, &ae);

    // print_buffer_hex(ptr + offset, 200, "raft: appendentries after header");

    msg_entry_t *entries = NULL;
    if (ae.n_entries > 0) {
        // logmsg(LOG_DEBUG, "raft: going to deserialize %d entries, 29038409238409", ae.n_entries);

        entries = calloc(ae.n_entries, sizeof(msg_entry_t));
        if (!entries) {
            logmsg(LOG_ERR, "raft: failed to allocate entries array");
            return;
        }
        offset += deserialize_msg_entry_array(ptr + offset, ae.n_entries, entries);
        ae.entries = entries;
    } else {
        ae.entries = NULL;
    }

    msg_appendentries_response_t resp;
    logmsg(LOG_DEBUG, "raft: received appendentries from peerid=%d, term %ld, prev_log_idx %ld, prev_log_term %ld, leader_commit %ld, n_entries %d", 
                          peer->id, ae.term, ae.prev_log_idx, ae.prev_log_term, ae.leader_commit, ae.n_entries);
    raft_recv_appendentries(raft_srv, peer->node, &ae, &resp);
    
    logmsg(LOG_DEBUG, "raft: appendentries response: term %ld, success %d, current_idx %ld, first_idx %ld", 
                          resp.term, resp.success, resp.current_idx, resp.first_idx);

    // for (int i = 0; i < ae.n_entries; ++i) {
    //     if (entries[i].data.buf) free(entries[i].data.buf);
    // }
    // free(entries);

    uint8_t resp_buf[64];
    size_t resp_len = 0;
    resp_buf[resp_len++] = 4;
    resp_len += serialize_appendentries_response(&resp, resp_buf + resp_len);

    sendto(raft_sock, resp_buf, resp_len, 0, (struct sockaddr*)src, slen);

    logmsg(LOG_DEBUG, "raft: sent appendentries response to %d, term %ld, success %d", peer->id, resp.term, resp.success);
}

static void handle_appendentries_response(const struct raft_peer* peer, 
                                          char* ptr) {
    logmsg(LOG_DEBUG, "raft: received appendentries response from peerid=%d", peer->id);

    msg_appendentries_response_t r;
    deserialize_appendentries_response(ptr, &r);

    logmsg(LOG_DEBUG, "raft: appendentries response: term %ld, success %d, current_idx %ld, first_idx %ld", r.term, r.success, r.current_idx, r.first_idx);

    raft_recv_appendentries_response(raft_srv, peer->node, &r);
}

////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////



/* ----------------------------------------------------------------------
 * Handle translation helpers
 * ---------------------------------------------------------------------- */

static nfs_fh3 local_root_fh;
static uint32_t local_fsid;
static int local_fh_ready = 0;

/* Obtain the local root file handle and fsid from the exports list.  */
static void init_local_handle(void)
{
    if (local_fh_ready)
        return;

    if (!exports_nfslist) {
        logmsg(LOG_ERR, "init_local_handle: no exports available");
        return;
    }

    const char *path = exports_nfslist->ex_dir;
    if (!path) {
        logmsg(LOG_ERR, "init_local_handle: export path missing");
        return;
    }

    unfs3_fh_t fh = fh_comp_raw(path, NULL, FH_DIR);
    if (!fh_valid(fh)) {
        logmsg(LOG_ERR, "init_local_handle: failed to compose fh for %s", path);
        return;
    }

    static char buf[FH_MAXBUF];
    local_root_fh = fh_encode(&fh, buf);
    local_fsid = fh.dev;
    local_fh_ready = 1;
}

/* Replace the device (fsid) in a handle with the local fsid.  If the handle
 * refers to the export root itself, replace it entirely with the local root
 * handle. */
static void adjust_handle(nfs_fh3 *fh)
{
    init_local_handle();
    if (!local_fh_ready)
        return;

    unfs3_fh_t obj = fh_decode(fh);

    if (obj.len == 0) {
        /* Root handle */
        free(fh->data.data_val);
        fh->data.data_val = malloc(local_root_fh.data.data_len);
        memcpy(fh->data.data_val, local_root_fh.data.data_val,
               local_root_fh.data.data_len);
        fh->data.data_len = local_root_fh.data.data_len;
        return;
    }

    if (obj.dev != local_fsid) {
        obj.dev = local_fsid;
        char *buf = malloc(FH_MAXBUF);
        nfs_fh3 newfh = fh_encode(&obj, buf);
        free(fh->data.data_val);
        fh->data = newfh.data;
    }
}

static void fh_to_hex2(const nfs_fh3 *fh, char *out)
{
    size_t len = fh->data.data_len;
    const unsigned char *d = (const unsigned char *)fh->data.data_val;
    size_t i;
    for (i = 0; i < len && i < FH_MAXBUF2; i++)
    {
        // logmsg(LOG_DEBUG, "fh_to_hex: byte %zu = %02x", i, d[i]);
        sprintf(out + i * 2, "%02x", d[i]);
    }
    // logmsg(LOG_DEBUG, "fh_to_hex: total length = %zu", len);
    out[i * 2] = '\0';
    // logmsg(LOG_DEBUG, "fh_to_hex: hex string = %s", out);   
}

const char *fh_to_hexstr2(const nfs_fh3 *fh)
{
    // Each byte = 2 hex digits, plus null terminator
    // char hexbuf[FH_MAXBUF * 2 + 1];
    char *hexbuf = malloc(FH_MAXBUF2 * 2 + 1);
    fh_to_hex2(fh, hexbuf);
    // logmsg(LOG_DEBUG, "fh_to_hexstr: hex string = %s", hexbuf);    
    return hexbuf;
}

/* Adjust all file handles contained in the RPC arguments so that they use
 * the local device id and root handle. */
void adjust_handles_for_proc(u_long proc, void *argp)
{
    switch (proc) {
        case NFSPROC3_GETATTR:
            adjust_handle(&((GETATTR3args *)argp)->object);
            break;
        case NFSPROC3_SETATTR:
            adjust_handle(&((SETATTR3args *)argp)->object);
            break;
        case NFSPROC3_LOOKUP:
            adjust_handle(&((LOOKUP3args *)argp)->what.dir);
            break;
        case NFSPROC3_ACCESS:
            adjust_handle(&((ACCESS3args *)argp)->object);
            break;
        case NFSPROC3_READLINK:
            adjust_handle(&((READLINK3args *)argp)->symlink);
            break;
        case NFSPROC3_READ:
            adjust_handle(&((READ3args *)argp)->file);
            break;
        case NFSPROC3_WRITE:
            adjust_handle(&((WRITE3args *)argp)->file);
            break;
        case NFSPROC3_CREATE:
            adjust_handle(&((CREATE3args *)argp)->where.dir);
            break;
        case NFSPROC3_MKDIR:
            adjust_handle(&((MKDIR3args *)argp)->where.dir);
            break;
        case NFSPROC3_SYMLINK:
            adjust_handle(&((SYMLINK3args *)argp)->where.dir);
            break;
        case NFSPROC3_MKNOD:
            adjust_handle(&((MKNOD3args *)argp)->where.dir);
            break;
        case NFSPROC3_REMOVE:
            adjust_handle(&((REMOVE3args *)argp)->object.dir);
            break;
        case NFSPROC3_RMDIR:
            adjust_handle(&((RMDIR3args *)argp)->object.dir);
            break;
        case NFSPROC3_RENAME:
            adjust_handle(&((RENAME3args *)argp)->from.dir);
            adjust_handle(&((RENAME3args *)argp)->to.dir);
            break;
        case NFSPROC3_LINK:
            adjust_handle(&((LINK3args *)argp)->file);
            adjust_handle(&((LINK3args *)argp)->link.dir);
            break;
        case NFSPROC3_READDIR:
            adjust_handle(&((READDIR3args *)argp)->dir);
            break;
        case NFSPROC3_READDIRPLUS:
            adjust_handle(&((READDIRPLUS3args *)argp)->dir);
            break;
        case NFSPROC3_FSSTAT:
            adjust_handle(&((FSSTAT3args *)argp)->fsroot);
            break;
        case NFSPROC3_FSINFO:
            adjust_handle(&((FSINFO3args *)argp)->fsroot);
            break;
        case NFSPROC3_PATHCONF:
            adjust_handle(&((PATHCONF3args *)argp)->object);
            break;
        case NFSPROC3_COMMIT:
            adjust_handle(&((COMMIT3args *)argp)->file);
            break;
        default:
            break;
    }
}

static void apply_nfs_operation(uint32_t proc, raft_client_info_t *info, char* buf, size_t len)
{
    union {
        GETATTR3args getattr;
        SETATTR3args setattr;
        LOOKUP3args lookup;
        ACCESS3args access;
        READLINK3args readlink;
        READ3args read;
        WRITE3args write;
        CREATE3args create;
        MKDIR3args mkdir;
        SYMLINK3args symlink;
        MKNOD3args mknod;
        REMOVE3args remove;
        RMDIR3args rmdir;
        RENAME3args rename;
        LINK3args link;
        READDIR3args readdir;
        READDIRPLUS3args readdirplus;
        FSSTAT3args fsstat;
        FSINFO3args fsinfo;
        PATHCONF3args pathconf;
        COMMIT3args commit;
    } argument;
    xdrproc_t xdr_argument;
    char *(*local)(char *, struct svc_req *);

    logmsg(LOG_DEBUG, "apply_nfs_operation: proc %u, buf %p, len %zu", proc, buf, len); 
    
    switch (proc) {
        case NFSPROC3_NULL:
            xdr_argument = (xdrproc_t) xdr_void;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_null_3_svc;
            break;
        case NFSPROC3_GETATTR:
            xdr_argument = (xdrproc_t) xdr_GETATTR3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_getattr_3_svc;
            break;
        case NFSPROC3_SETATTR:
            xdr_argument = (xdrproc_t) xdr_SETATTR3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_setattr_3_svc;
            break;
        case NFSPROC3_LOOKUP:
            xdr_argument = (xdrproc_t) xdr_LOOKUP3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_lookup_3_svc;
            break;
        case NFSPROC3_ACCESS:
            xdr_argument = (xdrproc_t) xdr_ACCESS3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_access_3_svc;
            break;
        case NFSPROC3_READLINK:
            xdr_argument = (xdrproc_t) xdr_READLINK3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_readlink_3_svc;
            break;
        case NFSPROC3_READ:
            xdr_argument = (xdrproc_t) xdr_READ3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_read_3_svc;
            break;
        case NFSPROC3_WRITE:
            xdr_argument = (xdrproc_t) xdr_WRITE3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_write_3_svc;
            break;
        case NFSPROC3_CREATE:
            xdr_argument = (xdrproc_t) xdr_CREATE3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_create_3_svc;
            break;
        case NFSPROC3_MKDIR:
            xdr_argument = (xdrproc_t) xdr_MKDIR3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_mkdir_3_svc;
            break;
        case NFSPROC3_SYMLINK:
            xdr_argument = (xdrproc_t) xdr_SYMLINK3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_symlink_3_svc;
            break;
        case NFSPROC3_MKNOD:
            xdr_argument = (xdrproc_t) xdr_MKNOD3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_mknod_3_svc;
            break;
        case NFSPROC3_REMOVE:
            xdr_argument = (xdrproc_t) xdr_REMOVE3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_remove_3_svc;
            break;
        case NFSPROC3_RMDIR:
            xdr_argument = (xdrproc_t) xdr_RMDIR3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_rmdir_3_svc;
            break;
        case NFSPROC3_RENAME:
            xdr_argument = (xdrproc_t) xdr_RENAME3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_rename_3_svc;
            break;
        case NFSPROC3_LINK:
            xdr_argument = (xdrproc_t) xdr_LINK3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_link_3_svc;
            break;
        case NFSPROC3_READDIR:
            xdr_argument = (xdrproc_t) xdr_READDIR3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_readdir_3_svc;
            break;
        case NFSPROC3_READDIRPLUS:
            xdr_argument = (xdrproc_t) xdr_READDIRPLUS3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_readdirplus_3_svc;
            break;
        case NFSPROC3_FSSTAT:
            xdr_argument = (xdrproc_t) xdr_FSSTAT3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_fsstat_3_svc;
            break;
        case NFSPROC3_FSINFO:
            xdr_argument = (xdrproc_t) xdr_FSINFO3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_fsinfo_3_svc;
            break;
        case NFSPROC3_PATHCONF:
            xdr_argument = (xdrproc_t) xdr_PATHCONF3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_pathconf_3_svc;
            break;
        case NFSPROC3_COMMIT:
            xdr_argument = (xdrproc_t) xdr_COMMIT3args;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_commit_3_svc;
            break;
        default:
            return;
    }

    print_buffer_hex(buf, len, "apply_nfs_operation: received buffer");

    memset(&argument, 0, sizeof(argument));
    XDR xdrs;
    xdrmem_create(&xdrs, buf, len, XDR_DECODE);
    if (!xdr_argument(&xdrs, (caddr_t)&argument)) {
        xdr_destroy(&xdrs);
        logmsg(LOG_ERR, "raft: failed to decode args for proc %u", proc);
        return;
    }
    xdr_destroy(&xdrs);

    /* Convert incoming file handles to local form */
    adjust_handles_for_proc(proc, &argument);

    struct authunix_parms cred = {0};
    gid_t gids[NGRPS] = {0};
    unsigned int i;

    cred.aup_machname = "raft";
    cred.aup_time = 0;
    cred.aup_uid = info->uid;
    cred.aup_gid = info->gid;
    cred.aup_len = info->gid_len;
    cred.aup_gids = gids;
    for (i = 0; i < cred.aup_len && i < NGRPS; i++)
        gids[i] = info->gids[i];

    logmsg(LOG_DEBUG, "apply_nfs_operation: uid=%u, gid=%u, gids_len=%u",
           cred.aup_uid, cred.aup_gid, cred.aup_len);

    SVCXPRT xprt;
    memset(&xprt, 0, sizeof(xprt));
    memcpy(&xprt.xp_rtaddr, &info->addr, sizeof(info->addr));
    xprt.xp_fd = -1;
    xprt.xp_port = 0;
    xprt.xp_addrlen = sizeof(info->addr);

    struct svc_req dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.rq_clntcred = &cred;
    dummy.rq_cred.oa_flavor = AUTH_UNIX;
    dummy.rq_xprt = &xprt;

    logmsg(LOG_DEBUG, "apply_nfs_operation: executing proc %s", nfs3_proc_name(proc));

    (void)local((char *)&argument, &dummy);
}


////////////////////////////////////////////////////////////////////////////////////////////////////
//// Raft callbacks
////////////////////////////////////////////////////////////////////////////////////////////////////


static int raft_send_requestvote_cb(raft_server_t* raft, void* udata, raft_node_t* node, msg_requestvote_t* msg) {
    if (raft_disabled)
        return -1;

    struct raft_peer *peer = raft_node_get_udata(node);
    if (!peer)
        return -1;

    char buf[RAFT_MAX_PKT];
    size_t len = 0;
    buf[len++] = 1;
    len += serialize_requestvote(msg, buf + len);

    ssize_t sent = sendto(raft_sock, buf, len, 0, (struct sockaddr*)&peer->addr, sizeof(peer->addr));

    if (sent < 0) {
        logmsg(LOG_ERR, "raft: sendto failed: %s", strerror(errno));
        return -1;
    } else if (sent < len) {
        logmsg(LOG_WARNING, "raft: sent only %zd bytes, expected %zu bytes", sent, len);
    } else {
        logmsg(LOG_DEBUG, "raft: send requestvote to %d, term %ld, candidate %d", peer->id, msg->term, msg->candidate_id);
    }

    return 0;
}

static int raft_send_appendentries_cb(raft_server_t* raft, 
                                      void* udata, 
                                      raft_node_t* node, 
                                      msg_appendentries_t* msg) {
    if (raft_disabled)
        return -1;

    struct raft_peer *peer = raft_node_get_udata(node);
    if (!peer)
        return -1;
    uint8_t buf[RAFT_MAX_PKT];
    size_t len = 0;

    buf[len++] = 3;
    len += serialize_appendentries(msg, buf + len);

    if (msg->n_entries > 0 && msg->entries) {
        len += serialize_msg_entry_array(msg->entries, msg->n_entries, buf + len);
    }

    // print_buffer_hex(buf, len, "raft: send appendentries");

    ssize_t sent = sendto(raft_sock, buf, len, 0, (struct sockaddr*)&peer->addr, sizeof(peer->addr));
    if (sent != len) {
        logmsg(LOG_WARNING, "raft: sendto incomplete: sent %zd, expected %zu", sent, len);
        return -1;
    }

    logmsg(LOG_DEBUG, "raft: send appendentries to %d, term %ld, prev_log_idx %ld, prev_log_term %ld, leader_commit %ld, n_entries %d", peer->id, msg->term, msg->prev_log_idx, msg->prev_log_term, msg->leader_commit, msg->n_entries);

    return 0;
}


static int raft_persist_term_cb(raft_server_t* raft, 
                                void* udata, 
                                raft_term_t term, 
                                raft_node_id_t vote) {

    (void)raft; (void)udata; (void)term; (void)vote;
    logmsg(LOG_DEBUG, "raft: persist term %llu, vote %d", (unsigned long long)term, vote);
    return 0;
}

static int raft_persist_vote_cb(raft_server_t* raft,
                                void* udata,
                                raft_node_id_t vote) {

    (void)raft; (void)udata; (void)vote;
    logmsg(LOG_DEBUG, "raft: persist vote %d", vote);
    return 0;
}

static int raft_log_offer_cb(raft_server_t* raft,
                             void* udata,
                             raft_entry_t* entry,
                             raft_index_t entry_idx) {
    (void)raft; (void)udata; (void)entry_idx;
    logmsg(LOG_DEBUG, "raft: log offer callback called, idx %lu, term %llu, id %llu, type %d",
           (unsigned long)entry_idx,
           (unsigned long long)entry->term,
           (unsigned long long)entry->id,
           entry->type);    

    if (entry->data.len >= sizeof(uint32_t)) {
        uint32_t proc;
        memcpy(&proc, entry->data.buf, sizeof(proc));
        proc = ntohl(proc);
        raft_log_entry(proc, (char*)entry->data.buf + sizeof(proc),
                       entry->data.len - sizeof(proc));
    }
    return 0;
}

static int raft_log_poll_cb(raft_server_t* raft,
                            void* udata,
                            raft_entry_t* entry,
                            raft_index_t entry_idx) {
    logmsg(LOG_DEBUG, "raft: log poll callback called, idx %lu, term %llu", 
              (unsigned long)entry_idx, (unsigned long long)entry->term);    
    return 0;
}

static int raft_log_pop_cb(raft_server_t* raft,
                           void* udata,
                           raft_entry_t* entry,
                           raft_index_t entry_idx) {
    logmsg(LOG_DEBUG, "raft: log pop callback called, idx %lu, term %llu",
           (unsigned long)entry_idx, (unsigned long long)entry->term);
    return 0;
}


static int raft_applylog_cb(raft_server_t* raft,
                            void* udata,
                            raft_entry_t* entry,
                            raft_index_t entry_idx) {
    logmsg(LOG_DEBUG, "raft: apply log callback called, idx %lu, term %llu",
           (unsigned long)entry_idx, (unsigned long long)entry->term);

    // if (raft_is_leader(raft_srv)){
        // logmsg(LOG_CRIT, "raft: skipping applylog on leader idx %lu", (unsigned long)entry_idx);
        // return 0;
    // }

    // Buffer must be large enough for proc and info header
    size_t proc_size = sizeof(uint32_t);
    size_t info_size = sizeof(raft_client_info_t);

    if (entry->type != RAFT_LOGTYPE_NORMAL || entry->data.len < proc_size + info_size)
        return 0;

    size_t offset = 0;

    print_buffer_hex(entry->data.buf, entry->data.len, "raft: apply log data");

    // Deserialize proc
    uint32_t proc;
    memcpy(&proc, entry->data.buf + offset, proc_size);
    proc = ntohl(proc);
    offset += proc_size;

    // Deserialize raft_client_info_t using your helper
    raft_client_info_t info;
    int info_bytes = raft_client_info_deserialize(&info,
                        (const uint8_t*)(entry->data.buf + offset), info_size);
    if (info_bytes != info_size) {
        logmsg(LOG_ERR, "raft_client_info_deserialize failed (%d/%zu)", info_bytes, info_size);
        return 0;
    }
    offset += info_size;

    logmsg(LOG_DEBUG, "raft: applying log idx %lu proc %s, uid=%u, gid=%u, gids_len=%u, data len %u",
           (unsigned long)entry_idx, nfs3_proc_name(proc),
           info.uid, info.gid, info.gid_len,
           entry->data.len - offset);

    logmsg(LOG_CRIT, "raft: applying log idx %lu proc %s",
           (unsigned long)entry_idx, nfs3_proc_name(proc));

    apply_nfs_operation(proc,
                        &info,
                        (char*)entry->data.buf + offset,
                        entry->data.len - offset);
    return 0;
}



void raft_init(void) {
    // initialize raft server
    raft_srv = raft_new();

    // set raft callbacks
    raft_cbs.send_requestvote = raft_send_requestvote_cb;
    raft_cbs.send_appendentries = raft_send_appendentries_cb;
    raft_cbs.persist_term = raft_persist_term_cb;
    raft_cbs.persist_vote = raft_persist_vote_cb;
    raft_cbs.log_offer = raft_log_offer_cb;
    raft_cbs.log_poll = raft_log_poll_cb;
    raft_cbs.log_pop = raft_log_pop_cb;
    raft_cbs.applylog = raft_applylog_cb;
    raft_set_callbacks(raft_srv, &raft_cbs, NULL);

    // create UDP socket for communication
    raft_sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in self = {0};
    self.sin_family = AF_INET;
    self.sin_port = htons(RAFT_PORT_BASE + opt_raft_id);
    self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(raft_sock, (struct sockaddr*)&self, sizeof(self));

    // add self as a raft node
    raft_add_node(raft_srv, NULL, opt_raft_id, 1);
    raft_peers[raft_peer_count].id = opt_raft_id;
    raft_peers[raft_peer_count].addr = self;
    raft_peers[raft_peer_count].node = raft_get_node(raft_srv, opt_raft_id);
    raft_node_set_udata(raft_peers[raft_peer_count].node, &raft_peers[raft_peer_count]);
    raft_peer_count++;

    // parse peers from command line
    if (opt_raft_peers) {
        char *tmp = strdup(opt_raft_peers);
        char *p = strtok(tmp, ",");
        while (p) {
            int id = atoi(p);
            if (id != opt_raft_id) {
                logmsg(LOG_INFO, "Adding raft peer %d", id);
                struct sockaddr_in addr = {0};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(RAFT_PORT_BASE + id);
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                raft_node_t* n = raft_add_node(raft_srv, NULL, id, 0);
                raft_peers[raft_peer_count].id = id;
                raft_peers[raft_peer_count].addr = addr;
                raft_peers[raft_peer_count].node = n;
                raft_node_set_udata(n, &raft_peers[raft_peer_count]);
                raft_peer_count++;
            }
            p = strtok(NULL, ",");
        }
        free(tmp);
    }
}

