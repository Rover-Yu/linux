/*
 * Generic UP xchg and cmpxchg using interrupt disablement.  Does not
 * support SMP.
 */

#ifndef __ASM_LKL_CMPXCHG_H
#define __ASM_LKL_CMPXCHG_H

#include <linux/types.h>
#include <linux/irqflags.h>

#ifndef xchg

/*
 * This function doesn't exist, so you'll get a linker error if
 * something tries to do an invalidly-sized xchg().
 */
extern void __xchg_called_with_bad_pointer(void);

static inline
unsigned long __xchg(unsigned long x, volatile void *ptr, int size)
{
	return x;	//FIXME
}

#define xchg(ptr, x) ({							\
	((__typeof__(*(ptr)))						\
		__xchg((unsigned long)(x), (ptr), sizeof(*(ptr))));	\
})

#endif /* xchg */

/*
 * Atomic compare and exchange.
 */
#include <asm-generic/cmpxchg-local.h>

#ifndef cmpxchg_local
#define cmpxchg_local(ptr, o, n) ({					       \
	((__typeof__(*(ptr)))__cmpxchg_local_generic((ptr), (unsigned long)(o),\
			(unsigned long)(n), sizeof(*(ptr))));		       \
})
#endif

#ifndef cmpxchg64_local
#define cmpxchg64_local(ptr, o, n) __cmpxchg64_local_generic((ptr), (o), (n))
#endif

#define cmpxchg(ptr, o, n)	cmpxchg_local((ptr), (o), (n))
#define cmpxchg64(ptr, o, n)	cmpxchg64_local((ptr), (o), (n))

#endif /* __ASM_LKL_CMPXCHG_H */
