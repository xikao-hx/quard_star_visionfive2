#define LOG_TAG     "LOG_BUF"
#define LOG_LVL     ELOG_LVL_VERBOSE
#include "elog.h"
#include <stddef.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "timers.h"
#include "ringbuffer.h"
#include "buffer_log_internal.h"
#include "quard_mbox_router_consumer.h"
#include "quard_soc_log.h"
#include "uart.h"

/* TODO: 后面优化任务优先安排，并从配置文件中读取 */
#define LOG_FLUSH_TASK_PRIORITY   4
#define MBOX_CONS_TASK_PRI        4

/* log flush task */
#define LOG_FLUSH_TASK_STACK_SIZE   (256)
static TaskHandle_t log_flush_task_handle;
static StaticTask_t log_flush_task_buf;
static StackType_t log_flush_stack[LOG_FLUSH_TASK_STACK_SIZE];

static SemaphoreHandle_t flush_mutex;
static StaticSemaphore_t flush_mutex_buf;

/* log timer to trigger log flush task */
#define FLUSH_QUEUE_SIZE    (3U)
static QueueHandle_t flush_queue;
static StaticQueue_t flush_queue_buf;
static uint8_t flush_queue_storage[FLUSH_QUEUE_SIZE * sizeof(uint8_t)];

/* soc log agent task */
#define RB_SIZE		(MCU_LOG_BUF_SIZE - sizeof(slave_buffer_cb))
#define SHELL_AGENT_STACK_SIZE			0x200
#define SHELL_AGENT_QUEUE_MSG_NUM		10

struct soc_log_agent_struct {
    struct quard_mbox_consumer *mbox;
    StackType_t stack_buf[SHELL_AGENT_STACK_SIZE];
    struct quard_consumer_msg rx_queue_buf[SHELL_AGENT_QUEUE_MSG_NUM];
};

static struct soc_log_agent_struct log_agent;
static struct quard_mbox_consumer mbox;

/* log timer */
static TimerHandle_t log_timer;
static StaticTimer_t log_timer_buf;

/* log buffer */
static __attribute__((aligned(64), __section__("DTCM_SECTION"))) uint8_t master_buf[MCU_LOG_BUF_SIZE];
static slave_buffer_cb *mst_cb;

void log_rb_backend_output(const char *log, uint32_t len)
{
    uint32_t put_size = 0;

    /* The shared-memory backend is optional until its PMP region is enabled. */
    if (flush_mutex == NULL || mst_cb == NULL) {
        return;
    }

    if (xSemaphoreTakeRecursive(flush_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    while (len > put_size) {
        uint32_t cur_size;
        cur_size = rb_output_internal((slave_buffer_cb *)master_buf, sizeof(master_buf), (log + put_size), (len - put_size));
        if (cur_size == 0) { 
            break;
        }
        put_size += cur_size;
        if ((cur_size == 0) && (quard_ringbuffer_status((struct quard_ringbuffer *)&(mst_cb->rb_ctrl)) == QUARD_RINGBUFFER_FULL))
            break;
    }
    xSemaphoreGiveRecursive(flush_mutex);
}

void rb_log_output(const char *log, uint32_t len)
{
    uint32_t put_size = 0;
    while (len > put_size) {
        uint32_t cur_size;
        cur_size = rb_output_internal((slave_buffer_cb *)master_buf, sizeof(master_buf), (log + put_size), (len - put_size));
        put_size += cur_size;
        if ((cur_size == 0) && (quard_ringbuffer_status((struct quard_ringbuffer *)&(mst_cb->rb_ctrl)) == QUARD_RINGBUFFER_FULL))
            break;
    }
}

static void log_agent_recv_cb(struct quard_consumer_msg *msg)
{
    if (msg != NULL) {
        uint8_t validBytesCount = (uint8_t)msg->cmd;

        if(validBytesCount > USER_PARAM_NUM * sizeof(uint32_t)) {
            validBytesCount = (uint8_t)(USER_PARAM_NUM * sizeof(uint32_t));
        }

        if((msg->cmd == 0x4) && (msg->params[0] == 0x1234ABCD)) {
            LOG_D("Received log flush cmd\r\n");
            log_flush_enable = true;
            flush_log(true, (slave_buffer_cb *)master_buf, sizeof(master_buf));
        } else {
            LOG_E("Wrong flush cmd: \r\n");
            LOG_E("msg cnt: %d\r\n", msg->cmd);
            LOG_E("params[0-5]: 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x\r\n", 
                 msg->params[0], msg->params[1], msg->params[2], msg->params[3], 
                 msg->params[4], msg->params[5]);
        }
    }
}

void soc_log_agent(void)
{
    struct quard_mbox_consumer *consumer = &mbox;
    struct quard_mbox_consumer_cfg *cfg = &consumer->cfg;

    /* Fullfill mbox consumer config info */
    cfg->server_id = (uint8_t)QUARD_SERVER_ID_LOG;
    cfg->thread_stack = log_agent.stack_buf;
    cfg->stack_depth = SHELL_AGENT_STACK_SIZE;
    cfg->thread_priority = (UBaseType_t)MBOX_CONS_TASK_PRI;
    cfg->recv_cb = log_agent_recv_cb;
    cfg->rx_queue_item_num = SHELL_AGENT_QUEUE_MSG_NUM;
    cfg->rx_queue_buf = log_agent.rx_queue_buf;

    (void)quard_mbox_consumer_register(consumer);
}

static void log_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    uint8_t msg = 1;
    xQueueSend(flush_queue, &msg, 0);
}

static void log_flush_task(void *pvParameters)
{
    (void)pvParameters;
    uint8_t msg;
    while (1) {
        if (xQueueReceive(flush_queue, &msg, portMAX_DELAY) == pdTRUE) {
            if((log_flush_enable == false) && (LOG_BUF_BASE->buffer_state == SLAVE_BUFFER_EMPTY)) {
                log_flush_enable = true;
            }
            if(!log_flush_enable)
                continue;
            if (xSemaphoreTakeRecursive(flush_mutex, portMAX_DELAY) == pdTRUE) {
#if LOG_RB_FORCE_PUT
                flush_log(true, (slave_buffer_cb *)master_buf, sizeof(master_buf));
#else
                if(LOG_BUF_BASE->buffer_state == SLAVE_BUFFER_EMPTY) {
                    flush_log(false, (slave_buffer_cb *)master_buf, sizeof(master_buf));
                }
#endif
                xSemaphoreGiveRecursive(flush_mutex);
            }
        }
    }
}

int log_rb_backend_init(void)
{
    mst_cb = (slave_buffer_cb *)(&master_buf[0]);
    mst_cb->head_magic = 0xF1F11F1F;
    mst_cb->middle_magic = 0xF1F11F1F;

    quard_ringbuffer_init((struct quard_ringbuffer *)&(mst_cb->rb_ctrl), (uint8_t *)&(mst_cb->buffer_first_byte), (int32_t)RB_SIZE);

    flush_mutex = xSemaphoreCreateRecursiveMutexStatic(&flush_mutex_buf);
    if (flush_mutex == NULL) {
        LOG_E("Failed to create flush mutex.\r\n");
        return -1;
    }

    flush_queue = xQueueCreateStatic(FLUSH_QUEUE_SIZE, sizeof(uint8_t), flush_queue_storage, &flush_queue_buf);
    if (flush_queue == NULL) {
        LOG_E("Failed to create flush queue.\r\n");
        return -1;
    }

    log_flush_task_handle = xTaskCreateStatic(log_flush_task, "log_flush", LOG_FLUSH_TASK_STACK_SIZE, NULL,
                                              (UBaseType_t)LOG_FLUSH_TASK_PRIORITY, log_flush_stack, &log_flush_task_buf);
    if (log_flush_task_handle == NULL) {
        LOG_E("Failed to create log_flush task.\r\n");
        return -1;
    }

    log_timer = xTimerCreateStatic("log_timer", pdMS_TO_TICKS(1000), pdTRUE, NULL,
        log_timer_callback, &log_timer_buf);
    if (log_timer == NULL) {
        LOG_E("Failed to create log_timer.\r\n");
    } else {
        if ( xTimerStart(log_timer, 0) != pdPASS ) {
            LOG_E("Failed to start log_timer.\r\n");
        }
    }

    return 0;
}

void ulog_enable_timer(void)
{
    if (xTimerStart(log_timer, 0) != pdPASS ) {
        LOG_E("Failed to start log_flush timer.\r\n");
    }
    return;
}

void ulog_disable_timer(void)
{
    if (xTimerStop(log_timer, 0) != pdPASS) {
        // Handle error in stopping the timer
        LOG_E("%s Timer stop failed!\n", __func__);
    }
    return;
}
