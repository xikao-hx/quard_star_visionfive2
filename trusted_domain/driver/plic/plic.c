#include <stdint.h>
#include "memlayout.h"
#include "riscv.h"
//
// the riscv Platform Level Interrupt Controller (PLIC).
//

void
plicinit(void)
{
  // set desired IRQ priorities non-zero (otherwise disabled).
  *(uint32_t*)(PLIC + UART2_IRQ*4) = 1;
#if MAILBOX0_IRQ != 0
  *(uint32_t*)(PLIC + MAILBOX0_IRQ*4) = 1;
#endif
}

void
plicinithart(void)
{
  int hart = cpuid();

  // Enable UART2 in the PLIC word containing its source ID.
  *(uint32_t*)(PLIC_SENABLE(hart) + (UART2_IRQ / 32) * 4) |=
      (1U << (UART2_IRQ % 32));
#if MAILBOX0_IRQ != 0
  *(uint32_t*)(PLIC_SENABLE(hart) + (MAILBOX0_IRQ / 32) * 4) |=
      (1U << (MAILBOX0_IRQ % 32));
#endif
  
  // set this hart's S-mode priority threshold to 0.
  *(uint32_t*)PLIC_SPRIORITY(hart) = 0;

  // RT-Thread opens SEIP when unmasking a PLIC source. Keep the same
  // per-hart S-mode external-interrupt enable sequence here.
  w_sie(r_sie() | SIE_SEIE);
}

// ask the PLIC what interrupt we should serve.
int
plic_claim(void)
{
  int hart = cpuid();
  int irq = *(uint32_t*)PLIC_SCLAIM(hart);
  return irq;
}

// tell the PLIC we've served this IRQ.
void
plic_complete(int irq)
{
  int hart = cpuid();
  *(uint32_t*)PLIC_SCLAIM(hart) = irq;
}

/* Read-only PLIC state for diagnostics. Do not read SCLAIM here: reading it
 * claims an interrupt and would change the behavior being debugged. */
void
plic_debug_snapshot(uint32_t *pending, uint32_t *enable, uint32_t *threshold)
{
  int hart = cpuid();

  if (pending)
    *pending = *(uint32_t *)(PLIC_PENDING + (MAILBOX0_IRQ / 32) * 4);
  if (enable)
    *enable = *(uint32_t *)(PLIC_SENABLE(hart) +
                            (MAILBOX0_IRQ / 32) * 4);
  if (threshold)
    *threshold = *(uint32_t *)PLIC_SPRIORITY(hart);
}
