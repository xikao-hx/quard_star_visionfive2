#include <stdint.h>
#include <stdbool.h>
#include "sbi.h"
#include "riscv_asm.h"
#include <stdint.h>
#include "plic.h"
#include "memlayout.h"
#include <stdio.h>
#include <FreeRTOS.h>
#include <task.h>
#include "ns16550.h"
#include "irq.h"
#define LOG_TAG     "TRAP"
#define LOG_LVL     ELOG_LVL_VERBOSE
#include "elog.h"

static const char *Exception_Name[] = 
{
    "Instruction Address Misaligned",
    "Instruction Access Fault",
    "Illegal Instruction",
    "Breakpoint",
    "Load Address Misaligned",
    "Load Access Fault",
    "Store/AMO Address Misaligned",
    "Store/AMO Access Fault",
    "Environment call from U-mode",
    "Environment call from S-mode",
    "Reserved-10",
    "Reserved-11",
    "Instruction Page Fault",
    "Load Page Fault",
    "Reserved-14",
    "Store/AMO Page Fault"
};

static const char *Interrupt_Name[] = 
{
    "User Software Interrupt",
    "Supervisor Software Interrupt",
    "Reversed-2",
    "Reversed-3",
    "User Timer Interrupt",
    "Supervisor Timer Interrupt",
    "Reversed-6",
    "Reversed-7",
    "User External Interrupt",
    "Supervisor External Interrupt",
    "Reserved-10",
    "Reserved-11",
};

struct stack_frame
{
    uint64_t sepc;
    uint64_t ra;
    uint64_t t0;
    uint64_t t1;
    uint64_t t2;
    uint64_t s0_fp;
    uint64_t s1;
    uint64_t a0;
    uint64_t a1;
    uint64_t a2;
    uint64_t a3;
    uint64_t a4;
    uint64_t a5;
    uint64_t a6;
    uint64_t a7;
    uint64_t s2;
    uint64_t s3;
    uint64_t s4;
    uint64_t s5;
    uint64_t s6;
    uint64_t s7;
    uint64_t s8;
    uint64_t s9;
    uint64_t s10;
    uint64_t s11;
    uint64_t t3;
    uint64_t t4;
    uint64_t t5;
    uint64_t t6;
    uint64_t sstatus;
};

void dump_regs(struct stack_frame *regs)
{
    LOG_I("--------------Dump Registers-----------------\n");
    LOG_I("Function Registers:\n");
    LOG_I("\tra(x1) = 0x%x\tuser_sp = 0x%x\n",regs -> ra);
    LOG_I("Temporary Registers:\n");
    LOG_I("\tt0(x5) = 0x%x\tt1(x6) = 0x%x\n",regs -> t0,regs -> t1);
    LOG_I("\tt2(x7) = 0x%x\n",regs -> t2);
    LOG_I("\tt3(x28) = 0x%x\tt4(x29) = 0x%x\n",regs -> t3,regs -> t4);
    LOG_I("\tt5(x30) = 0x%x\tt6(x31) = 0x%x\n",regs -> t5,regs -> t6);
    LOG_I("Saved Registers:\n");
    LOG_I("\ts0/fp(x8) = 0x%x\ts1(x9) = 0x%x\n",regs -> s0_fp,regs -> s1);
    LOG_I("\ts2(x18) = 0x%x\ts3(x19) = 0x%x\n",regs -> s2,regs -> s3);
    LOG_I("\ts4(x20) = 0x%x\ts5(x21) = 0x%x\n",regs -> s4,regs -> s5);
    LOG_I("\ts6(x22) = 0x%x\ts7(x23) = 0x%x\n",regs -> s6,regs -> s7);
    LOG_I("\ts8(x24) = 0x%x\ts9(x25) = 0x%x\n",regs -> s8,regs -> s9);
    LOG_I("\ts10(x26) = 0x%x\ts11(x27) = 0x%x\n",regs -> s10,regs -> s11);
    LOG_I("Function Arguments Registers:\n");
    LOG_I("\ta0(x10) = 0x%x\ta1(x11) = 0x%x\n",regs -> a0,regs -> a1);
    LOG_I("\ta2(x12) = 0x%x\ta3(x13) = 0x%x\n",regs -> a2,regs -> a3);
    LOG_I("\ta4(x14) = 0x%x\ta5(x15) = 0x%x\n",regs -> a4,regs -> a5);
    LOG_I("\ta6(x16) = 0x%x\ta7(x17) = 0x%x\n",regs -> a6,regs -> a7);
    LOG_I("sstatus = 0x%x\n",regs -> sstatus);
    LOG_I("\t%s\n",(regs -> sstatus & SSTATUS_SIE) ? "Supervisor Interrupt Enabled" : "Supervisor Interrupt Disabled");
    LOG_I("\t%s\n",(regs -> sstatus & SSTATUS_SPIE) ? "Last Time Supervisor Interrupt Enabled" : "Last Time Supervisor Interrupt Disabled");
    LOG_I("\t%s\n",(regs -> sstatus & SSTATUS_SPP) ? "Last Privilege is Supervisor Mode" : "Last Privilege is User Mode");
    LOG_I("\t%s\n",(regs -> sstatus & (1 << 19)) ? "Permit to Read Executable-only Page" : "Not Permit to Read Executable-only Page");
    LOG_I("-----------------Dump OK---------------------\n");
}

void handle_trap(uint64_t scause,uint64_t sepc,uint64_t stval, struct stack_frame *sp)
{
    int id = scause&0x1ff;
    const char *msg;

    if(scause >> 63)
    {
        if(id < sizeof(Interrupt_Name) / sizeof(const char *))
        {
            msg = Interrupt_Name[id];
        }
        else
        {
            msg = "Unknown Interrupt";
        }

        LOG_I("Unhandled Interrupt %d:%s\n",id,msg);
    }
    else
    {
        if(id < sizeof(Exception_Name) / sizeof(const char *))
        {
            msg = Exception_Name[id];
        }
        else
        {
            msg = "Unknown Exception";
        }

        LOG_I("Unhandled Exception %d:%s\n",id,msg);
    }

    LOG_I("scause:0x%x,stval:0x%x,sepc:0x%x\n",scause,stval,sepc);
    dump_regs(sp);
    while(1);
}

// volatile uint32_t g_uart_irq_count = 0;
// volatile uint32_t g_last_irq = 0;
// g_last_irq = irq;
// g_uart_irq_count++;

struct irq_desc {
    isr_handler_t handler;
    void *param;
    const char *name;
};

#define MAX_IRQ_COUNT 64
static struct irq_desc irq_table[MAX_IRQ_COUNT];
static volatile unsigned int isr_nesting;

void hw_interrupt_install(int irq, isr_handler_t handler, void *param, const char *name)
{
    if (irq < 0 || irq >= MAX_IRQ_COUNT) return;

    taskENTER_CRITICAL(); 

    irq_table[irq].handler = handler;
    irq_table[irq].param = param;
    irq_table[irq].name = name;

    taskEXIT_CRITICAL();
}

bool irq_is_in_isr(void)
{
    return isr_nesting != 0;
}

void handle_interrupt(void)
{
    int irq = plic_claim();

    /* A zero claim means that no interrupt is pending for this context. */
    if (irq == 0) {
        return;
    }

    // LOG_D("irq: %d\n",irq);
    
    if (irq < MAX_IRQ_COUNT) {
        if (irq_table[irq].handler != NULL) {
            irq_table[irq].handler(
                irq, irq_table[irq].param);
        }
    }

    /* Complete the exact source returned by claim, even if it is unhandled. */
    plic_complete(irq);
    isr_nesting--;
}

/*
static volatile unsigned int g_uart_irq_count;
static volatile int g_last_irq;
g_last_irq = irq;
g_uart_irq_count++;
isr_nesting++;
void uart_task(void *pvParameters) {
    int count = 0;
    char msg[128];
    
    for (;;) {
        snprintf(msg, sizeof(msg), 
                 "Hello from FreeRTOS! Count: %d, PLIC IRQs: %u, Last IRQ: %d\r\n", 
                 count++, g_uart_irq_count, g_last_irq);
        
        LOG_I(msg);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
*/
