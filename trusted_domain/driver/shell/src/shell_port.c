#define LOG_TAG "SHELL_PORT"
#include "elog.h"
#include "shell.h"
#include "ns16550.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "queue.h"
#include <string.h>
#include "shell.h"
#include <stdbool.h>
#include "quard_mbox_router_consumer.h"

// shell
Shell shell;
static char shell_buffer[512];

// background shell agent queue
#define ITEM_SIZE sizeof (uint8_t)
QueueHandle_t shell_rx_queue;
#define QUEUE_LENGTH 128
static StaticQueue_t ShellStaticQueue;
uint8_t ShellQueueStorageArea[ QUEUE_LENGTH * ITEM_SIZE ];

// background shell agent
#define SHELL_AGENT_STACK_SIZE      256
#define SHELL_AGENT_QUEUE_MSG_NUM	10
#define MBOX_CONS_TASK_PRI			4
#define FULL_PAYLOAD_SIZE           (USER_PARAM_NUM * 4)

// shell task
#define SHELL_STACK_SIZE    (512)
static StaticTask_t xShellTaskTCB;
static StackType_t uxShellTaskStack[SHELL_STACK_SIZE];
#define SHELL_TASK_PRI		4

/**
 * @brief uart中断调用，发送数据到shell_rx_queue
 * 
 */
#if defined(UART_SHELL)  || defined(UART_REMOTE_SHELL)
void send_shell_queue(char c)
{
	/* The RISC-V trap epilogue restores the selected task context. */
	xQueueSendToBackFromISR(shell_rx_queue, &c, NULL);
}
#endif

/**
 * @brief 从shell_rx_queue读取数据
 * 
 */
uint32_t shell_read_bytes(uint8_t *buf, uint32_t length)
{
    uint32_t i = 0;
    for (i = 0; i < length; i++) {
        xQueueReceive(shell_rx_queue, &buf[i], (portTickType) portMAX_DELAY);
    }
    return i;
}

struct quard_shell_agent {
    struct quard_mbox_consumer *shell_mbox;
    StackType_t stack_buf[SHELL_AGENT_STACK_SIZE];
    struct quard_consumer_msg rx_queue_buf[SHELL_AGENT_QUEUE_MSG_NUM];
};

static struct quard_shell_agent shell_agent;
static struct quard_mbox_consumer shell_mbox;
bool remote_shell_init_done = false;

/**
 * @brief router接收回调函数
 * 
 */
void shell_agent_recv_cb(struct quard_consumer_msg *msg)
{
    if(!remote_shell_init_done)
        remote_shell_init_done = true;
    if (msg != NULL) {
        uint8_t validBytesCount = msg->cmd;

        if(validBytesCount > USER_PARAM_NUM * sizeof(uint32_t)) {
            validBytesCount = USER_PARAM_NUM * sizeof(uint32_t);
        }

        for(uint8_t i = 0; i < validBytesCount; ++i) {
            char character = ((char *)msg->params)[i];

            if (xQueueSend(shell_rx_queue, &character, 0) != pdTRUE) {
                LOG_E("Failed to send queue from shell_agent to shell_rx_queue\r\n");
            }
        }
    }
}

/**
 * @brief consumer初始化
 * 
 */
static void remote_shell_agent(void)
{
    struct quard_mbox_consumer *consumer = &shell_mbox;
    struct quard_mbox_consumer_cfg *cfg = &consumer->cfg;

    /* Fullfill mbox consumer config info */
    cfg->server_id = QUARD_SERVER_ID_VSHELL;
    cfg->thread_stack = shell_agent.stack_buf;
    cfg->stack_depth = SHELL_AGENT_STACK_SIZE;
    cfg->thread_priority = MBOX_CONS_TASK_PRI;
    cfg->recv_cb = shell_agent_recv_cb;
    cfg->rx_queue_item_num = SHELL_AGENT_QUEUE_MSG_NUM;
    cfg->rx_queue_buf = shell_agent.rx_queue_buf;

    quard_mbox_consumer_register(consumer);
}

/**
 * @brief remote shell发送数据
 * 
 */
short remote_shell_write(char *data, unsigned short len)
{
    struct quard_consumer_msg msg;

	unsigned short left_size = len;
	uint32_t data_idx = 0;
    if(len == 0) {
        return 0;
    }

    if(!remote_shell_init_done) {
        return 0;
    }

	while(left_size > (FULL_PAYLOAD_SIZE)) {
		memset(&msg, 0, sizeof(struct quard_consumer_msg));
		/* Set buffer size */
		msg.cmd = FULL_PAYLOAD_SIZE;
		memcpy(&msg.params[0], &data[data_idx], msg.cmd);
		quard_router_send_msg(&shell_mbox, &msg);
		left_size -= FULL_PAYLOAD_SIZE;
		data_idx += FULL_PAYLOAD_SIZE;
	}

	memset(&msg, 0, sizeof(struct quard_consumer_msg));
	msg.cmd = left_size;
	memcpy(&msg.params[0], &data[data_idx], msg.cmd);
	quard_router_send_msg_shell(&shell_mbox, &msg);

    return len;
}

/**
 * @brief 用户shell写：用于 Shell 回显和输出
 * 
 * @param data 数据
 * @param len 数据长度
 * 
 * @return short 实际写入的数据长度
 */
signed short userShellWrite(char *data, unsigned short len) {
    // return uartputs_sync(data, len);
#ifdef UART_SHELL
    uart_puts(data, len);
#elif defined(REMOTE_SHELL)
    remote_shell_write(data, len);
#elif defined(UART_REMOTE_SHELL)
    uart_puts(data, len);
    remote_shell_write(data, len);
#endif

    return len;
}

/**
 * @brief 用户shell读：适配 shellScan 等交互功能
 * 
 * @param data 数据
 * @param len 数据长度
 * 
 * @return short 实际读取到
 */ 
signed short userShellRead(char *data, unsigned short len) {
#ifdef UART_SHELL
    return uartgets_sync(data, len);
#elif defined(REMOTE_SHELL) || defined(UART_REMOTE_SHELL)
    return shell_read_bytes((uint8_t *)data, len);
#endif
}

/**
 * @brief 用户shell初始化
 * 
 */
void userShellInit(void) {
    shell_rx_queue = xQueueCreateStatic( QUEUE_LENGTH, ITEM_SIZE, ShellQueueStorageArea, &ShellStaticQueue );
    if (shell_rx_queue == NULL) {
        LOG_E("shell rx queue create failed");
        return;
    }

#if defined(REMOTE_SHELL) || defined(UART_REMOTE_SHELL)
    remote_shell_agent();
#endif

    shell.write = userShellWrite;
    shell.read = userShellRead;

    if (xTaskCreateStatic( shellTask,
                           "shell",
                           SHELL_STACK_SIZE,
                           &shell,
                           SHELL_TASK_PRI,
                           &(uxShellTaskStack[0]),
                           &xShellTaskTCB) == NULL) {
        LOG_D("shell task creat failed");
    }

    shellInit(&shell, shell_buffer, sizeof(shell_buffer));
}
