%{
/*
 * UNFS3 exports parser and export controls
 * (C) 2004, Pascal Schmidt
 * see file LICENSE for license details
 */
#include "config.h"

#ifdef WIN32
#include <ws2tcpip.h>
#endif

#include <assert.h>
#include <rpc/rpc.h>
#include <limits.h>

#ifdef WIN32
#include "winsupport.h"
#else
#include <netdb.h>
#include <syslog.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif /* WIN32 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nfs.h"
#include "mount.h"
#include "daemon.h"
#include "backend.h"
#include "exports.h"

#ifndef PATH_MAX
# define PATH_MAX	4096
#endif

/* for lack of a better place */
#ifdef __GNUC__
#define U(x) x __attribute__ ((unused))
#else
#define U(x) x
#endif

/* lexer stuff, to avoid compiler warnings */
int yylex(void);
extern FILE *yyin;

/*
 * C code used by yacc parser
 */

typedef struct {
        char			orig[NFS_MAXPATHLEN];
        int			options;
        char			password[PASSWORD_MAXLEN+1];
        uint32			password_hash;
        struct in6_addr	addr;
        unsigned		prefix;
        uint32			anonuid;
        uint32			anongid;
        struct e_host		*next;
} e_host;

typedef struct {
        char		path[NFS_MAXPATHLEN];
        char		orig[NFS_MAXPATHLEN];
        e_host		*hosts;
        uint32          fsid; /* export point fsid (for removables) */
        struct e_item	*next;
} e_item;

/* export list, item, and host filled during parse */
static e_item *e_list = NULL;
static e_item cur_item;
static e_host cur_host;

/* last looked-up anonuid and anongid */
static uint32 last_anonuid = ANON_NOTSPECIAL;
static uint32 last_anongid = ANON_NOTSPECIAL;

/* mount protocol compatible variants */
static exports ne_list = NULL;
static struct exportnode ne_item;
static struct groupnode ne_host;

/* error status of last parse */
int e_error = FALSE;

/*
 * The FNV1a-32 hash algorithm
 * (http://www.isthe.com/chongo/tech/comp/fnv/)
 */
uint32 fnv1a_32(const char *str)
{
    return fnv1a_32_update(str, 0x811c9dc5);
}

uint32 fnv1a_32_update(const char *str, uint32 hval)
{
    static const uint32 fnv_32_prime = 0x01000193;
    
    while (*str) {
        hval ^= *str++;
        hval *= fnv_32_prime;
    }
    return hval;
}

#ifdef WIN32
uint32 wfnv1a_32(const wchar_t *str)
{
    static const uint32 fnv_32_prime = 0x01000193;
    uint32 hval = 0x811c9dc5;
    
    while (*str) {
        hval ^= *str++;
        hval *= fnv_32_prime;
    }
    return hval;
}
#endif

/*
 * get static fsid, for use with removable media export points
 */
static uint32 get_free_fsid(const char *path)
{
    uint32 hval;

    /* The 32:th bit is set to one on all special filehandles. The
       last 31 bits are hashed from the export point path. */
    hval = fnv1a_32(path);
    hval |= 0x80000000;
    return hval;
}


/*
 * clear current host
 */
static void clear_host(void)
{
        memset(&cur_host, 0, sizeof(e_host));
        strcpy(cur_host.orig, "<anon clnt>");
        memset(&ne_host, 0, sizeof(struct groupnode));

        cur_host.anonuid =
        cur_host.anongid = ANON_NOTSPECIAL;
}

/*
 * clear current item
 */
static void clear_item(void)
{
        memset(&cur_item, 0, sizeof(e_item));
        memset(&ne_item, 0, sizeof(struct exportnode));
}

/*
 * add current host to current export item
 */
static void add_host(void)
{
        e_host *new;
        e_host *iter;

        groups ne_new;
        groups ne_iter;

        new = malloc(sizeof(e_host));
        ne_new = malloc(sizeof(struct groupnode));
        if (!new || !ne_new) {
                logmsg(LOG_EMERG, "Out of memory, aborting");
                daemon_exit(CRISIS);
        }

        *new = cur_host;
        *ne_new = ne_host;
        ne_new->gr_name = new->orig;

        /* internal list */
        if (cur_item.hosts) {
                iter = cur_item.hosts;
                while (iter->next)
                        iter = (e_host *) iter->next;
                iter->next = (struct e_host *) new;
        } else
                cur_item.hosts = new;

        /* matching mount protocol list */
        if (ne_item.ex_groups) {
                ne_iter = ne_item.ex_groups;
                while (ne_iter->gr_next)
                        ne_iter = (groups) ne_iter->gr_next;
                ne_iter->gr_next = ne_new;
        } else
                ne_item.ex_groups = ne_new;

        clear_host();
}

/* 
   Normalize path, eliminating double slashes, etc. To be used instead
   of realpath, when realpath is not possible. Normalizing export
   points is important. Otherwise, mount requests might fail, since
   /x/y is not a prefix of ///x///y/ etc.
*/
char *normpath(const char *path, char *normpath)
{
        char *n;
        const char *p;

        /* Copy path to normpath, and replace blocks of slashes with
           single slash */
        p = path;
        n = normpath;
        while (*p) {
                /* Skip over multiple slashes */
                if (*p == '/' && *(p+1) == '/') {
                        p++;
                        continue;
                }
                *n++ = *p++;
        }
        *n = '\0';

        /* Remove trailing slash, if any. */
        if ((n - normpath) > 1 && *(n-1) == '/')
                *(n-1) = '\0';

        return normpath;
}

/*
 * add current item to current export list
 */
static void add_item(const char *path)
{
        char buf[PATH_MAX];
        e_item *new;
        e_item *iter;
        e_host *host;
        /* Is this item marked as removable for all hosts? */
        int removable_for_all = 1;

        exports ne_new;
        exports ne_iter;

        new = malloc(sizeof(e_item));
        ne_new = malloc(sizeof(struct exportnode));
        if (!new || !ne_new) {
                logmsg(LOG_EMERG, "Out of memory, aborting");
                daemon_exit(CRISIS);
        }

        /* Loop over all hosts and check if marked as removable. */
        host = cur_item.hosts;
        while (host) {
                if (!(host->options & OPT_REMOVABLE))
                        removable_for_all = 0;
                host = (e_host *) host->next;
        }

        if (removable_for_all) {
                /* If marked as removable for all hosts, don't try
                   realpath. */
                normpath(path, buf);
        } else if (!backend_realpath(path, buf)) {
                logmsg(LOG_CRIT, "realpath for %s failed", path);
                e_error = TRUE;
                free(new);
                free(ne_new);
                clear_item();
                return;
        }

        if (strlen(buf) + 1 > NFS_MAXPATHLEN) {
                logmsg(LOG_CRIT, "Attempted to export too long path");
                e_error = TRUE;
                free(new);
                free(ne_new);
                clear_item();
                return;
        }

        /* if no hosts listed, list default host */
        if (!cur_item.hosts)
                add_host();

        *new = cur_item;
        strcpy(new->path, buf);
        strcpy(new->orig, path);
        new->fsid = get_free_fsid(path);

        *ne_new = ne_item;
        ne_new->ex_dir = new->orig;

        /* internal list */
        if (e_list) {
                iter = e_list;
                while (iter->next)
                        iter = (e_item *) iter->next;
                iter->next = (struct e_item *) new;
        } else
                e_list = new;

        /* matching mount protocol list */
        if (ne_list) {
                ne_iter = ne_list;
                while (ne_iter->ex_next)
                        ne_iter = (exports) ne_iter->ex_next;
                ne_iter->ex_next = ne_new;
        } else
                ne_list = ne_new;

        clear_item();
}

/*
 * fill current host's address given a hostname
 */
static void set_hostname(const char *name)
{
        struct hostent *ent;

        if (strlen(name) + 1 > NFS_MAXPATHLEN) {
                logmsg(LOG_CRIT, "Hostname '%s' is too long", name);
                e_error = TRUE;
                return;
        }
        strcpy(cur_host.orig, name);

        // FIXME: We should use getaddrinfo() to get all addresses, and
        //        to get IPv6 addresses
        ent = gethostbyname(name);

        if (ent) {
                ((uint32_t*)&cur_host.addr)[0] = 0;
                ((uint32_t*)&cur_host.addr)[1] = 0;
                ((uint32_t*)&cur_host.addr)[2] = htonl(0xffff);
                ((uint32_t*)&cur_host.addr)[3] = ((const struct in_addr**)ent->h_addr_list)[0]->s_addr;

                cur_host.prefix = 128;
        } else {
                logmsg(LOG_CRIT, "Could not resolve hostname '%s'", name);
                e_error = TRUE;
        }
}	

/*
 * fill current host's address given an IP address
 */
static void set_ipv4addr(const char *addr)
{
        struct in_addr in4;

        strcpy(cur_host.orig, addr);

        if (inet_pton(AF_INET, addr, &in4) != 1) {
                logmsg(LOG_CRIT, "Could not parse IPv4 address '%s'", addr);
                e_error = TRUE;
        }

        ((uint32_t*)&cur_host.addr)[0] = 0;
        ((uint32_t*)&cur_host.addr)[1] = 0;
        ((uint32_t*)&cur_host.addr)[2] = htonl(0xffff);
        ((uint32_t*)&cur_host.addr)[3] = in4.s_addr;

        cur_host.prefix = 128;
}
static void set_ipv6addr(const char *addr)
{
        strcpy(cur_host.orig, addr);

        if (inet_pton(AF_INET6, addr, &cur_host.addr) != 1) {
                logmsg(LOG_CRIT, "Could not parse IPv6 address '%s'", addr);
                e_error = TRUE;
        }

        /* Convert compat addresses to mapped */
        if (IN6_IS_ADDR_V4COMPAT(&cur_host.addr)) {
                ((uint32_t*)&cur_host.addr)[0] = 0;
                ((uint32_t*)&cur_host.addr)[1] = 0;
                ((uint32_t*)&cur_host.addr)[2] = htonl(0xffff);
        }

        cur_host.prefix = 128;
}

/*
 * compute network prefix
 */
static unsigned long make_prefix(const char *mask) {
        struct in_addr addr;
        uint32_t haddr;
        int i, prefix;

        if (!inet_pton(AF_INET, mask, &addr)) {
                logmsg(LOG_CRIT, "Could not parse IPv4 network mask '%s'", mask);
                e_error = TRUE;
                return 0;
        }

        haddr = ntohl(addr.s_addr);

        prefix = 0;
        for (i = 0; i < 32; i++) {
                if (haddr & (1<<i)) {
                        prefix = 32 - i;
                        break;
                }
        }

        for (; i < 32; i++) {
                if (!(haddr & (1<<i))) {
                        logmsg(LOG_CRIT, "Can not convert IPv4 network mask '%s' to a prefix", mask);
                        e_error = TRUE;
                        break;
                }
        }

        return prefix;
}

/*
 * fill current host's address given IP address and netmask
 */
static void set_ipv4net(char *addr)
{
        char *pos, *net;

        pos = strchr(addr, '/');
        net = pos + 1;
        *pos = 0;

        set_ipv4addr(addr);

        cur_host.prefix = atoi(net) + 128-32;

        *pos = '/';
        strcpy(cur_host.orig, addr);
}

static void set_ipv6net(char *addr)
{
        char *pos, *net;

        pos = strchr(addr, '/');
        net = pos + 1;
        *pos = 0;

        set_ipv6addr(addr);

        cur_host.prefix = atoi(net);

        *pos = '/';
        strcpy(cur_host.orig, addr);
}

static void set_oldnet(char *addr)
{
        char *pos, *net;

        pos = strchr(addr, '/');
        net = pos + 1;
        *pos = 0;

        set_ipv4addr(addr);

        cur_host.prefix = make_prefix(net) + 128-32;

        *pos = '/';
        strcpy(cur_host.orig, addr);
}

/*
 * add an option bit to the current host
 */
static void add_option(const char *opt)
{
        if (strcmp(opt,"no_root_squash") == 0)
                cur_host.options |= OPT_NO_ROOT_SQUASH;
        else if (strcmp(opt,"root_squash") == 0)
                cur_host.options &= ~OPT_NO_ROOT_SQUASH;
        else if (strcmp(opt,"all_squash") == 0)
                cur_host.options |= OPT_ALL_SQUASH;
        else if (strcmp(opt,"no_all_squash") == 0)
                cur_host.options &= ~OPT_ALL_SQUASH;
        else if (strcmp(opt,"rw") == 0)
                cur_host.options |= OPT_RW;
        else if (strcmp(opt,"ro") == 0)
                cur_host.options &= ~OPT_RW;
        else if (strcmp(opt,"removable") == 0) {
                cur_host.options |= OPT_REMOVABLE;
        } else if (strcmp(opt,"fixed") == 0)
                cur_host.options &= ~OPT_REMOVABLE;
        else if (strcmp(opt,"insecure") == 0)
                cur_host.options |= OPT_INSECURE;
        else if (strcmp(opt,"secure") == 0)
                cur_host.options &= ~OPT_INSECURE;
        else
                logmsg(LOG_WARNING, "Warning: Unknown exports option `%s' ignored",
                        opt);
}

static void add_option_with_value(const char *opt, const char *val)
{
    if (strcmp(opt,"password") == 0) {
        if (strlen(val) > PASSWORD_MAXLEN) {
            logmsg(LOG_WARNING, "Warning: Password for export %s truncated to 64 chars",
                   cur_item.orig);
        }
        strncpy(cur_host.password, val, sizeof(password));
        cur_host.password[PASSWORD_MAXLEN] = '\0';
        /* Calculate hash */
        cur_host.password_hash = fnv1a_32(cur_host.password);
    } else if (strcmp(opt,"anonuid") == 0) {
        cur_host.anonuid = atoi(val);
    } else if (strcmp(opt,"anongid") == 0) {
        cur_host.anongid = atoi(val);
    } else {
        logmsg(LOG_WARNING, "Warning: Unknown exports option `%s' ignored",
            opt);
    }
}

/*
 * dummy error function
 */
void yyerror(U(char *s))
{
        logmsg(LOG_CRIT, "Parser error: %s", s);

        e_error = TRUE;
        return;
}

%}

%union {
        char text[NFS_MAXPATHLEN];
};

%token <text> PATH
%token <text> ID
%token <text> OPTVALUE
%token <text> WHITE
%token <text> IPV4
%token <text> IPV6
%token <text> IPV4NET
%token <text> IPV6NET
%token <text> OLDNET

%%

exports:
        export
        | export '\n' exports
        |
        ;

export:
        PATH			{ add_item($1); }
        | PATH WHITE hosts	{ add_item($1); }
        | PATH WHITE		{ add_item($1); }
        ;

hosts:
        host
        | host WHITE hosts
        | host WHITE
        ;

host:
        name			{ add_host(); }
        | name '(' opts ')'	{ add_host(); }
        | '(' opts ')'		{ add_host(); }
        ;

name:
        ID			{ set_hostname($1); }
        | IPV4			{ set_ipv4addr($1); }
        | IPV6			{ set_ipv6addr($1); }
        | IPV6NET		{ set_ipv6net($1); }
        | IPV4NET		{ set_ipv4net($1); }
        | OLDNET		{ set_oldnet($1); }
        ;

opts:
        opt		
        | opt ',' opts	
        |
        ;
 
opt: 
        ID                      { add_option($1); }
        | ID OPTVALUE           { add_option_with_value($1,$2); } 
        ;
%%

/*
 * C code using yacc parser + access code for exports list
 */

/* effective export list and access flag */
static e_item *export_list = NULL;
static volatile int exports_access = FALSE;

/* mount protocol compatible exports list */
exports exports_nfslist = NULL;

/*
 * free NFS groups list
 */
void free_nfsgroups(groups group)
{
        groups list, next;

        list = group;
        while (list) {
                next = (groups) list->gr_next;
                free(list);
                list = next;
        }
}

/*
 * free NFS exports list
 */
void free_nfslist(exports elist)
{
        exports list, next;

        list = elist;
        while(list) {
                next = (exports) list->ex_next;
                free_nfsgroups(list->ex_groups);
                free(list);
                list = next;
        }
}

/*
 * free list of host structures
 */
static void free_hosts(e_item *item)
{
        e_host *host, *cur;

        host = item->hosts;
        while (host) {
                cur = host;
                host = (e_host *) host->next;
                free(cur);
        }
}

/*
 * free list of export items
 */
static void free_list(e_item *item)
{
        e_item *cur;

        while (item) {
                free_hosts(item);
                cur = item;
                item = (e_item *) item->next;
                free(cur);
        }
}

/*
 * print out the current exports list (for debugging)
 */
void print_list(void)
{
        char addrbuf[INET6_ADDRSTRLEN];

        e_item *item;
        e_host *host;

        item = e_list;

        while (item) {
                host = item->hosts;
                while (host) {
                        inet_ntop(AF_INET6, &host->addr,
                                  addrbuf, sizeof(addrbuf));
                        printf("%s: ip %s/%d options %i\n",
                               item->path, addrbuf,
                               host->prefix,
                               host->options);
                        host = (e_host *) host->next;
                }
                item = (e_item *) item->next;
        }
}

/*
 * clear current parse state
 */
static void clear_cur(void)
{
        e_list = NULL;
        ne_list = NULL;
        e_error = FALSE;
        clear_host();
        clear_item();
}

/*
 * parse an exports file
 */
int exports_parse(void)
{
        FILE *efile;

        /*
         * if we are in the SIGHUP handler, a may_mount or get_options
         * may currently be accessing the list
         */
        if (exports_access) {
                logmsg(LOG_CRIT, "Export list is being traversed, no reload\n");
                return FALSE;
        }

        efile = fopen(opt_exports, "r");
        if (!efile) {
                logmsg(LOG_CRIT, "Could not open '%s', exporting nothing",
                       opt_exports);
                free_list(export_list);
                free_nfslist(exports_nfslist);
                export_list = NULL;
                exports_nfslist = NULL;
                return FALSE;
        }

        yyin = efile;
        clear_cur();
        yyparse();
        fclose(efile);

        if (e_error) {
                logmsg(LOG_CRIT, "Syntax error in '%s', exporting nothing",
                       opt_exports);
                free_list(export_list);
                free_nfslist(exports_nfslist);
                export_list = NULL;
                exports_nfslist = NULL;
                return FALSE;
        }

        /* print out new list for debugging */
        if (!opt_detach)
                print_list();

        free_list(export_list);
        free_nfslist(exports_nfslist);
        export_list = e_list;
        exports_nfslist = ne_list;
        return TRUE;
}

/*
 * compare the network part of two addresses
 */
static int is_equal_addr(const struct in6_addr *a,
                         const struct in6_addr *b,
                         unsigned prefix)
{
        unsigned byte, bit;
        uint32_t mask[4], masked_a[4], masked_b[4];

        mask[0] = mask[1] = mask[2] = mask[3] = 0;

        for (byte = 0;byte < 4;byte++) {
                for (bit = 0;bit < 32;bit++) {
                        if (((byte * 32) + bit) >= prefix)
                                break;

                        mask[byte] |= htonl(1 << (31 - bit));
                }
        }

        assert(sizeof(struct in6_addr) == sizeof(uint32_t[4]));

        memcpy(&masked_a, a, sizeof(struct in6_addr));
        memcpy(&masked_b, b, sizeof(struct in6_addr));

        for (byte = 0;byte < 4;byte++) {
                masked_a[byte] &= mask[byte];
                masked_b[byte] &= mask[byte];
        }

        return memcmp(masked_a, masked_b, sizeof(uint32_t[4])) == 0;
}

/*
 * find a given host inside a host list, return options
 */
static e_host* find_host(const struct in6_addr *remote, e_item *item,
                     char **password, uint32 *password_hash)
{
        e_host *host;

        host = item->hosts;
        while (host) {
                if (is_equal_addr(remote, &host->addr, host->prefix)) {
                        if (password != NULL)
                                *password = host->password;
                        if (password_hash != NULL)
                                *password_hash = host->password_hash;
                        return host;
                }
                host = (e_host *) host->next;
        }
        return NULL;
}

/* options cache */
int exports_opts = -1;
const char *export_path = NULL; 
uint32 export_fsid = 0;
uint32 export_password_hash = 0;

/*
 * given a path, return client's effective options
 */
int exports_options(const char *path, struct svc_req *rqstp,
                    char **password, uint32 *fsid)
{
        e_item *list;
        struct in6_addr remote;
        unsigned int last_len = 0;

        exports_opts = -1;
        export_path = NULL;
        export_fsid = 0;
        last_anonuid = ANON_NOTSPECIAL;
        last_anongid = ANON_NOTSPECIAL;

        /* check for client attempting to use invalid pathname */
        if (!path || strstr(path, "/../"))
                return exports_opts;

        if (get_remote(rqstp, &remote)) 
                return exports_opts;

        /* protect against SIGHUP reloading the list */
        exports_access = TRUE;

        list = export_list;
        while (list) {
                /* longest matching prefix wins */
                if (strlen(list->path) > last_len    &&
#ifndef WIN32
                    strstr(path, list->path) == path) {
#else
                    !win_utf8ncasecmp(path, list->path, strlen(list->path))) {
#endif
                    e_host* cur_host = find_host(&remote, list, password, &export_password_hash);

                        if (fsid != NULL)
                                *fsid = list->fsid;
                        if (cur_host) {
                                exports_opts = cur_host->options;
                                export_path = list->path;
                                export_fsid = list->fsid;
                                last_len = strlen(list->path);
                                last_anonuid = cur_host->anonuid;
                                last_anongid = cur_host->anongid;
                        }
                }
                list = (e_item *) list->next;
        }
        exports_access = FALSE;
        return exports_opts;
}

/*
 * check whether path is an export point
 */
int export_point(const char *path)
{
        e_item *list;

        exports_access = TRUE;
        list = export_list;

        while (list) {
            if (strcmp(path, list->path) == 0) {
                exports_access = FALSE;
                return TRUE;
            }
            list = (e_item *) list->next;
        }
        exports_access = FALSE;
        return FALSE;
}

/*
 * return exported path from static fsid
 */
char *export_point_from_fsid(uint32 fsid)
{
    e_item *list;
    
    exports_access = TRUE;
    list = export_list;
    
    while (list) {
        if (list->fsid == fsid) {
            exports_access = FALSE;
            return list->path;
        }
        list = (e_item *) list->next;
    }
    exports_access = FALSE;
    return NULL;
}


/*
 * check whether export options of a path match with last set of options
 */
nfsstat3 exports_compat(const char *path, struct svc_req *rqstp)
{
        int prev;
        uint32 prev_anonuid, prev_anongid;

        prev = exports_opts;
        prev_anonuid = last_anonuid;
        prev_anongid = last_anongid;

        if (exports_options(path, rqstp, NULL, NULL) == prev &&
            last_anonuid == prev_anonuid &&
            last_anongid == prev_anongid)
                return NFS3_OK;
        else if (exports_opts == -1)
                return NFS3ERR_ACCES;
        else
                return NFS3ERR_XDEV;
}

/*
 * check whether options indicate rw mount
 */
nfsstat3 exports_rw(void)
{
        return NFS3_OK; 
        
        if (exports_opts != -1 && (exports_opts & OPT_RW))
                return NFS3_OK;
        else
                return NFS3ERR_ROFS;
}

/*
 * returns the last looked-up anonuid for a mount (ANON_NOTSPECIAL means none in effect)
 */
uint32 exports_anonuid(void)
{
        return last_anonuid;
}

/*
 * returns the last looked-up anongid for a mount (ANON_NOTSPECIAL means none in effect)
 */
uint32 exports_anongid(void)
{
        return last_anongid;
}
