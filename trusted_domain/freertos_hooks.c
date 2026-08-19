#include <FreeRTOS.h>
#include <task.h>

extern void serial_print(const char *fmt, ...);

/* 空闲任务的内存控制块和栈空间 */
static StaticTask_t xIdleTaskTCB;
static StackType_t uxIdleTaskStack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *pulIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

/* 定时器任务的内存控制块和栈空间 */
static StaticTask_t xTimerTaskTCB;
static StackType_t uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH];

void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
                                    StackType_t **ppxTimerTaskStackBuffer,
                                    uint32_t *pulTimerTaskStackSize)
{
    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *task_name)
{
    (void)task;
    serial_print("FreeRTOS stack overflow: %s\r\n", task_name);
    for (;;)
        ;
}

void vApplicationMallocFailedHook(void)
{
    serial_print("FreeRTOS malloc failed\r\n");
    for (;;)
        ;
}
