#ifndef __ASM_LKL_ALTERNATIVE_H
#define __ASM_LKL_ALTERNATIVE_H

#ifdef CONFIG_LKL_X86_64
#include <asm/x86/alternative.h>
#elif defined(CONFIG_LKL_ARM_64)
#include <asm/arm/alternative.h>
#else
#error "oops, bitops impl is missed."
#endif

#endif /* __ASM_LKL_ALTERNATIVE_H */
