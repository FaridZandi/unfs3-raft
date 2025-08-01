/*
 * UNFS3 server framework
 * (C) 2004, Pascal Schmidt
 * see file LICENSE for license details
 */

#ifndef UNFS3_DAEMON_H
#define UNFS3_DAEMON_H

#include "nfs.h"

/* exit status for internal errors */
#define CRISIS	99

/* HP-UX does not have seteuid() and setegid() */
#if HAVE_SETEUID == 0 && HAVE_SETRESUID == 1
#define seteuid(u) setresuid(-1, u, -1)
#endif
#if HAVE_SETEGID == 0 && HAVE_SETRESGID == 1
#define setegid(g) setresgid(-1, g, -1)
#endif

/* error handling */
void daemon_exit(int);
void logmsg(int, const char *, ...);


#define mytimout 1000 /* 1 second timeout for svc_getreqset() */

/* remote address */
int get_remote(struct svc_req *, struct in6_addr *);
short get_port(struct svc_req *);
int get_socket_type(struct svc_req *rqstp);

/* write verifier */
extern writeverf3 wverf;
void regenerate_write_verifier(void);

/* readdir cookie */
extern cookie3 rcookie;
void change_readdir_cookie(void);

/* options */
extern int	opt_detach;
extern char	*opt_exports;
extern int	opt_cluster;
extern char	*opt_cluster_path;
extern int	opt_singleuser;
extern int	opt_brute_force;
extern int	opt_readable_executables;
extern int      opt_32_bit_truncate;

#endif
