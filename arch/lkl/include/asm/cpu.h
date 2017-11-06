#ifndef _ASM_LKL_CPU_H
#define _ASM_LKL_CPU_H

int __lkl_cpu_get(int cpu);
void __lkl_cpu_put(int cpu);

int lkl_cpu_get(void);
void lkl_cpu_put(void);
int lkl_cpu_try_run_irq(int irq);
int lkl_cpu_init(void);
void lkl_cpu_shutdown(void);
void lkl_cpu_wait_shutdown(void);
void lkl_cpu_change_owner(int cpu, lkl_thread_t owner);
void lkl_cpu_set_irqs_pending(void);

void lkl_tick_broadcast(int cpu);
int lkl_smp_init(void);
int lkl_start_secondary(void *unused);

void lkl_set_current_cpu(int cpu);
extern int __thread lkl_tls_cpu;

lkl_thread_t lkl_cpu_owner(int cpu);
unsigned int lkl_cpu_count(int cpu);

void lkl_cpu_first_idle(int cpu);

void lkl_cpu_clock_init(int cpu);
#endif /* _ASM_LKL_CPU_H */
