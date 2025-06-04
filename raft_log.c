#include "config.h"
#include "raft_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

static FILE *raft_log_fp = NULL;

void raft_log_init(const char *path)
{
    raft_log_fp = fopen(path, "a");
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
