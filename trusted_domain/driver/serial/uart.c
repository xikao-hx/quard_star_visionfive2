#include <stdint.h>
#include <string.h>
#include "ns16550.h"
#include <FreeRTOS.h>
#include <semphr.h>
#include <stdarg.h>
#include <stdio.h>

#define UART_LOG_BUFF_SIZE 1024
static char log_buf[UART_LOG_BUFF_SIZE] = {0};
static SemaphoreHandle_t xMutex = NULL;

void serial_init(void)
{
    uartinit();
    xMutex = xSemaphoreCreateMutex();
}

static int _puts(char *str)
{
    int counter = 0;
    if (!str)
    {
        return 0;
    }
    while (*str && (counter < UART_LOG_BUFF_SIZE))
    {
        if(*str == '\n')
	        uartputc_sync('\r');
	    uartputc_sync(*str++);
        counter++;
    }
    return counter;
}

int serial_print(char *fmt, ...)
{
    if(xMutex)
        xSemaphoreTake(xMutex, portMAX_DELAY);

    va_list args;
    int plen;
    va_start(args, fmt);
    plen = vsnprintf(log_buf, sizeof(log_buf)/sizeof(char) - 1, fmt, args);
    _puts(log_buf);
    va_end(args);

    if(xMutex)
        xSemaphoreGive(xMutex);    

    return plen;
}


