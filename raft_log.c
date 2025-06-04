#include "config.h"
#include "raft_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <arpa/inet.h>

static FILE *raft_log_fp = NULL;

struct raft_log_header {
    uint32_t proc;
    uint32_t len;
};

void raft_log_init(const char *path)
{
    raft_log_fp = fopen(path, "ab");
    if (!raft_log_fp) {
        perror("raft_log_init");
    }
}

void raft_log_close(void)
{
    if (raft_log_fp) {
        fclose(raft_log_fp);
        raft_log_fp = NULL;
    }
}

void raft_log(const char *fmt, ...)
{
    if (!raft_log_fp)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(raft_log_fp, fmt, ap);
    fprintf(raft_log_fp, "\n");
    fflush(raft_log_fp);
    va_end(ap);
}

void raft_log_entry(uint32_t proc, const void *data, uint32_t len)
{
    if (!raft_log_fp)
        return;

    struct raft_log_header hdr;
    hdr.proc = htonl(proc);
    hdr.len = htonl(len);

    fwrite(&hdr, sizeof(hdr), 1, raft_log_fp);
    if (len > 0 && data != NULL)
        fwrite(data, len, 1, raft_log_fp);
    fflush(raft_log_fp);
}

