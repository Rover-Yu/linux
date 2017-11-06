#ifndef _ASM_LKL_IRQ_H
#define _ASM_LKL_IRQ_H

#define IRQ_STATUS_BITS		(sizeof(long) * 8)
#define NR_IRQS			((int)(IRQ_STATUS_BITS * IRQ_STATUS_BITS))

#define LKL_IRQ_BASE	(NR_IRQS-128)
#define LKL_IRQ_IPI	LKL_IRQ_BASE

void run_irqs(void);
void set_irq_pending(int irq);
void lkl_ipi(void);
void lkl_scheduler_ipi(void);

#include <uapi/asm/irq.h>

#endif
