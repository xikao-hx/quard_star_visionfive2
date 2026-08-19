#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"
#include "quard_star.h"
#include "memlayout.h"
#include "irq.h"
#include "elog.h"
#include "shell_port.h"

/* UART寄存器定义 */ 
#ifdef BOARD_JH7110
/* JH7110 NS16550 registers use reg-shift = 2 and 32-bit accesses. */
#define Reg(reg) ((volatile uint32_t *)(UART2 + ((reg) << 2)))
#else
#define Reg(reg) ((volatile uint8_t *)(UART2 + reg))
#endif

#define RHR 0
#define THR 0
#define IER 1
#define IER_RX_ENABLE (1<<0)
#define IER_TX_ENABLE (1<<1)
#define FCR 2
#define FCR_FIFO_ENABLE (1<<0)
#define FCR_FIFO_CLEAR (3<<1)
#define ISR 2
#define LCR 3
#define LCR_EIGHT_BITS (3<<0)
#define LCR_BAUD_LATCH (1<<7)
#define LSR 5
#define LSR_RX_READY (1<<0)
#define LSR_TX_IDLE (1<<5)

#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

#ifdef BOARD_JH7110
#define UART_BAUD_DIVISOR 13U /* 24 MHz input clock, 115200 baud. */
#endif

// FreeRTOS同步对象
static TaskHandle_t uart_tx_task_handle = NULL;

#define UART_SEND_TASK_PRI 4
#define UART_TX_BUF_SIZE 1024
#define UART_RX_BUF_SIZE 64
static QueueHandle_t uart_tx_queue = NULL;
static QueueHandle_t uart_rx_queue;

void uartintr(int irq, void *data);

/**
 * @brief UART发送任务
 * 
 */
void uart_tx_task(void *pvParameters)
{
    char c;
    while(1) {
        if (xQueueReceive(uart_tx_queue, &c, portMAX_DELAY) == pdTRUE) {
            while((ReadReg(LSR) & LSR_TX_IDLE) == 0) {
                __asm volatile("nop"); 
            }
            WriteReg(THR, c);
        }
    }
}

/**
 * @brief 初始化UART
 * 
 */
void uartinit(void)
{
    WriteReg(IER, 0x00);
    WriteReg(LCR, LCR_BAUD_LATCH);
#ifdef BOARD_JH7110
    WriteReg(0, UART_BAUD_DIVISOR & 0xff);
    WriteReg(1, (UART_BAUD_DIVISOR >> 8) & 0xff);
#else
    WriteReg(0, 0x03);
    WriteReg(1, 0x00);
#endif
    WriteReg(LCR, LCR_EIGHT_BITS);
    WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);
    WriteReg(4, 0x00); /* MCR: no modem control */
    (void)ReadReg(LSR); /* clear line status */
    (void)ReadReg(RHR); /* clear receive buffer */
    WriteReg(7, 0x00); /* SCR */
    /* TX is polled by uart_tx_task; enabling TX-empty IRQ without handling
     * it in uartintr() causes an interrupt storm on NS16550. */
    WriteReg(IER, IER_RX_ENABLE);

    uart_rx_queue = xQueueCreate(UART_RX_BUF_SIZE, sizeof(char));
    uart_tx_queue = xQueueCreate(UART_TX_BUF_SIZE, sizeof(char));

    hw_interrupt_install(UART2_IRQ, (isr_handler_t)uartintr, NULL, "ns16550");

    xTaskCreate(uart_tx_task, "UART_TX", 1024, NULL,
               UART_SEND_TASK_PRI, &uart_tx_task_handle);
}

/* Minimal early-boot probe.  This path intentionally does not use
 * FreeRTOS queues, tasks, interrupts, or critical-section primitives. */
void uart_poll_init(void)
{
    WriteReg(IER, 0x00);
    WriteReg(LCR, LCR_BAUD_LATCH);
    WriteReg(0, UART_BAUD_DIVISOR & 0xff);
    WriteReg(1, (UART_BAUD_DIVISOR >> 8) & 0xff);
    WriteReg(LCR, LCR_EIGHT_BITS);
    WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);
    WriteReg(4, 0x00); /* MCR */
    (void)ReadReg(LSR);
    (void)ReadReg(RHR);
    WriteReg(7, 0x00); /* SCR */
}

void uart_poll_puts(const char *str)
{
    while (*str) {
        while ((ReadReg(LSR) & LSR_TX_IDLE) == 0)
            ;
        WriteReg(THR, (uint32_t)*str++);
    }
}

// 添加到输出缓冲区
void uartputc(int c)
{
    if (uart_tx_queue == NULL) return;

    xQueueSend(uart_tx_queue, &c, portMAX_DELAY);
}

// 同步输出
void uartputc_sync(int c)
{
    taskENTER_CRITICAL();

    while((ReadReg(LSR) & LSR_TX_IDLE) == 0)
        ;
    WriteReg(THR, c);

    taskEXIT_CRITICAL();
}

// 读取输入字符
int uartgetc(void)
{
    if(ReadReg(LSR) & 0x01){
        return ReadReg(RHR);
    } else {
        return -1;
    }
}

// 非阻塞接收
int uart_getchar_nonblock(void)
{
    char c;
    if(xQueueReceive(uart_rx_queue, &c, 0) == pdTRUE){
        return c;
    }
    return -1;
}

// 阻塞接收
int uart_getchar_block(void)
{
    char c;
    if(xQueueReceive(uart_rx_queue, &c, portMAX_DELAY) == pdTRUE){
        return c;
    }
    return -1;
}

/**
 * @brief 同步非阻塞接收字符串
 * 
 */
int uartgets_sync(char *str, unsigned short len)
{
    unsigned short i = 0;
    int ch;

    while (i < len) {
        ch = uart_getchar_nonblock(); 
        if (ch != -1) {
            str[i] = (char)ch;
            i++;
        }
    }
    return i;
}

/**
 * @brief 异步阻塞接收字符串
 * 
 */
int uart_gets(char *str, unsigned short len)
{
    unsigned short i = 0;
    int ch;

    while (i < len) {
        ch = uart_getchar_block(); 
        if (ch != -1) {
            str[i] = (char)ch;
            i++;
        }
    }
    return i;
}

/**
 * @brief 异步阻塞发送字符串
 * 
 */
int uart_puts(const char *str, unsigned short len)
{
    unsigned short i = 0;
    for(; i < len && str[i] != '\0'; i++){
        if(str[i] == '\n')
	        uartputc('\r');
        uartputc(str[i]);
    }

    return i;
}

/**
 * @brief 同步阻塞发送字符串
 * 
 */
int uartputs_sync(const char *str, unsigned short len)
{
    unsigned short i = 0;
    taskENTER_CRITICAL(); 

    for(; i < len && str[i] != '\0'; i++){
        if(str[i] == '\n') {
            while((ReadReg(LSR) & LSR_TX_IDLE) == 0);
            WriteReg(THR, '\r');
        }
        
        while((ReadReg(LSR) & LSR_TX_IDLE) == 0); 
        WriteReg(THR, str[i]);
    }

    taskEXIT_CRITICAL(); 
    return i;
}

/**
 * @brief 中断处理 - 只处理接收
 * 
 */
void uartintr(int irq, void *data)
{
    while(1){
        int c = uartgetc();
        if(c == -1)
            break;
        /* The trap epilogue performs the context restore. */
        xQueueSendFromISR(uart_rx_queue, &c, NULL);
        #if defined(UART_SHELL)  || defined(UART_REMOTE_SHELL)
            extern void send_shell_queue(char c);
            send_shell_queue(c);
        #endif
    }

}
