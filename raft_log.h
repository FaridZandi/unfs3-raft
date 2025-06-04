#ifndef RAFT_LOG_H
#define RAFT_LOG_H

#include <stdarg.h>

void raft_log_init(const char *path);
void raft_log_close(void);
void raft_log(const char *fmt, ...);

#endif /* RAFT_LOG_H */
