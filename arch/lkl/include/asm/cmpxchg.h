#ifndef ASM_LKL_CMPXCHG_H
#define ASM_LKL_CMPXCHG_H

#ifdef CONFIG_LKL_X86_64
#include <asm/x86/cmpxchg.h>
#elif defined(CONFIG_LKL_ARM_64)
#include <asm/arm/cmpxchg.h>
#else
#error "oops, cmpxchg impl is missed."
#endif

#endif	/* ASM_LKL_CMPXCHG_H */
