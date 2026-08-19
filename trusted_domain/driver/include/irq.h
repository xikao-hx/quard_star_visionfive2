#ifndef __IRQ_H__
#define __IRQ_H__

#include <stdbool.h>

typedef void (*isr_handler_t)(int irq, void *param);
void hw_interrupt_install(int irq, isr_handler_t handler, void *param, const char *name);
bool irq_is_in_isr(void);

#endif /* __IRQ_H__ */
