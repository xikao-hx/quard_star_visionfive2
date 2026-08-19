// riscv.h
#ifndef __RISCV_H__
#define __RISCV_H__

#include <stdint.h>

// ====================== 通用寄存器操作 ======================
static inline uint64_t r_tp(void)
{
    uint64_t x;
    asm volatile("mv %0, tp" : "=r" (x));
    return x;
}

static inline void w_tp(uint64_t x)
{
    asm volatile("mv tp, %0" : : "r" (x));
}

static inline uint64_t r_sp(void)
{
    uint64_t x;
    asm volatile("mv %0, sp" : "=r" (x));
    return x;
}

static inline uint64_t r_fp(void)
{
    uint64_t x;
    asm volatile("mv %0, s0" : "=r" (x));
    return x;
}

static inline uint64_t r_ra(void)
{
    uint64_t x;
    asm volatile("mv %0, ra" : "=r" (x));
    return x;
}

static inline uint64_t r_time(void)
{
    uint64_t x;
    asm volatile("csrr %0, time" : "=r" (x));
    return x;
}

// ====================== S 模式 CSR 寄存器操作 ======================

// 1. S 模式状态寄存器 sstatus
#ifndef SSTATUS_SPP  
#define SSTATUS_SPP (1L << 8)   // 先前模式：1=S模式，0=U模式
#endif

#ifndef SSTATUS_SPIE
#define SSTATUS_SPIE (1L << 5)  // S 模式先前中断使能
#endif

#ifndef SSTATUS_SIE
#define SSTATUS_SIE (1L << 1)   // S 模式中断使能
#endif

static inline uint64_t r_sstatus(void)
{
    uint64_t x;
    asm volatile("csrr %0, sstatus" : "=r" (x));
    return x;
}

static inline void w_sstatus(uint64_t x)
{
    asm volatile("csrw sstatus, %0" : : "r" (x));
}

// 2. S 模式中断使能寄存器 sie
#ifndef SIE_SEIE
#define SIE_SEIE (1L << 9)      // 外部中断使能
#endif

#ifndef SIE_STIE
#define SIE_STIE (1L << 5)      // 定时器中断使能
#endif

#ifndef SIE_SSIE
#define SIE_SSIE (1L << 1)      // 软件中断使能
#endif

static inline uint64_t r_sie(void)
{
    uint64_t x;
    asm volatile("csrr %0, sie" : "=r" (x));
    return x;
}

static inline void w_sie(uint64_t x)
{
    asm volatile("csrw sie, %0" : : "r" (x));
}

// 3. S 模式中断挂起寄存器 sip
#ifndef SIP_SEIP
#define SIP_SEIP (1L << 9)      // 外部中断挂起
#endif

#ifndef SIP_STIP
#define SIP_STIP (1L << 5)      // 定时器中断挂起
#endif

#ifndef SIP_SSIP
#define SIP_SSIP (1L << 1)      // 软件中断挂起
#endif

static inline uint64_t r_sip(void)
{
    uint64_t x;
    asm volatile("csrr %0, sip" : "=r" (x));
    return x;
}

static inline void w_sip(uint64_t x)
{
    asm volatile("csrw sip, %0" : : "r" (x));
}

// 4. S 模式陷阱向量基址寄存器 stvec
static inline void w_stvec(uint64_t x)
{
    asm volatile("csrw stvec, %0" : : "r" (x));
}

static inline uint64_t r_stvec(void)
{
    uint64_t x;
    asm volatile("csrr %0, stvec" : "=r" (x));
    return x;
}

// 5. S 模式异常程序计数器 sepc
static inline void w_sepc(uint64_t x)
{
    asm volatile("csrw sepc, %0" : : "r" (x));
}

static inline uint64_t r_sepc(void)
{
    uint64_t x;
    asm volatile("csrr %0, sepc" : "=r" (x));
    return x;
}

// 6. S 模式陷阱原因寄存器 scause
static inline uint64_t r_scause(void)
{
    uint64_t x;
    asm volatile("csrr %0, scause" : "=r" (x));
    return x;
}

// 7. S 模式陷阱值寄存器 stval
static inline uint64_t r_stval(void)
{
    uint64_t x;
    asm volatile("csrr %0, stval" : "=r" (x));
    return x;
}

// 8. S 模式临时寄存器 sscratch
static inline void w_sscratch(uint64_t x)
{
    asm volatile("csrw sscratch, %0" : : "r" (x));
}

static inline uint64_t r_sscratch(void)
{
    uint64_t x;
    asm volatile("csrr %0, sscratch" : "=r" (x));
    return x;
}

// ====================== 中断控制函数 ======================
static inline void intr_on(void)
{
    w_sstatus(r_sstatus() | SSTATUS_SIE);
}

static inline void intr_off(void)
{
    w_sstatus(r_sstatus() & ~SSTATUS_SIE);
}

static inline int intr_get(void)
{
    return (r_sstatus() & SSTATUS_SIE) != 0;
}

static inline void enable_external_interrupt(void)
{
    w_sie(r_sie() | SIE_SEIE);
}

static inline int cpuid(void)
{
    return (int)r_tp();
}

#endif // __RISCV_H__