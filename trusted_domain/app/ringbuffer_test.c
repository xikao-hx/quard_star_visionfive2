#define LOG_TAG "RINGBUFFER_TEST"
#include "elog.h"
#include "shell.h"
#include "ringbuffer.h"
#include "quard_soc_log.h"
#include "uart.h"

/**
 * @brief ringbuffer 读测试
 * 格式: ringbuffer_read 1
 */
int ringbuffer_read(int argc, char *argv[]) {

    char log_buf[128];
    uint32_t read_len = 0;
    
    serial_print("--- Ringbuffer Buffer Status ---\n");
    serial_print("State: %s\n", (LOG_BUF_BASE->buffer_state == SLAVE_BUFFER_FULL) ? "FULL" : "EMPTY");
    serial_print("Magic: 0x%08X\n", LOG_BUF_BASE->head_magic);

    struct quard_ringbuffer *rb = (struct quard_ringbuffer *)&(LOG_BUF_BASE->rb_ctrl);
    if (quard_ringbuffer_status(rb) == QUARD_RINGBUFFER_EMPTY && 
        LOG_BUF_BASE->buffer_state == SLAVE_BUFFER_EMPTY) {
        serial_print("Buffer is empty.\n");
        return 0;
    }

    serial_print("magic pattern mismatch(expect:%x act_head:%x act_mid:%x\n)", 
            MAGIC_PATTERN, LOG_BUF_BASE->head_magic, LOG_BUF_BASE->middle_magic);

    // 循环读取并输出 Ringbuffer 里的数据
    serial_print("--- Buffer Content ---\n");
    while (1) {
        read_len = quard_ringbuffer_get(rb, (uint8_t *)log_buf, sizeof(log_buf) - 1);
        if (read_len == 0) {
            break;
        }
        
        log_buf[read_len] = '\0';   
        serial_print("%s", log_buf);      
    }

    return 0;
}

// 导出命令
SHELL_EXPORT_CMD(SHELL_CMD_PERMISSION(0)|SHELL_CMD_TYPE(SHELL_TYPE_CMD_MAIN)|SHELL_CMD_DISABLE_RETURN, 
                ringbuffer_read, ringbuffer_read, ringbuffer read);
