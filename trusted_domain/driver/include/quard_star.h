#ifndef QUARD_STAR_H_
#define QUARD_STAR_H_

#include "riscv-reg.h"

#ifdef __ASSEMBLER__
#define CONS(NUM, TYPE)NUM
#else
#define CONS(NUM, TYPE)NUM##TYPE
#endif /* __ASSEMBLER__ */

#ifdef BOARD_JH7110
#define PRIM_HART			4
#define NS16550_ADDR		CONS(0x10020000, UL)
#else
#define PRIM_HART			7
#define NS16550_ADDR		CONS(0x10002000, UL)
#endif

#ifndef __ASSEMBLER__

#endif /* __ASSEMBLER__ */

#endif /* QUARD_STAR_H_ */
