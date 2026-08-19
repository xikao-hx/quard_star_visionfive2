#define LOG_TAG     "MAIN"
#define LOG_LVL     ELOG_LVL_VERBOSE
#include "elog.h"
#include <FreeRTOS.h>
#include <task.h>
#include <stdio.h>
#include "sbi.h"
#include "riscv_asm.h"
#include "memlayout.h"
#include "plic.h"
#include "riscv.h"
#include "uart.h"
#include "shell.h"
#include "shell_port.h"
#include "quard_nor_agent.h"
#include "buffer_be_mcu.h"
#include "jh7110.h"
#include "quard_mbox_router.h"

int main(void)
{
    jh7110_uart_clock_init();
    plicinit();
    plicinithart();
    serial_init();
    serial_print("Hello FreeRTOS on VisionFive 2 (hart4)\r\n");
    // log_rb_backend_init();
    elog_init();
    elog_set_fmt(ELOG_LVL_INFO, ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME);
    elog_set_fmt(ELOG_LVL_DEBUG, ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME);
    elog_set_filter(ELOG_LVL_VERBOSE, "", "");
    elog_start();

    init_quard_mbox_router();
    userShellInit();
    
    LOG_I("Hello FreeRTOS!");

    enable_external_interrupt();
    vTaskStartScheduler();

    LOG_E("ERROR: Scheduler returned!");

	return 0;
}
