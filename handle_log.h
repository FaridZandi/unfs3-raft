#ifndef HANDLE_LOG_H
#define HANDLE_LOG_H

#include <rpc/rpc.h>
#include "nfs.h"

void handle_log_init(const char *path);
void handle_log_close(void);
void handle_log_record(const char *client, const char *path, const nfs_fh3 *fh);
const char *handle_log_lookup(const char *client, const nfs_fh3 *fh);

#endif /* HANDLE_LOG_H */
