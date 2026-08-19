#ifndef __QUARD_SOC_LOG_H__
#define __QUARD_SOC_LOG_H__

#include <stdint.h>
#include "ringbuffer.h"
#include "../../../common_inc/bsp/quard_log.h"

#define LOG_BUF_BASE_ADDR   MCU_LOG_BUF_BASE_ADDR
#define LOG_BUF_BASE   ((slave_buffer_cb *)(uintptr_t)LOG_BUF_BASE_ADDR)

enum quard_log_buffer_state
{
    SLAVE_BUFFER_EMPTY  = 0,
    SLAVE_BUFFER_FULL   = 1,
};

typedef struct
{
    uint32_t head_magic;
    uint32_t buffer_state;
    struct quard_ringbuffer rb_ctrl;
    uint32_t middle_magic;
    uint32_t buffer_first_byte;
} slave_buffer_cb;

#define MAGIC_PATTERN   (0xF1F11F1F)

#endif /* __QUARD_SOC_LOG_H__ */
