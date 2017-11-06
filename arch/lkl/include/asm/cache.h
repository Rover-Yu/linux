#ifndef __ASM_LKL_CACHE_H
#define __ASM_LKL_CACHE_H

#define L1_CACHE_SHIFT		6
#define L1_CACHE_BYTES		(1 << L1_CACHE_SHIFT)

#define __lock_aligned		__attribute__((aligned))

#endif /* __ASM_LKL_CACHE_H */
