#ifndef __LOG_BUFFER_MCU_H__
#define __LOG_BUFFER_MCU_H__

#include <stdint.h>

void soc_log_agent(void);
int log_rb_backend_init(void);

void exp_flush_log(void);
void log_rb_backend_output(const char *log, uint32_t len);
void rb_log_output(const char *log, uint32_t len);

#endif /* __LOG_BUFFER_MCU_H__ */