#ifndef BUFFER_LOG_INTERNAL_H
#define BUFFER_LOG_INTERNAL_H

#include <stdbool.h>
#include "quard_soc_log.h"

extern bool log_flush_enable;

void flush_log(bool force, slave_buffer_cb *master_buf, uint32_t size);
void atomic_set_buf_full(slave_buffer_cb * buf_cb);
uint32_t rb_output_internal(slave_buffer_cb *master_buf, uint32_t size, const char *log, uint32_t len);

#endif /* BUFFER_LOG_INTERNAL_H */
