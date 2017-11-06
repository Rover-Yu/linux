#ifndef _ASM_LKL_BITOPS_H
#define _ASM_LKL_BITOPS_H

#ifndef _LINUX_BITOPS_H
#error only <linux/bitops.h> can be included directly
#endif

#ifdef CONFIG_LKL_X86_64
#include <asm/x86/bitops.h>
#elif defined(CONFIG_LKL_ARM_64)
#include <asm/arm/bitops.h>
#else
#error "oops, bitops impl is missed."
#endif

#endif /* _ASM_LKL_BITOPS_H */
