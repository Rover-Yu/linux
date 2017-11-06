#ifndef _ASM_LKL_BARRIER_H
#define _ASM_LKL_BARRIER_H

#ifdef CONFIG_LKL_X86_64
#include <asm/x86/barrier.h>
#elif defined(CONFIG_LKL_ARM_64)
#include <asm/arm/barrier.h>
#else
#error "oops, barrier impl is missed."
#endif

#endif /* _ASM_LKL_BARRIER_H */
