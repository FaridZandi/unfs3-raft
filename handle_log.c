#include "config.h"
#include "handle_log.h"
#include "fh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

struct handle_entry {
    char client[INET6_ADDRSTRLEN];
    char path[NFS_MAXPATHLEN];
    char handle_hex[FH_MAXBUF * 2 + 1];
    struct handle_entry *next;
};

static struct handle_entry *entries = NULL;
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

    char line[INET6_ADDRSTRLEN + NFS_MAXPATHLEN + FH_MAXBUF * 2 + 8];
    rewind(handle_fp);
    while (fgets(line, sizeof(line), handle_fp)) {
        char *saveptr = NULL;
        char *client = strtok_r(line, " \t\n", &saveptr);
        char *p = strtok_r(NULL, " \t\n", &saveptr);
        char *hex = strtok_r(NULL, " \t\n", &saveptr);
        if (!client || !p || !hex)
            continue;
        struct handle_entry *e = malloc(sizeof(*e));
        if (!e)
            break;
        strncpy(e->client, client, INET6_ADDRSTRLEN);
        e->client[INET6_ADDRSTRLEN - 1] = '\0';
        strncpy(e->path, p, NFS_MAXPATHLEN);
        e->path[NFS_MAXPATHLEN - 1] = '\0';
        strncpy(e->handle_hex, hex, FH_MAXBUF * 2);
        e->handle_hex[FH_MAXBUF * 2] = '\0';
        e->next = entries;
        entries = e;
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
    strncpy(e->client, client, INET6_ADDRSTRLEN);
    e->client[INET6_ADDRSTRLEN - 1] = '\0';
    strncpy(e->path, path, NFS_MAXPATHLEN);
    e->path[NFS_MAXPATHLEN - 1] = '\0';
    fh_to_hex(fh, e->handle_hex);
    e->next = entries;
    entries = e;

    fprintf(handle_fp, "%s %s %s\n", e->client, e->path, e->handle_hex);
    fflush(handle_fp);
}

const char *handle_log_lookup(const char *client, const nfs_fh3 *fh)
{
    char hex[FH_MAXBUF * 2 + 1];
    fh_to_hex(fh, hex);
    struct handle_entry *e;
    for (e = entries; e; e = e->next)
        if (strcmp(e->client, client) == 0 && strcmp(e->handle_hex, hex) == 0)
            return e->path;
    return NULL;
}
