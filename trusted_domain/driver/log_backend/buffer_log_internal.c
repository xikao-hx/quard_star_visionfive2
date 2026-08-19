#define LOG_TAG     "LOG_INTERNAL"
#define LOG_LVL     ELOG_LVL_VERBOSE
#include "elog.h"
#include "ringbuffer.h"
#include "buffer_log_internal.h"
#include <string.h>
#include "uart.h"

bool log_flush_enable = false;

void atomic_set_buf_full(slave_buffer_cb * buf_cb)
{
    /* memory barrier */
    __asm volatile ("fence rw, w" ::: "memory"); 

    /* atomic set full */
    unsigned int set_val = (unsigned int)SLAVE_BUFFER_FULL;
    unsigned int tmp;
    int error;

    __asm volatile (
        "1: lr.w %0, (%2)\n"      
        "   sc.w %1, %3, (%2)\n"  
        "   bnez %1, 1b\n"        
        : "=&r" (tmp), "=&r" (error)
        : "r" (&buf_cb->buffer_state), "r" (set_val)
        : "memory"
    );

    /* sync barrier */
    __asm volatile ("fence rw, rw" ::: "memory");
}

/* Do not use LOG_* here: it may recursively enter elog_output_lock() and deadlock */
void flush_log(bool force, slave_buffer_cb *master_buf, uint32_t size)
{
    slave_buffer_cb * mst_cb = master_buf;

    if (quard_ringbuffer_status((struct quard_ringbuffer *)&(mst_cb->rb_ctrl)) == QUARD_RINGBUFFER_EMPTY) {
        return;
    }

    if(LOG_BUF_BASE->buffer_state == SLAVE_BUFFER_FULL) {
        if(force) {
            serial_print("Log buffer is full and force to flush\r\n");
        } else {
            serial_print("Log buffer is full and ignore current flush cmd\r\n");
            return;
        }
    }
    
    // arch_dcache_flush_range(master_buf, size);
    (void)memcpy((void *)LOG_BUF_BASE, (void *)master_buf, MCU_LOG_BUF_SIZE);
    quard_ringbuffer_reset((struct quard_ringbuffer *)&(mst_cb->rb_ctrl));

    atomic_set_buf_full(LOG_BUF_BASE);
}

uint32_t rb_output_internal(slave_buffer_cb *master_buf, uint32_t size, 
                            const char *log, uint32_t len)
{
    uint32_t put_size = 0;
    slave_buffer_cb * mst_cb = master_buf;

#if LOG_RB_FORCE_PUT
    put_size = quard_ringbuffer_put_force((struct quard_ringbuffer *)&(mst_cb->rb_ctrl), (const uint8_t*)log, len);
#else
    put_size = quard_ringbuffer_put((struct quard_ringbuffer *)&(mst_cb->rb_ctrl), (const uint8_t*)log, len);
#endif /* LOG_RB_FORCE_PUT */
    if (quard_ringbuffer_status((struct quard_ringbuffer *)&(mst_cb->rb_ctrl)) == QUARD_RINGBUFFER_FULL) {
#if LOG_RB_FORCE_PUT
        flush_log(true, master_buf, size);
#else
        if(LOG_BUF_BASE->buffer_state == SLAVE_BUFFER_EMPTY) {
            flush_log(false, master_buf, size);
            log_flush_enable = true;
        }
#endif
    }

    return put_size;
}