
/*
 * UNFS3 server framework
 * Originally generated using rpcgen
 * Portions (C) 2004, Pascal Schmidt
 * see file LICENSE for license details
 */

#include "config.h"

#ifdef WIN32
#include <ws2tcpip.h>
#endif

#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <rpc/rpc.h>

#ifndef WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#endif				       /* WIN32 */

#include <fcntl.h>
#include <memory.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <unistd.h>
#ifdef HAVE_LIBPROC_H
#include <libproc.h>
#endif

#ifdef HAVE_RPC_SVC_SOC_H
# include <rpc/svc_soc.h>
#endif
#if defined(HAVE_SVC_GETREQ_POLL) && HAVE_DECL_SVC_POLLFD
# include <sys/poll.h>
#endif

#include "nfs.h"
#include "mount.h"
#include "xdr.h"
#include "fh.h"
#include "fh_cache.h"
#include "fd_cache.h"
#include "user.h"
#include "daemon.h"
#include "backend.h"
#include "Config/exports.h"
#include "raft_log.h"
#include "raft.h"
#include "daemon_raft.h"
#ifndef SIG_PF
#define SIG_PF void(*)(int)
#endif

#define UNFS_NAME "UNFS3 unfsd " PACKAGE_VERSION " (C) 2003-2025, various authors\n"

/* write verifier */
writeverf3 wverf;

/* readdir cookie */
cookie3 rcookie = 0;

/* options and default values */
int opt_detach = TRUE;
char *opt_exports = "/etc/exports";

int opt_cluster = FALSE;
char *opt_cluster_path = "/";
int opt_tcponly = FALSE;
unsigned int opt_nfs_port = NFS_PORT;	/* 0 means RPC_ANYSOCK */
unsigned int opt_mount_port = NFS_PORT;
int opt_singleuser = FALSE;
int opt_brute_force = FALSE;
int opt_testconfig = FALSE;
struct in6_addr opt_bind_addr;
int opt_readable_executables = FALSE;
char *opt_pid_file = NULL;
int opt_32_bit_truncate = FALSE;

char *my_mount_root = NULL; // Mount root for this instance
char *logical_mount_root = NULL; // Logical mount root for this instance

/* Global RPC transport handles so we can re-register services */
static SVCXPRT *nfs_udptransp = NULL;
static SVCXPRT *nfs_tcptransp = NULL;
static SVCXPRT *mount_udptransp = NULL;
static SVCXPRT *mount_tcptransp = NULL;

static int was_leader = 0;
static int is_leader_now = 0;
volatile sig_atomic_t raft_disabled = 0;

/* Register with portmapper? */
int opt_portmapper = TRUE;

/*
 * output message to syslog or stdout
 */
void logmsg(int prio, const char *fmt, ...)
{
    va_list ap;

#if HAVE_VSYSLOG == 0
    char mesg[1024];
#endif

    va_start(ap, fmt);
    if (opt_detach) {
#if HAVE_VSYSLOG == 1
        vsyslog(prio, fmt, ap);
#else
        vsnprintf(mesg, 1024, fmt, ap);
        syslog(prio, mesg, 1024);
#endif
    } else {
        if (prio <= LOG_CRIT) {
            fprintf(stderr, "[%s] ", time_str());
            vprintf(fmt, ap);
            putchar('\n');
        }
    }
    va_end(ap);
}

/*
 * return remote address from svc_req structure
 */
int get_remote(struct svc_req *rqstp, struct in6_addr *addr6)
{
    const struct sockaddr *saddr;

    memset(addr6, 0, sizeof(struct in6_addr));

    saddr = (const struct sockaddr *) svc_getrpccaller(rqstp->rq_xprt)->buf;

    if (saddr->sa_family == AF_INET6) {
        memcpy(addr6, &((const struct sockaddr_in6*)saddr)->sin6_addr, sizeof(struct in6_addr));
        return 0;
    }

    if (saddr->sa_family == AF_INET) {
        ((uint32_t*)addr6)[0] = 0;
        ((uint32_t*)addr6)[1] = 0;
        ((uint32_t*)addr6)[2] = htonl(0xffff);
        ((uint32_t*)addr6)[3] = ((const struct sockaddr_in*)saddr)->sin_addr.s_addr;
        return 0;
    }

    errno = EAFNOSUPPORT;
    return -1;
}

/*
 * return remote port from svc_req structure
 */
short get_port(struct svc_req *rqstp)
{
    const struct sockaddr *saddr;

    saddr = (const struct sockaddr *) svc_getrpccaller(rqstp->rq_xprt)->buf;

    if (saddr->sa_family == AF_INET6)
        return ((const struct sockaddr_in6*)saddr)->sin6_port;

    if (saddr->sa_family == AF_INET)
        return ((const struct sockaddr_in*)saddr)->sin_port;

    errno = EAFNOSUPPORT;
    return -1;
}

/*
 * return the socket type of the request (SOCK_STREAM or SOCK_DGRAM)
 */
int get_socket_type(struct svc_req *rqstp)
{
    int v, res;
    socklen_t l;

    l = sizeof(v);

    res = getsockopt(rqstp->rq_xprt->xp_fd, SOL_SOCKET, SO_TYPE, (void*)&v, &l);

    if (res < 0) {
        logmsg(LOG_CRIT, "Unable to determine socket type");
        return -1;
    }

    return v;
}

/*
 * write current pid to a file
 */
static void create_pid_file(void)
{
    char buf[16];
    int fd, res, len;

    if (!opt_pid_file)
        return;

    fd = backend_open_create(opt_pid_file, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        logmsg(LOG_WARNING, "Failed to create pid file `%s'", opt_pid_file);
        return;
    }
#if defined(LOCK_EX) && defined(LOCK_NB)
    res = backend_flock(fd, LOCK_EX | LOCK_NB);
    if (res == -1) {
        logmsg(LOG_WARNING, "Failed to lock pid file `%s'", opt_pid_file);
        backend_close(fd);
        return;
    }
#endif

    sprintf(buf, "%i\n", backend_getpid());
    len = strlen(buf);

    res = backend_pwrite(fd, buf, len, 0);
    backend_close(fd);
    if (res != len) {
        logmsg(LOG_WARNING, "Failed to write pid file `%s'", opt_pid_file);
    }
}

/*
 * remove pid file
 */
static void remove_pid_file(void)
{
    int res;

    if (!opt_pid_file)
        return;

    res = backend_remove(opt_pid_file);
    if (res == -1 && errno != ENOENT) {
        logmsg(LOG_WARNING, "Failed to remove pid file `%s'", opt_pid_file);
    }
}

/*
 * parse command line options
 */
static void parse_options(int argc, char **argv)
{
    int opt = 0;
    char *optstring = "3bcC:de:hl:m:n:prstTuwi:R:I:P:g:G:";

#if defined(WIN32) || defined(AFS_SUPPORT)
    /* Allways truncate to 32 bits in these cases */
    opt_32_bit_truncate = TRUE;
#endif

    while (opt != -1) {
        opt = getopt(argc, argv, optstring);
        switch (opt) {
            case '3':
                opt_32_bit_truncate = TRUE;
                break;
            case 'b':
                opt_brute_force = TRUE;
                break;
#ifdef WANT_CLUSTER
            case 'c':
                opt_cluster = TRUE;
                break;
            case 'C':
                opt_cluster_path = optarg;
                break;
#endif
            case 'd':
                printf(UNFS_NAME);
                opt_detach = FALSE;
                break;
            case 'e':
#ifndef WIN32
                if (optarg[0] != '/') {
                    /* A relative path won't work for re-reading the exports
                       file on SIGHUP, since we are changing directory */
                    fprintf(stderr, "Error: relative path to exports file\n");
                    exit(1);
                }
#endif
                opt_exports = optarg;
                break;
            case 'h':
                printf(UNFS_NAME);
                printf("Usage: %s [options]\n", argv[0]);
                printf("\t-h          display this short option summary\n");
                printf("\t-u          use unprivileged port for services\n");
                printf("\t-d          do not detach from terminal\n");
                printf("\t-e <file>   file to use instead of /etc/exports\n");
                printf("\t-i <file>   write daemon pid to given file\n");
#ifdef WANT_CLUSTER
                printf("\t-c          enable cluster extensions\n");
                printf("\t-C <path>   set path for cluster extensions\n");
#endif
                printf("\t-n <port>   port to use for NFS service\n");
                printf("\t-m <port>   port to use for MOUNT service\n");
                printf
                ("\t-t          TCP only, do not listen on UDP ports\n");
                printf("\t-p          do not register with portmap/rpcbind\n");
                printf("\t-s          single user mode\n");
                printf("\t-b          enable brute force file searching\n");
                printf
                ("\t-l <addr>   bind to interface with specified address\n");
                printf
                ("\t-r          report unreadable executables as readable\n");
                printf
                ("\t-3          truncate fileid and cookie to 32 bits\n");
                printf("\t-T          test exports file and exit\n");
                printf("\t-R <file>   path to raft log\n");
                printf("\t-I <id>     raft node id\n");
                printf("\t-P <ids>    comma separated raft peer ids\n");
                printf("\t-g <path>   this replica mount root path \n");
                printf("\t-G <path>   logical mount root path \n");

                exit(0);
                break;
            case 'l':
                if (inet_pton(AF_INET6, optarg, &opt_bind_addr) != 1) {
                    struct in_addr in4;

                    if (inet_pton(AF_INET, optarg, &in4) != 1) {
                        fprintf(stderr, "Invalid bind address\n");
                        exit(1);
                    }

                    ((uint32_t*)&opt_bind_addr)[0] = 0;
                    ((uint32_t*)&opt_bind_addr)[1] = 0;
                    ((uint32_t*)&opt_bind_addr)[2] = htonl(0xffff);
                    ((uint32_t*)&opt_bind_addr)[3] = in4.s_addr;
                }
                break;
            case 'm':
                opt_mount_port = strtol(optarg, NULL, 10);
                if (opt_mount_port == 0) {
                    fprintf(stderr, "Invalid port\n");
                    exit(1);
                }
                break;
            case 'n':
                opt_nfs_port = strtol(optarg, NULL, 10);
                if (opt_nfs_port == 0) {
                    fprintf(stderr, "Invalid port\n");
                    exit(1);
                }
                break;
            case 'p':
                opt_portmapper = FALSE;
                break;
            case 'r':
                opt_readable_executables = TRUE;
                break;
            case 's':
                opt_singleuser = TRUE;
#ifndef WIN32
                if (backend_getuid() == 0) {
                    logmsg(LOG_WARNING,
                           "Warning: running as root with -s is dangerous");
                    logmsg(LOG_WARNING,
                           "All clients will have root access to all exported files!");
                }
#endif
                break;
            case 't':
                opt_tcponly = TRUE;
                break;
            case 'T':
                opt_testconfig = TRUE;
                break;
            case 'u':
                opt_nfs_port = 0;
                opt_mount_port = 0;
                break;
            case 'i':
                opt_pid_file = optarg;
                break;
            case 'R':
                opt_raft_log = optarg;
                break;
            case 'I':
                opt_raft_id = atoi(optarg);
                break;
            case 'P':
                opt_raft_peers = optarg;
                break;
            case 'g':
                my_mount_root = optarg;
                break;  
            case 'G':
                logical_mount_root = optarg; 
                break;
            case '?':
                exit(1);
                break;
        }
    }
}

/*
 * signal handler and error exit function
 */
void daemon_exit(int error)
{
#ifndef WIN32
    if (error == SIGHUP) {
        get_squash_ids();
        exports_parse();
        return;
    }

    if (error == SIGUSR1) {
        if (fh_cache_use > 0)
            logmsg(LOG_INFO, "fh entries %i access %i hit %i miss %i",
                   fh_cache_max, fh_cache_use, fh_cache_hit,
                   fh_cache_use - fh_cache_hit);
        else
            logmsg(LOG_INFO, "fh cache unused");
        logmsg(LOG_INFO, "Open file descriptors: read %i, write %i",
               fd_cache_readers, fd_cache_writers);
        return;
    }
#endif				       /* WIN32 */

    if (opt_portmapper) {
        svc_unreg(MOUNTPROG, MOUNTVERS1);
        svc_unreg(MOUNTPROG, MOUNTVERS3);
    }

    if (opt_portmapper) {
        svc_unreg(NFS3_PROGRAM, NFS_V3);
    }

    if (error == SIGSEGV)
        logmsg(LOG_EMERG, "Segmentation fault");

    fd_cache_purge();

    if (opt_detach)
        closelog();

    remove_pid_file();
    backend_shutdown();

    raft_shutdown();
    
    exit(1);
}

/*
 * NFS service dispatch function
 * generated by rpcgen
 */
static void nfs3_program_3(struct svc_req *rqstp, register SVCXPRT * transp)
{
    union {
        GETATTR3args nfsproc3_getattr_3_arg;
        SETATTR3args nfsproc3_setattr_3_arg;
        LOOKUP3args nfsproc3_lookup_3_arg;
        ACCESS3args nfsproc3_access_3_arg;
        READLINK3args nfsproc3_readlink_3_arg;
        READ3args nfsproc3_read_3_arg;
        WRITE3args nfsproc3_write_3_arg;
        CREATE3args nfsproc3_create_3_arg;
        MKDIR3args nfsproc3_mkdir_3_arg;
        SYMLINK3args nfsproc3_symlink_3_arg;
        MKNOD3args nfsproc3_mknod_3_arg;
        REMOVE3args nfsproc3_remove_3_arg;
        RMDIR3args nfsproc3_rmdir_3_arg;
        RENAME3args nfsproc3_rename_3_arg;
        LINK3args nfsproc3_link_3_arg;
        READDIR3args nfsproc3_readdir_3_arg;
        READDIRPLUS3args nfsproc3_readdirplus_3_arg;
        FSSTAT3args nfsproc3_fsstat_3_arg;
        FSINFO3args nfsproc3_fsinfo_3_arg;
        PATHCONF3args nfsproc3_pathconf_3_arg;
        COMMIT3args nfsproc3_commit_3_arg;
    } argument;
    
    char *result;
    xdrproc_t _xdr_argument, _xdr_result;
    char *(*local) (char *, struct svc_req *);
    
    struct in6_addr remote_addr;
    char remote_host[INET6_ADDRSTRLEN];
    get_remote(rqstp, &remote_addr);
    inet_ntop(AF_INET6, &remote_addr, remote_host, sizeof(remote_host));

    switch (rqstp->rq_proc) {
        case NFSPROC3_NULL:
            _xdr_argument = (xdrproc_t) xdr_void;
            _xdr_result = (xdrproc_t) xdr_void;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_null_3_svc;
            break;

        case NFSPROC3_GETATTR:
            _xdr_argument = (xdrproc_t) xdr_GETATTR3args;
            _xdr_result = (xdrproc_t) xdr_GETATTR3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_getattr_3_svc;
            break;

        case NFSPROC3_SETATTR:
            _xdr_argument = (xdrproc_t) xdr_SETATTR3args;
            _xdr_result = (xdrproc_t) xdr_SETATTR3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_setattr_3_svc;
            break;

        case NFSPROC3_LOOKUP:
            _xdr_argument = (xdrproc_t) xdr_LOOKUP3args;
            _xdr_result = (xdrproc_t) xdr_LOOKUP3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_lookup_3_svc;
            break;

        case NFSPROC3_ACCESS:
            _xdr_argument = (xdrproc_t) xdr_ACCESS3args;
            _xdr_result = (xdrproc_t) xdr_ACCESS3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_access_3_svc;
            break;

        case NFSPROC3_READLINK:
            _xdr_argument = (xdrproc_t) xdr_READLINK3args;
            _xdr_result = (xdrproc_t) xdr_READLINK3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_readlink_3_svc;
            break;

        case NFSPROC3_READ:
            _xdr_argument = (xdrproc_t) xdr_READ3args;
            _xdr_result = (xdrproc_t) xdr_READ3res;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_read_3_svc;
            break;

        case NFSPROC3_WRITE:
            _xdr_argument = (xdrproc_t) xdr_WRITE3args;
            _xdr_result = (xdrproc_t) xdr_WRITE3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_write_3_svc;
            break;

        case NFSPROC3_CREATE:
            _xdr_argument = (xdrproc_t) xdr_CREATE3args;
            _xdr_result = (xdrproc_t) xdr_CREATE3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_create_3_svc;
            break;

        case NFSPROC3_MKDIR:
            _xdr_argument = (xdrproc_t) xdr_MKDIR3args;
            _xdr_result = (xdrproc_t) xdr_MKDIR3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_mkdir_3_svc;
            break;

        case NFSPROC3_SYMLINK:
            _xdr_argument = (xdrproc_t) xdr_SYMLINK3args;
            _xdr_result = (xdrproc_t) xdr_SYMLINK3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_symlink_3_svc;
            break;

        case NFSPROC3_MKNOD:
            _xdr_argument = (xdrproc_t) xdr_MKNOD3args;
            _xdr_result = (xdrproc_t) xdr_MKNOD3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_mknod_3_svc;
            break;

        case NFSPROC3_REMOVE:
            _xdr_argument = (xdrproc_t) xdr_REMOVE3args;
            _xdr_result = (xdrproc_t) xdr_REMOVE3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_remove_3_svc;
            break;

        case NFSPROC3_RMDIR:
            _xdr_argument = (xdrproc_t) xdr_RMDIR3args;
            _xdr_result = (xdrproc_t) xdr_RMDIR3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_rmdir_3_svc;
            break;

        case NFSPROC3_RENAME:
            _xdr_argument = (xdrproc_t) xdr_RENAME3args;
            _xdr_result = (xdrproc_t) xdr_RENAME3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_rename_3_svc;
            break;

        case NFSPROC3_LINK:
            _xdr_argument = (xdrproc_t) xdr_LINK3args;
            _xdr_result = (xdrproc_t) xdr_LINK3res;
            local = (char *(*)(char *, struct svc_req *)) nfsproc3_link_3_svc;
            break;

        case NFSPROC3_READDIR:
            _xdr_argument = (xdrproc_t) xdr_READDIR3args;
            _xdr_result = (xdrproc_t) xdr_READDIR3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_readdir_3_svc;
            break;

        case NFSPROC3_READDIRPLUS:
            _xdr_argument = (xdrproc_t) xdr_READDIRPLUS3args;
            _xdr_result = (xdrproc_t) xdr_READDIRPLUS3res;
            local = (char *(*)(char *, struct svc_req *))
                    nfsproc3_readdirplus_3_svc;
            break;

        case NFSPROC3_FSSTAT:
            _xdr_argument = (xdrproc_t) xdr_FSSTAT3args;
            _xdr_result = (xdrproc_t) xdr_FSSTAT3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_fsstat_3_svc;
            break;

        case NFSPROC3_FSINFO:
            _xdr_argument = (xdrproc_t) xdr_FSINFO3args;
            _xdr_result = (xdrproc_t) xdr_FSINFO3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_fsinfo_3_svc;
            break;

        case NFSPROC3_PATHCONF:
            _xdr_argument = (xdrproc_t) xdr_PATHCONF3args;
            _xdr_result = (xdrproc_t) xdr_PATHCONF3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_pathconf_3_svc;
            break;

        case NFSPROC3_COMMIT:
            _xdr_argument = (xdrproc_t) xdr_COMMIT3args;
            _xdr_result = (xdrproc_t) xdr_COMMIT3res;
            local =
                (char *(*)(char *, struct svc_req *)) nfsproc3_commit_3_svc;
            break;

        default:
            svcerr_noproc(transp);
            return;
    }

    memset((char *) &argument, 0, sizeof(argument));
    if (!svc_getargs(transp, (xdrproc_t) _xdr_argument, (caddr_t) & argument)) {
        svcerr_decode(transp);
        return;
    }

    logmsg(LOG_CRIT, "received operation %s", nfs3_proc_name(rqstp->rq_proc));

    switch (rqstp->rq_proc) {
        // case NFSPROC3_LOOKUP:
        case NFSPROC3_READLINK:
        case NFSPROC3_READDIR:
        case NFSPROC3_READDIRPLUS:
        case NFSPROC3_READ:
        case NFSPROC3_GETATTR:
        case NFSPROC3_FSSTAT:
        case NFSPROC3_FSINFO:
        case NFSPROC3_PATHCONF:
        case NFSPROC3_ACCESS:
            // These operations do not modify the filesystem, we don't need to replicate them

            // change the handles to that of this replica. 
            // useful for scenarios where the handle was generated by a different leader 
            // at a different time, and and now the client is asking to do operations based 
            // on that handle on this replica.
            adjust_handles_for_proc(rqstp->rq_proc, &argument);

            logmsg(LOG_CRIT, "Executing operation %s", nfs3_proc_name(rqstp->rq_proc));
            result = (*local)((char *)&argument, rqstp);
            break;

        default:
            // For all other operations, serialize and replicate. 
            logmsg(LOG_CRIT, "Replicating operation %s", nfs3_proc_name(rqstp->rq_proc));

            result = raft_serialize_and_replicate_nfs_op(rqstp, remote_addr, 
                                                        _xdr_argument, &argument);
            
            if (result == NULL) {
                logmsg(LOG_CRIT, "Failed to replicate operation %s",
                          nfs3_proc_name(rqstp->rq_proc));
            }
            break;
    }

    if (result != NULL &&
        !svc_sendreply(transp, (xdrproc_t) _xdr_result, result)) {
        svcerr_systemerr(transp);
        logmsg(LOG_CRIT, "Unable to send RPC reply");
    }
    if (!svc_freeargs
        (transp, (xdrproc_t) _xdr_argument, (caddr_t) & argument)) {
        logmsg(LOG_CRIT, "Unable to free XDR arguments");
    }
    return;
}

/*
 * mount protocol dispatcher
 * generated by rpcgen
 */
static void mountprog_3(struct svc_req *rqstp, register SVCXPRT * transp)
{
    union {
        dirpath mountproc_mnt_3_arg;
        dirpath mountproc_umnt_3_arg;
    } argument;
    char *result;
    xdrproc_t _xdr_argument, _xdr_result;
    char *(*local) (char *, struct svc_req *);

    struct in6_addr remote_addr;
    char remote_host[INET6_ADDRSTRLEN];
    get_remote(rqstp, &remote_addr);
    inet_ntop(AF_INET6, &remote_addr, remote_host, sizeof(remote_host));

    switch (rqstp->rq_proc) {
        case MOUNTPROC_NULL:
            _xdr_argument = (xdrproc_t) xdr_void;
            _xdr_result = (xdrproc_t) xdr_void;
            local =
                (char *(*)(char *, struct svc_req *)) mountproc_null_3_svc;
            break;

        case MOUNTPROC_MNT:
            _xdr_argument = (xdrproc_t) xdr_dirpath;
            _xdr_result = (xdrproc_t) xdr_mountres3;
            local = (char *(*)(char *, struct svc_req *)) mountproc_mnt_3_svc;
            break;

        case MOUNTPROC_DUMP:
            _xdr_argument = (xdrproc_t) xdr_void;
            _xdr_result = (xdrproc_t) xdr_mountlist;
            local =
                (char *(*)(char *, struct svc_req *)) mountproc_dump_3_svc;
            break;

        case MOUNTPROC_UMNT:
            _xdr_argument = (xdrproc_t) xdr_dirpath;
            _xdr_result = (xdrproc_t) xdr_void;
            local =
                (char *(*)(char *, struct svc_req *)) mountproc_umnt_3_svc;
            break;

        case MOUNTPROC_UMNTALL:
            _xdr_argument = (xdrproc_t) xdr_void;
            _xdr_result = (xdrproc_t) xdr_void;
            local =
                (char *(*)(char *, struct svc_req *)) mountproc_umntall_3_svc;
            break;

        case MOUNTPROC_EXPORT:
            _xdr_argument = (xdrproc_t) xdr_void;
            _xdr_result = (xdrproc_t) xdr_exports;
            local =
                (char *(*)(char *, struct svc_req *)) mountproc_export_3_svc;
            break;

        default:
            svcerr_noproc(transp);
            return;
    }
    memset((char *) &argument, 0, sizeof(argument));
    if (!svc_getargs(transp, (xdrproc_t) _xdr_argument, (caddr_t) & argument)) {
        svcerr_decode(transp);
        return;
    }
    result = (*local) ((char *) &argument, rqstp);
    if (result != NULL &&
        !svc_sendreply(transp, (xdrproc_t) _xdr_result, result)) {
        svcerr_systemerr(transp);
        logmsg(LOG_CRIT, "Unable to send RPC reply");
    }
    if (!svc_freeargs
        (transp, (xdrproc_t) _xdr_argument, (caddr_t) & argument)) {
        logmsg(LOG_CRIT, "Unable to free XDR arguments");
    }
    return;
}

static int
_socket_getdomain(int socket)
{
#ifdef SO_DOMAIN
    int ret, domain;
    socklen_t len;
    len = sizeof(domain);
    ret = getsockopt(socket, SOL_SOCKET, SO_DOMAIN, (void*)&domain, &len);
    if (ret == -1)
        return -1;
    return domain;
#elif defined(PROC_PIDFDSOCKETINFO)
    struct socket_fdinfo info;
    int ret;
    ret = proc_pidfdinfo(getpid(), socket, PROC_PIDFDSOCKETINFO, &info,
                         sizeof info);
    /* returns bytes written; if we didn't write info, it's a failure */
    if (ret <= 0)
        return -1;
    return info.psi.soi_family;
#elif defined(SO_PROTOCOL_INFO)
    WSAPROTOCOL_INFO info;
    socklen_t len;

    len = sizeof(info);
    if (getsockopt(socket, SOL_SOCKET, SO_PROTOCOL_INFO, (void*)&info, &len))
        return -1;

    return info.iAddressFamily;
#else
#error no way to get socket domain
#endif
}


static void _register_service(SVCXPRT *transp,
                              const rpcprog_t prognum,
                              const char *progname,
                              const rpcvers_t versnum,
                              const char *versname,
                              void (*dispatch)(struct svc_req *, SVCXPRT *))
{
    int type, domain;
    socklen_t len;
    const char *netid;
    struct netconfig *nconf = NULL;

    len = sizeof(type);
    if (getsockopt(transp->xp_fd, SOL_SOCKET, SO_TYPE, (void*)&type, &len)) {
        perror("getsockopt");
        fprintf(stderr, "Unable to register (%s, %s).\n",
                progname, versname);
        daemon_exit(0);
    }

    if (type == SOCK_STREAM)
        netid = "tcp";
    else
        netid = "udp";

    if (opt_portmapper) {
        nconf = getnetconfigent(netid);
        if (nconf == NULL) {
            fprintf(stderr, "Unable to get netconfig entry \"%s\"\n", netid);
            daemon_exit(0);
        }
    }

    if (!svc_reg(transp, prognum, versnum, dispatch, nconf)) {
        fprintf(stderr, "Unable to register (%s, %s, %s).\n",
                progname, versname, netid);
        daemon_exit(0);
    }

    if (nconf != NULL)
        freenetconfigent(nconf);

    if (nconf == NULL)
        return;

    domain = _socket_getdomain(transp->xp_fd);
    if (domain == -1) {
        perror("_socket_getdomain");
        fprintf(stderr, "Unable to register (%s, %s).\n",
                progname, versname);
        daemon_exit(0);
    }

    if (domain != PF_INET6)
        return;

    if (type == SOCK_STREAM)
        netid = "tcp6";
    else
        netid = "udp6";

    nconf = getnetconfigent(netid);
    if (nconf == NULL) {
        fprintf(stderr, "Unable to get netconfig entry \"%s\"\n", netid);
        daemon_exit(0);
    }

    if (!svc_reg(transp, prognum, versnum, dispatch, nconf)) {
        fprintf(stderr, "Unable to register (%s, %s, %s).\n",
                progname, versname, netid);
        daemon_exit(0);
    }

    if (nconf != NULL)
        freenetconfigent(nconf);
}

#define register_service(t, p, v, d) \
    _register_service(t, p, #p, v, #v, d)

static void register_nfs_service(SVCXPRT * udptransp, SVCXPRT * tcptransp)
{
    if (opt_portmapper) {
        svc_unreg(NFS3_PROGRAM, NFS_V3);
    }

    if (udptransp != NULL) {
        /* Register NFS service for UDP */
        register_service(udptransp, NFS3_PROGRAM, NFS_V3, nfs3_program_3);
    }

    if (tcptransp != NULL) {
        /* Register NFS service for TCP */
        register_service(tcptransp, NFS3_PROGRAM, NFS_V3, nfs3_program_3);
    }
}

static void register_mount_service(SVCXPRT * udptransp, SVCXPRT * tcptransp)
{
    if (opt_portmapper) {
        svc_unreg(MOUNTPROG, MOUNTVERS1);
        svc_unreg(MOUNTPROG, MOUNTVERS3);
    }

    if (udptransp != NULL) {
        /* Register MOUNT service (v1) for UDP */
        register_service(udptransp, MOUNTPROG, MOUNTVERS1, mountprog_3);

        /* Register MOUNT service (v3) for UDP */
        register_service(udptransp, MOUNTPROG, MOUNTVERS3, mountprog_3);
    }

    if (tcptransp != NULL) {
        /* Register MOUNT service (v1) for TCP */
        register_service(tcptransp, MOUNTPROG, MOUNTVERS1, mountprog_3);

        /* Register MOUNT service (v3) for TCP */
        register_service(tcptransp, MOUNTPROG, MOUNTVERS3, mountprog_3);
    }
}

static SVCXPRT *create_udp_transport(unsigned int port)
{
    SVCXPRT *transp = NULL;
    int sock;

    sock = socket(PF_INET6, SOCK_DGRAM, 0);

    if ((sock == -1) && (errno == EAFNOSUPPORT))
        sock = socket(PF_INET, SOCK_DGRAM, 0);

    if (sock == -1) {
        perror("socket");
        fprintf(stderr, "Couldn't create a listening udp socket\n");
        exit(1);
    }

    int domain;
    const int on = 1;
    const int off = 0;

    const struct sockaddr *sin;
    size_t sin_len;
    struct sockaddr_in sin4;
    struct sockaddr_in6 sin6;

    domain = _socket_getdomain(sock);
    if (domain == -1) {
        perror("_socket_getdomain");
        fprintf(stderr, "Couldn't create a listening udp socket\n");
        exit(1);
    }

    if (domain == PF_INET6) {
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&off, sizeof(off));

        /* Make sure we null the entire sockaddr_in6 structure */
        memset(&sin6, 0, sizeof(struct sockaddr_in6));

        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons(port);
        sin6.sin6_addr = opt_bind_addr;

        sin = (const struct sockaddr*)&sin6;
        sin_len = sizeof(sin6);
    } else {
        /* Make sure we null the entire sockaddr_in structure */
        memset(&sin4, 0, sizeof(struct sockaddr_in));

        sin4.sin_family = AF_INET;
        sin4.sin_port = htons(port);

        if (IN6_IS_ADDR_UNSPECIFIED(&opt_bind_addr)) {
            sin4.sin_addr.s_addr = INADDR_ANY;
        } else if (IN6_IS_ADDR_V4MAPPED(&opt_bind_addr) ||
                   IN6_IS_ADDR_V4COMPAT(&opt_bind_addr)) {
            sin4.sin_addr.s_addr = ((uint32_t*)&opt_bind_addr)[3];
        } else {
            fprintf(stderr, "Invalid bind address specified\n");
            exit(1);
        }

        sin = (const struct sockaddr*)&sin4;
        sin_len = sizeof(sin4);
    }

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*)&on, sizeof(on));

    int bind_attempts = 0;

    /////////////////////////////////////////////////
    while (bind(sock, sin, sin_len)) {
        if (++bind_attempts >= 100) {
            perror("bind");
            logmsg(LOG_CRIT, "Couldn't bind to udp port %d after 100 attempts", port);  
            exit(1);
        }
        logmsg(LOG_WARNING, "Bind to udp port %d failed, retrying (%d/100)", port, bind_attempts);
        usleep(timeout_ms * 1000); // 0.1 seconds
    }
    /////////////////////////////////////////////////


    transp = svc_dg_create(sock, 0, 0);

    if (transp == NULL) {
        fprintf(stderr, "Cannot create udp service.\n");
        daemon_exit(0);
    }

    return transp;
}

static SVCXPRT *create_tcp_transport(unsigned int port)
{
    SVCXPRT *transp = NULL;
    int sock;

    sock = socket(PF_INET6, SOCK_STREAM, 0);

    if ((sock == -1) && (errno == EAFNOSUPPORT))
        sock = socket(PF_INET, SOCK_STREAM, 0);

    if (sock == -1) {
        perror("socket");
        fprintf(stderr, "Couldn't create a listening tcp socket\n");
        exit(1);
    }

    int domain;
    const int on = 1;
    const int off = 0;

    const struct sockaddr *sin;
    size_t sin_len;
    struct sockaddr_in sin4;
    struct sockaddr_in6 sin6;

    domain = _socket_getdomain(sock);
    if (domain == -1) {
        perror("_socket_getdomain");
        fprintf(stderr, "Couldn't create a listening tcp socket\n");
        exit(1);
    }

    if (domain == PF_INET6) {
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&off, sizeof(off));

        /* Make sure we null the entire sockaddr_in6 structure */
        memset(&sin6, 0, sizeof(struct sockaddr_in6));

        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons(port);
        sin6.sin6_addr = opt_bind_addr;

        sin = (const struct sockaddr*)&sin6;
        sin_len = sizeof(sin6);
    } else {
        /* Make sure we null the entire sockaddr_in structure */
        memset(&sin4, 0, sizeof(struct sockaddr_in));

        sin4.sin_family = AF_INET;
        sin4.sin_port = htons(port);

        if (IN6_IS_ADDR_UNSPECIFIED(&opt_bind_addr)) {
            sin4.sin_addr.s_addr = INADDR_ANY;
        } else if (IN6_IS_ADDR_V4MAPPED(&opt_bind_addr) ||
                   IN6_IS_ADDR_V4COMPAT(&opt_bind_addr)) {
            sin4.sin_addr.s_addr = ((uint32_t*)&opt_bind_addr)[3];
        } else {
            fprintf(stderr, "Invalid bind address specified\n");
            exit(1);
        }

        sin = (const struct sockaddr*)&sin4;
        sin_len = sizeof(sin4);
    }

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*)&on, sizeof(on));

    int bind_attempts = 0;
    while (bind(sock, sin, sin_len)) {
        if (++bind_attempts >= 100) {
            perror("bind");
            logmsg(LOG_CRIT, "Couldn't bind to tcp port %d after 100 attempts", port);
            exit(1);
        }
        logmsg(LOG_CRIT, "Bind to tcp port %d failed, retrying (%d/100)", port, bind_attempts);
        usleep(timeout_ms * 1000);
    }

    if (listen(sock, SOMAXCONN)) {
        perror("listen");
        fprintf(stderr, "Couldn't listen on tcp socket\n");
        exit(1);
    }

    transp = svc_vc_create(sock, 0, 0);

    if (transp == NULL) {
        fprintf(stderr, "Cannot create tcp service.\n");
        daemon_exit(0);
    }

    return transp;
}

static void unregister_transport(SVCXPRT **transp){
    // same as the comment above
    if (transp && *transp) {
        xprt_unregister(*transp);
        int fd = (*transp)->xp_fd;
        if (fd >= 0) (void)shutdown(fd, SHUT_RDWR);
        svc_destroy(*transp);
        *transp = NULL;
    }
}

static void unregister_nfs_service(SVCXPRT **udptransp, SVCXPRT **tcptransp){
    unregister_transport(udptransp);
    unregister_transport(tcptransp);
}


/* Take over NFS and MOUNT services when this node becomes leader */
static void become_leader(void)
{
    logmsg(LOG_CRIT, "Leadership acquired by %d, registering NFS and MOUNT services", opt_raft_id);

    if (!opt_tcponly)
        nfs_udptransp = create_udp_transport(opt_nfs_port);
    else
        nfs_udptransp = NULL;
    nfs_tcptransp = create_tcp_transport(opt_nfs_port);

    logmsg(LOG_INFO, "Leader binding NFS service to port %d", opt_nfs_port);
    register_nfs_service(nfs_udptransp, nfs_tcptransp);

    if (opt_mount_port != opt_nfs_port) {

        logmsg(LOG_CRIT, "This shouldn't happen. Or change the code such that handles this case.");
        exit(1);

        if (!opt_tcponly)
            mount_udptransp = create_udp_transport(opt_mount_port);
        else
            mount_udptransp = NULL;
        mount_tcptransp = create_tcp_transport(opt_mount_port);
    } else {
        mount_udptransp = nfs_udptransp;
        mount_tcptransp = nfs_tcptransp;
    }

    logmsg(LOG_INFO, "Leader binding MOUNT service to port %d", opt_mount_port);
    register_mount_service(mount_udptransp, mount_tcptransp);
}


static void leadership_lost(void) {
    logmsg(LOG_CRIT, "Leadership lost by %d, unregistering NFS and MOUNT services", opt_raft_id);
    unregister_nfs_service(&nfs_udptransp, &nfs_tcptransp);
}


/* Run RPC service. This is our own implementation of svc_run(), which
   allows us to handle other events as well. */
static void unfs3_svc_run(void)
{
#if defined(HAVE_SVC_GETREQ_POLL) && HAVE_DECL_SVC_POLLFD
    int r;
    struct pollfd *pollfds = NULL;
    int pollfds_len = 0;
#else
    fd_set readfds;
    struct timeval tv;
#endif

    for (;;) {
        fd_cache_close_inactive();

#if defined(HAVE_SVC_GETREQ_POLL) && HAVE_DECL_SVC_POLLFD

        if (svc_max_pollfd == 0) {
            usleep(timeout_ms * 1000);
        } else { ///////////// original code //// 
        if (pollfds_len != svc_max_pollfd) {
            pollfds = realloc(pollfds, sizeof(struct pollfd) * svc_max_pollfd);
            if (pollfds == NULL) {
                perror("unfs3_svc_run: realloc failed");
                return;
            }
            pollfds_len = svc_max_pollfd;
        }

        for (int i = 0; i < svc_max_pollfd; i++) {
            pollfds[i].fd = svc_pollfd[i].fd;
            pollfds[i].events = svc_pollfd[i].events;
            pollfds[i].revents = 0;
        }

        r = poll(pollfds, svc_max_pollfd, timeout_ms);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("unfs3_svc_run: poll failed");
            return;
        } else if (r)
            svc_getreq_poll(pollfds, r);
        ///////////// end original code ////     
        } 

        raft_make_progress(); 

        if(was_leader && raft_disabled) {
            logmsg(LOG_CRIT, "Raft disabled, stepping down from leadership");
            raft_become_follower(raft_srv);
            leadership_lost();
            is_leader_now = 0;
            was_leader = 0;
        } else {
            is_leader_now = raft_is_leader(raft_srv);

            if (is_leader_now && !was_leader) {
                become_leader();
            } else if(!is_leader_now && was_leader) {
                leadership_lost();
            }

            was_leader = is_leader_now;
            print_leader_info(); 
        }



#else
        readfds = svc_fdset;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        /* Note: On Windows, it's not possible to call select with all sets
           empty; to use it as a sleep function. In our case, however,
           readfds should never be empty, since we always have our listen
           socket. Well, at least hope that our Windows RPC library works
           like that. oncrpc-ms does. */
        switch (select(FD_SETSIZE, &readfds, NULL, NULL, &tv)) {
            case -1:
                if (errno == EINTR) {
                    continue;
                }
                perror("unfs3_svc_run: select failed");
                return;
            case 0:
                /* timeout */
                continue;
            default:
                svc_getreqset(&readfds);
        }
        
        logmsg(LOG_CRIT, "THIS MODE IS ASSUMED TO BE NOT REACHED");
        fflush(stderr);
        fflush(stdout); 
        exit(0);

#endif
    }

#if defined(HAVE_SVC_GETREQ_POLL) && HAVE_DECL_SVC_POLLFD
    free(pollfds);
#endif
}

/*
 * Generate write verifier based on PID and current time
 */
void regenerate_write_verifier(void)
{
    *(wverf + 0) = (uint32) getpid();
    *(wverf + 0) ^= rand();
    *(wverf + 4) = (uint32) time(NULL);
}

/*
 * Change readdir cookie value
 */
void change_readdir_cookie(void)
{
    if(opt_32_bit_truncate) {
        rcookie = rcookie >> 20;
        ++rcookie;
        rcookie &= 0xFFF;
        rcookie = rcookie << 20;
    } else {
        rcookie = rcookie >> 32;
        ++rcookie;
        rcookie = rcookie << 32;
    }
}



static void disable_raft(int signo) {
    (void)signo;
    raft_disabled = 1;
    // async-signal-safe write to prove we got here
    const char msg[] = "SIGUSR2: raft disabled\n";
    write(STDOUT_FILENO, msg, sizeof msg - 1);
}

static void enable_raft(int signo) {
    (void)signo;
    raft_disabled = 0;
    const char msg[] = "SIGUSR1: raft enabled\n";
    write(STDOUT_FILENO, msg, sizeof msg - 1);
}

static void install_handler(int sig, void (*handler)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handler;
    sa.sa_flags = SA_RESTART;   // usually helpful; not required
    if (sigaction(sig, &sa, NULL) == -1) {
        perror("sigaction");
        _exit(2);
    }
    logmsg(LOG_INFO, "Installed handler for signal %d", sig);
}


/*
 * NFSD main function
 * originally generated by rpcgen
 * forking, logging, options, and signal handler stuff added
 */
int main(int argc, char **argv)
{
    register SVCXPRT *tcptransp = NULL, *udptransp = NULL;
    pid_t pid = 0;

#ifndef WIN32
    struct sigaction act;
    sigset_t actset;
#endif				       /* WIN32 */
    int res;

    int pipefd[2];

    /* flush stdout after each newline */
    setvbuf(stdout, NULL, _IOLBF, 0);

#ifdef WIN32
    /* Windows doesn't have line buffering, and it even buffers
       stderr by default, so turn off all buffering */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#endif

    /* Clear opt_bind_addr structure before we use it */
    memset(&opt_bind_addr, 0, sizeof(opt_bind_addr));

    opt_bind_addr = in6addr_any;

    parse_options(argc, argv);
    if (optind < argc) {
        fprintf(stderr, "Error: extra arguments on command line\n");
        exit(1);
    }

    /* create pid file if wanted */
    // had to move this higher up, beacuse sometimes the election takes a while. 
    create_pid_file();

    raft_init();
    int replica_election_timeout = (opt_raft_id + 10) * election_timeout_ms; 
    logmsg(LOG_CRIT, "Setting election timeout to %d ms", replica_election_timeout);
    raft_set_election_timeout(raft_srv, replica_election_timeout);
    was_leader = is_leader_now = wait_for_leader();
    
    /* init write verifier */
    regenerate_write_verifier();

    res = backend_init();
    if (res == -1) {
        fprintf(stderr, "Backend initialization failed\n");
        daemon_exit(0);
    }

    /* config test mode */
    if (opt_testconfig) {
        res = exports_parse();
        if (res) {
            exit(0);
        } else {
            fprintf(stderr, "Parse error in `%s'\n", opt_exports);
            exit(1);
        }
    }

    if (opt_detach) {
        /* prepare syslog access */
        openlog("unfsd", LOG_CONS | LOG_PID, LOG_DAEMON);
    }

    if (is_leader_now) {
        become_leader();
    }
    

#ifndef WIN32
    if (opt_detach) {
        if (pipe(pipefd) == -1) {
            fprintf(stderr, "Could not create a pipe\n");
            exit(1);
        }

        pid = fork();
        if (pid == -1) {
            fprintf(stderr, "Could not fork into background\n");
            daemon_exit(0);
        }
        if (pid > 0) {
            char buf;
            close(pipefd[1]);
            while (read(pipefd[0], &buf, 1) > 0) {
                // do nothing until pipe closes
            }
            close(pipefd[0]);
        }
    }
#endif				       /* WIN32 */

    if (!opt_detach || pid == 0) {
#ifndef WIN32

        logmsg(LOG_INFO, "unfsd starting%s",
               opt_detach ? " in background" : "");

               
        sigemptyset(&actset);
        act.sa_handler = daemon_exit;
        act.sa_mask = actset;
        act.sa_flags = 0;
        sigaction(SIGHUP, &act, NULL);
        sigaction(SIGTERM, &act, NULL);
        sigaction(SIGINT, &act, NULL);
        sigaction(SIGQUIT, &act, NULL);
        sigaction(SIGSEGV, &act, NULL);
        // sigaction(SIGUSR1, &act, NULL); // used to enable raft

        act.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &act, NULL);
        // sigaction(SIGUSR2, &act, NULL); // used to disable raft
        sigaction(SIGALRM, &act, NULL);

        install_handler(SIGUSR1, enable_raft);
        install_handler(SIGUSR2, disable_raft);

        /* don't make directory we started in busy */
        if(chdir("/") < 0) {
            fprintf(stderr, "Could not change working directory\n");
            daemon_exit(0);
        }

        /* detach from terminal */
        if (opt_detach) {
            setsid();
            fclose(stdin);
            fclose(stdout);
            fclose(stderr);
        }
#endif				       /* WIN32 */

        /* no umask to not screw up create modes */
        umask(0);


        /* initialize internal stuff */
        fh_cache_init();
        fd_cache_init();
        get_squash_ids();
        exports_parse();

        if (opt_detach) {
            close(pipefd[0]);
            write(pipefd[1], "1", 1);
            close(pipefd[1]);
        }

        unfs3_svc_run();
        exit(1);
        /* NOTREACHED */
    }

    return 0;
}
