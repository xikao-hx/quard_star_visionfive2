#ifndef PLIC_H_
#define PLIC_H_

#include <stdint.h>

#include <stdint.h>

// plic.c
void plicinit(void);
void plicinithart(void);
int plic_claim(void);
void plic_complete(int irq);
void plic_debug_snapshot(uint32_t *pending, uint32_t *enable,
                         uint32_t *threshold);

#endif
