// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0 
// 10001000 -- virtio disk 
// 80000000 -- boot ROM jumps here in machine mode
//             -kernel loads the kernel here
// unused RAM after 80000000.

// the kernel uses physical memory thus:
// 80000000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel

// qemu puts UART registers here in physical memory.
#ifdef BOARD_JH7110
#define UART2 0x10020000L
#define UART2_IRQ 34
#define MAILBOX0 0x13060000L
#define MAILBOX0_IRQ 26
#else
#define UART2 0x10002000L
#define UART2_IRQ 12

// qemu puts MAILBOX registers here in physical memory.
#define MAILBOX0 0x10004000L
#define MAILBOX0_IRQ 15
#endif

// local interrupt controller, which contains the timer.
#define CLINT 0x2000000L
#define CLINT_ADDR CLINT
#define CLINT_MTIMECMP(hartid) (CLINT + 0x4000 + 8*(hartid))
#define CLINT_MTIME (CLINT + 0xBFF8) // cycles since boot.

// qemu puts programmable interrupt controller here.
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_MENABLE(hart) (PLIC + 0x2000 + (hart)*0x100)
#ifdef BOARD_JH7110
/* JH7110 has no S-mode context for hart0; hart4 S-mode uses context 8. */
#define PLIC_CONTEXT(hart) ((hart) << 1)
#define PLIC_SENABLE(hart) (PLIC + 0x2000 + PLIC_CONTEXT(hart) * 0x80)
#define PLIC_SPRIORITY(hart) (PLIC + 0x200000 + PLIC_CONTEXT(hart) * 0x1000)
#define PLIC_SCLAIM(hart) (PLIC + 0x200004 + PLIC_CONTEXT(hart) * 0x1000)
#else
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_MPRIORITY(hart) (PLIC + 0x200000 + (hart)*0x2000)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_MCLAIM(hart) (PLIC + 0x200004 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)
#endif
