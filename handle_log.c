#include "config.h"
#include "handle_log.h"
#include "fh.h"
#include "daemon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#ifndef WIN32
#include <syslog.h>
#endif

struct handle_entry {
    enum handle_log_op op;
    char client[INET6_ADDRSTRLEN];
    char path[NFS_MAXPATHLEN];
    char path2[NFS_MAXPATHLEN];
    char handle_hex[FH_MAXBUF * 2 + 1];
    struct handle_entry *next;
};

static struct handle_entry *entries = NULL;
static struct handle_entry *entries_tail = NULL;
static FILE *handle_fp = NULL;

static void fh_to_hex(const nfs_fh3 *fh, char *out)
{
    size_t len = fh->data.data_len;
    const unsigned char *d = (const unsigned char *)fh->data.data_val;
    size_t i;
    for (i = 0; i < len && i < FH_MAXBUF; i++)
        sprintf(out + i * 2, "%02x", d[i]);
    out[i * 2] = '\0';
}

void handle_log_init(const char *path)
{
    handle_fp = fopen(path, "a+");
    if (!handle_fp)
        return;

    char line[INET6_ADDRSTRLEN + NFS_MAXPATHLEN * 2 + FH_MAXBUF * 2 + 16];
    rewind(handle_fp);
    while (fgets(line, sizeof(line), handle_fp)) {
        char *saveptr = NULL;
        char *op = strtok_r(line, " \t\n", &saveptr);
        char *client = strtok_r(NULL, " \t\n", &saveptr);
        if (!op || !client)
            continue;
        struct handle_entry *e = calloc(1, sizeof(*e));
        if (!e)
            break;
        if (strcmp(op, "H") == 0) {
            e->op = HANDLE_LOG_ADD;
            char *p = strtok_r(NULL, " \t\n", &saveptr);
            char *hex = strtok_r(NULL, " \t\n", &saveptr);
            if (!p || !hex) { free(e); continue; }
            strncpy(e->path, p, NFS_MAXPATHLEN);
            e->path[NFS_MAXPATHLEN - 1] = '\0';
            strncpy(e->handle_hex, hex, FH_MAXBUF * 2);
            e->handle_hex[FH_MAXBUF * 2] = '\0';
        } else if (strcmp(op, "R") == 0) {
            e->op = HANDLE_LOG_RENAME;
            char *from = strtok_r(NULL, " \t\n", &saveptr);
            char *to = strtok_r(NULL, " \t\n", &saveptr);
            if (!from || !to) { free(e); continue; }
            strncpy(e->path, from, NFS_MAXPATHLEN);
            e->path[NFS_MAXPATHLEN - 1] = '\0';
            strncpy(e->path2, to, NFS_MAXPATHLEN);
            e->path2[NFS_MAXPATHLEN - 1] = '\0';
        } else if (strcmp(op, "D") == 0) {
            e->op = HANDLE_LOG_REMOVE;
            char *p = strtok_r(NULL, " \t\n", &saveptr);
            if (!p) { free(e); continue; }
            strncpy(e->path, p, NFS_MAXPATHLEN);
            e->path[NFS_MAXPATHLEN - 1] = '\0';
        } else {
            free(e);
            continue;
        }
        strncpy(e->client, client, INET6_ADDRSTRLEN);
        e->client[INET6_ADDRSTRLEN - 1] = '\0';
        e->next = NULL;
        if (!entries) {
            entries = entries_tail = e;
        } else {
            entries_tail->next = e;
            entries_tail = e;
        }
    }
    fseek(handle_fp, 0, SEEK_END);
}

void handle_log_close(void)
{
    if (handle_fp)
        fclose(handle_fp);
    handle_fp = NULL;
    while (entries) {
        struct handle_entry *n = entries->next;
        free(entries);
        entries = n;
    }
    entries_tail = NULL;
}

void handle_log_record(const char *client, const char *path, const nfs_fh3 *fh)
{
    if (!handle_fp)
        return;
    struct handle_entry *e = calloc(1, sizeof(*e));
    if (!e)
        return;
    e->op = HANDLE_LOG_ADD;
    strncpy(e->client, client, INET6_ADDRSTRLEN);
    e->client[INET6_ADDRSTRLEN - 1] = '\0';
    strncpy(e->path, path, NFS_MAXPATHLEN);
    e->path[NFS_MAXPATHLEN - 1] = '\0';
    fh_to_hex(fh, e->handle_hex);
    e->next = NULL;
    if (!entries) {
        entries = entries_tail = e;
    } else {
        entries_tail->next = e;
        entries_tail = e;
    }

    logmsg(LOG_INFO, "handle_log_record: client=%s path=%s fh=%s", client, path,
           e->handle_hex);

    fprintf(handle_fp, "H %s %s %s\n", e->client, e->path, e->handle_hex);
    fflush(handle_fp);
}

void handle_log_record_rename(const char *client, const char *oldpath,
                              const char *newpath)
{
    if (!handle_fp)
        return;
    struct handle_entry *e = calloc(1, sizeof(*e));
    if (!e)
        return;
    e->op = HANDLE_LOG_RENAME;
    strncpy(e->client, client, INET6_ADDRSTRLEN);
    e->client[INET6_ADDRSTRLEN - 1] = '\0';
    strncpy(e->path, oldpath, NFS_MAXPATHLEN);
    e->path[NFS_MAXPATHLEN - 1] = '\0';
    strncpy(e->path2, newpath, NFS_MAXPATHLEN);
    e->path2[NFS_MAXPATHLEN - 1] = '\0';
    e->next = NULL;
    if (!entries) {
        entries = entries_tail = e;
    } else {
        entries_tail->next = e;
        entries_tail = e;
    }

    logmsg(LOG_INFO, "handle_log_record_rename: client=%s %s -> %s", client,
           oldpath, newpath);

    fprintf(handle_fp, "R %s %s %s\n", e->client, e->path, e->path2);
    fflush(handle_fp);
}

void handle_log_record_remove(const char *client, const char *path)
{
    if (!handle_fp)
        return;
    struct handle_entry *e = calloc(1, sizeof(*e));
    if (!e)
        return;
    e->op = HANDLE_LOG_REMOVE;
    strncpy(e->client, client, INET6_ADDRSTRLEN);
    e->client[INET6_ADDRSTRLEN - 1] = '\0';
    strncpy(e->path, path, NFS_MAXPATHLEN);
    e->path[NFS_MAXPATHLEN - 1] = '\0';
    e->next = NULL;
    if (!entries) {
        entries = entries_tail = e;
    } else {
        entries_tail->next = e;
        entries_tail = e;
    }

    logmsg(LOG_INFO, "handle_log_record_remove: client=%s path=%s", client,
           path);

    fprintf(handle_fp, "D %s %s\n", e->client, e->path);
    fflush(handle_fp);
}

const char *handle_log_lookup(const char *client, const nfs_fh3 *fh)
{
    char hex[FH_MAXBUF * 2 + 1];
    fh_to_hex(fh, hex);

    struct handle_entry *e;
    const char *path = NULL;

    logmsg(LOG_DEBUG, "handle_log_lookup: start lookup for client=%s fh=%s", client, hex);

    for (e = entries; e; e = e->next) {
        if (!path) {
            if (e->op == HANDLE_LOG_ADD &&
                strcmp(e->client, client) == 0 &&
                strcmp(e->handle_hex, hex) == 0) {
                path = e->path;
                logmsg(LOG_DEBUG, "handle_log_lookup: HANDLE_LOG_ADD matched for path=%s", path);
            }
        } else {
            if (strcmp(e->client, client) != 0)
                continue;
            if (e->op == HANDLE_LOG_RENAME && strcmp(e->path, path) == 0) {
                logmsg(LOG_DEBUG, "handle_log_lookup: HANDLE_LOG_RENAME: %s -> %s", path, e->path2);
                path = e->path2;
            } else if (e->op == HANDLE_LOG_REMOVE && strcmp(e->path, path) == 0) {
                logmsg(LOG_DEBUG, "handle_log_lookup: HANDLE_LOG_REMOVE for path=%s", path);
                path = NULL;
            }
        }
    }

    if (path)
        logmsg(LOG_INFO, "handle_log_lookup: found path %s for client=%s fh=%s",
               path, client, hex);
    else
        logmsg(LOG_INFO, "handle_log_lookup: no entry for client=%s fh=%s",
               client, hex);
    return path;
}
