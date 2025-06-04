#ifndef RAFT_LOG_H
#define RAFT_LOG_H

#include <stdarg.h>
#include <stdint.h>

void raft_log_init(const char *path);
void raft_log_close(void);
void raft_log(const char *fmt, ...);
void raft_log_entry(uint32_t proc, const void *data, uint32_t len);

#endif /* RAFT_LOG_H */
