#ifndef __ASM_LKL_TLBFLUSH_H
#define __ASM_LKL_TLBFLUSH_H

#ifndef CONFIG_MMU

struct mm_struct;

static inline void flush_tlb_mm(struct mm_struct *mm)
{
}

static inline void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
}

#endif

#endif /* __ASM_LKL_TLBFLUSH_H */
