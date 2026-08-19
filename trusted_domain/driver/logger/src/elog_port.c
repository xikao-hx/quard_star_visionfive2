/*
 * This file is part of the EasyLogger Library.
 *
 * Copyright (c) 2015, Armink, <armink.ztl@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * 'Software'), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Function: Portable interface for each platform.
 * Created on: 2015-04-28
 */
 
#include <elog.h>
#include <FreeRTOS.h>
#include <semphr.h>
#include <message_buffer.h>
#include <task.h>
#include "ns16550.h"
#include <stdio.h>
#include "shell.h"
#include "shell_port.h"
#include "buffer_be_mcu.h"
#include "irq.h"

#define ISR_LOG_BUF_SIZE        (ELOG_LINE_BUF_SIZE * 8)
#define ISR_LOG_TASK_STACK_SIZE 512
#define ISR_LOG_TASK_PRIORITY   4

static SemaphoreHandle_t g_log_mutex = NULL;
static MessageBufferHandle_t isr_log_buf;
static StaticMessageBuffer_t isr_log_buf_static;
static uint8_t isr_log_storage[ISR_LOG_BUF_SIZE];
static StaticTask_t isr_log_task_buf;
static StackType_t isr_log_task_stack[ISR_LOG_TASK_STACK_SIZE];

static void elog_port_output_task(const char *log, size_t size);

static void isr_log_flush_task(void *param)
{
    char log[ELOG_LINE_BUF_SIZE];
    size_t size;

    (void)param;

    while (1) {
        size = xMessageBufferReceive(isr_log_buf, log, sizeof(log), portMAX_DELAY);
        if (size == 0) {
            continue;
        }

        if (g_log_mutex != NULL) {
            xSemaphoreTake(g_log_mutex, portMAX_DELAY);
        }

        elog_port_output_task(log, size);

        if (g_log_mutex != NULL) {
            xSemaphoreGive(g_log_mutex);
        }
    }
}

/**
 * EasyLogger port initialize
 *
 * @return result
 */
ElogErrCode elog_port_init(void) {
    ElogErrCode result = ELOG_NO_ERR;

    /* add your code here */
    g_log_mutex = xSemaphoreCreateMutex();
    if (g_log_mutex == NULL) {
        LOG_I("Failed to create mutex\n");
        return -1;
    }

    isr_log_buf = xMessageBufferCreateStatic(sizeof(isr_log_storage),
                                             isr_log_storage,
                                             &isr_log_buf_static);
    if (isr_log_buf == NULL) {
        LOG_I("Failed to create isr log buffer\n");
        return -1;
    }

    if (xTaskCreateStatic(isr_log_flush_task, "isr_log",
                          ISR_LOG_TASK_STACK_SIZE, NULL,
                          ISR_LOG_TASK_PRIORITY,
                          isr_log_task_stack,
                          &isr_log_task_buf) == NULL) {
        LOG_I("Failed to create isr log task\n");
        return -1;
    }

    return result;
}

/**
 * EasyLogger port deinitialize
 *
 */
void elog_port_deinit(void) {

    /* add your code here */
    if (g_log_mutex != NULL) {
        vSemaphoreDelete(g_log_mutex);
        g_log_mutex = NULL;
    }
}

static Shell* remote_shell = NULL;
static void elog_port_output_task(const char *log, size_t size)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
        uartputs_sync(log, size);
    } else {
        uart_puts(log, size);
    }
    
    Shell* current_shell = shellGetCurrent();
    if(current_shell != NULL)
        remote_shell = current_shell;
#if defined(REMOTE_SHELL) || defined(UART_REMOTE_SHELL)
    if(remote_shell_init_done && (current_shell != NULL))
    {
        // shellWriteString(remote_shell, log);
        extern short remote_shell_write(char *data, unsigned short len);
        (void)remote_shell_write((char *)log, (unsigned short)size);
    }
#endif

    // log_rb_backend_output(log, size);
}

/**
 * output log port interface
 *
 * @param log output of log
 * @param size log size
 */
void elog_port_output(const char *log, size_t size) {

    if (irq_is_in_isr()) {
        BaseType_t high_pri_task_woken = pdFALSE;

        if (isr_log_buf == NULL ||
            xMessageBufferSendFromISR(isr_log_buf, log, size,
                                      &high_pri_task_woken) != size) {
            return;
        }

        return;
    }

    elog_port_output_task(log, size);
}

/**
 * output lock
 */
void elog_port_output_lock(void) {
    
    /* add your code here */
    if (irq_is_in_isr()) {
        return;
    }

    if (g_log_mutex != NULL) {
        xSemaphoreTake(g_log_mutex, portMAX_DELAY);
    }
}

/**
 * output unlock
 */
void elog_port_output_unlock(void) {
    
    /* add your code here */
    if (irq_is_in_isr()) {
        return;
    }

    if (g_log_mutex != NULL) {
        xSemaphoreGive(g_log_mutex);
    }
}

/**
 * get current time interface
 *
 * @return current time
 */
const char *elog_port_get_time(void) {
    
    /* add your code here */
    static char time_buf[20];

    if (irq_is_in_isr()) {
        return "ISR";
    }

    uint32_t ticks = xTaskGetTickCount();  // FreeRTOS tick
    uint32_t ms = ticks % 1000;
    uint32_t sec = (ticks / 1000) % 60;
    uint32_t min = (ticks / 60000) % 60;
    uint32_t hour = ticks / 3600000;
    snprintf(time_buf, sizeof(time_buf), "%02u:%02u:%02u.%03u",
             hour, min, sec, ms);
    return time_buf;
}

/**
 * get current process name interface
 *
 * @return current process name
 */
const char *elog_port_get_p_info(void) {
    
    /* add your code here */
    if (irq_is_in_isr()) {
        return "ISR";
    }

    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    return (current_task != NULL) ? pcTaskGetName(current_task) : "Unknown";
}

/**
 * get current thread name interface
 *
 * @return current thread name
 */
const char *elog_port_get_t_info(void) {
    
    /* add your code here */
    if (irq_is_in_isr()) {
        return "ISR";
    }

    return "main";
}
