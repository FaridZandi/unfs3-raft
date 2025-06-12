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

enum entry_type {
    ENTRY_HANDLE,
    ENTRY_RENAME
};

struct handle_entry {
    enum entry_type type;
    char client[INET6_ADDRSTRLEN];
    char path[NFS_MAXPATHLEN];
    char handle_hex[FH_MAXBUF * 2 + 1];
    char new_path[NFS_MAXPATHLEN];
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

static void add_entry(struct handle_entry *e)
{
    e->next = NULL;
    if (!entries) {
        entries = entries_tail = e;
    } else {
        entries_tail->next = e;
        entries_tail = e;
    }
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
        char *token = strtok_r(line, " \t\n", &saveptr);
        if (!token)
            continue;
        if (strcmp(token, "RENAME") == 0) {
            char *oldp = strtok_r(NULL, " \t\n", &saveptr);
            char *newp = strtok_r(NULL, " \t\n", &saveptr);
            if (!oldp || !newp)
                continue;
            struct handle_entry *e = malloc(sizeof(*e));
            if (!e)
                break;
            e->type = ENTRY_RENAME;
            e->client[0] = '\0';
            strncpy(e->path, oldp, NFS_MAXPATHLEN);
            e->path[NFS_MAXPATHLEN - 1] = '\0';
            e->handle_hex[0] = '\0';
            strncpy(e->new_path, newp, NFS_MAXPATHLEN);
            e->new_path[NFS_MAXPATHLEN - 1] = '\0';
            add_entry(e);
        } else {
            char *client;
            char *p;
            char *hex;
            if (strcmp(token, "HANDLE") == 0) {
                client = strtok_r(NULL, " \t\n", &saveptr);
                p = strtok_r(NULL, " \t\n", &saveptr);
                hex = strtok_r(NULL, " \t\n", &saveptr);
            } else {
                client = token;
                p = strtok_r(NULL, " \t\n", &saveptr);
                hex = strtok_r(NULL, " \t\n", &saveptr);
            }
            if (!client || !p || !hex)
                continue;
            struct handle_entry *e = malloc(sizeof(*e));
            if (!e)
                break;
            e->type = ENTRY_HANDLE;
            strncpy(e->client, client, INET6_ADDRSTRLEN);
            e->client[INET6_ADDRSTRLEN - 1] = '\0';
            strncpy(e->path, p, NFS_MAXPATHLEN);
            e->path[NFS_MAXPATHLEN - 1] = '\0';
            strncpy(e->handle_hex, hex, FH_MAXBUF * 2);
            e->handle_hex[FH_MAXBUF * 2] = '\0';
            e->new_path[0] = '\0';
            add_entry(e);
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
}

void handle_log_record(const char *client, const char *path, const nfs_fh3 *fh)
{
    if (!handle_fp)
        return;
    struct handle_entry *e = malloc(sizeof(*e));
    if (!e)
        return;
    e->type = ENTRY_HANDLE;
    strncpy(e->client, client, INET6_ADDRSTRLEN);
    e->client[INET6_ADDRSTRLEN - 1] = '\0';
    strncpy(e->path, path, NFS_MAXPATHLEN);
    e->path[NFS_MAXPATHLEN - 1] = '\0';
    fh_to_hex(fh, e->handle_hex);
    e->new_path[0] = '\0';
    add_entry(e);

    logmsg(LOG_INFO, "handle_log_record: client=%s path=%s fh=%s", client, path,
           e->handle_hex);

    fprintf(handle_fp, "HANDLE %s %s %s\n", e->client, e->path, e->handle_hex);
    fflush(handle_fp);
}

void handle_log_record_rename(const char *oldpath, const char *newpath)
{
    if (!handle_fp)
        return;
    struct handle_entry *e = malloc(sizeof(*e));
    if (!e)
        return;
    e->type = ENTRY_RENAME;
    e->client[0] = '\0';
    strncpy(e->path, oldpath, NFS_MAXPATHLEN);
    e->path[NFS_MAXPATHLEN - 1] = '\0';
    e->handle_hex[0] = '\0';
    strncpy(e->new_path, newpath, NFS_MAXPATHLEN);
    e->new_path[NFS_MAXPATHLEN - 1] = '\0';
    add_entry(e);

    logmsg(LOG_INFO, "handle_log_rename: %s -> %s", oldpath, newpath);

    fprintf(handle_fp, "RENAME %s %s\n", oldpath, newpath);
    fflush(handle_fp);
}

const char *handle_log_lookup(const char *client, const nfs_fh3 *fh)
{
    char hex[FH_MAXBUF * 2 + 1];
    fh_to_hex(fh, hex);
    struct handle_entry *e;
    const char *path = NULL;
    for (e = entries; e; e = e->next) {
        if (!path && e->type == ENTRY_HANDLE &&
            strcmp(e->client, client) == 0 &&
            strcmp(e->handle_hex, hex) == 0) {
            path = e->path;
            continue;
        }
        if (path && e->type == ENTRY_RENAME && strcmp(e->path, path) == 0) {
            path = e->new_path;
        }
    }
    if (path)
        logmsg(LOG_INFO, "handle_log_lookup: found path %s for client=%s fh=%s", path, client, hex);
    else
        logmsg(LOG_INFO, "handle_log_lookup: no entry for client=%s fh=%s", client, hex);
    return path;
}
