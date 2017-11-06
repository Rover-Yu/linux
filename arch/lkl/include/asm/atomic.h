#ifndef ASM_LKL_ATOMIC_H
#define ASM_LKL_ATOMIC_H

#ifdef CONFIG_LKL_X86_64
#include <asm/x86/atomic.h>
#elif defined(CONFIG_LKL_ARM_64)
#include <asm/arm/atomic.h>
#else
#error "oops, atomic impl is missed."
#endif

#endif	/* ASM_LKL_ATOMIC_H */
