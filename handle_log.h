#ifndef HANDLE_LOG_H
#define HANDLE_LOG_H

#include <rpc/rpc.h>
#include "nfs.h"

enum handle_log_op {
    HANDLE_LOG_ADD,
    HANDLE_LOG_RENAME,
    HANDLE_LOG_REMOVE
};

void handle_log_init(const char *path);
void handle_log_close(void);
void handle_log_record(const char *client, const char *path,
                       const nfs_fh3 *fh);
void handle_log_record_rename(const char *client, const char *oldpath,
                              const char *newpath);
void handle_log_record_remove(const char *client, const char *path);
const char *handle_log_lookup(const char *client, const nfs_fh3 *fh);

#endif /* HANDLE_LOG_H */
