#include <stdint.h>

#include "jh7110.h"

#define SYS_CRG_BASE       0x13020000UL
#define UART2_APB_OFFSET   0x254U
#define UART2_CORE_OFFSET  0x258U
#define SYS_CRG_RESET2     0x300U

static inline volatile uint32_t *jh7110_reg(uintptr_t address)
{
    return (volatile uint32_t *)address;
}

void jh7110_uart_clock_init(void)
{
    /* Match the 6.6 AMP SDK RT-Thread board init for UART2. */
    *jh7110_reg(SYS_CRG_BASE + UART2_APB_OFFSET) |= (1U << 31);
    *jh7110_reg(SYS_CRG_BASE + UART2_CORE_OFFSET) |= (1U << 31);
    *jh7110_reg(SYS_CRG_BASE + SYS_CRG_RESET2) &= ~((1U << 23) | (1U << 24));
}
