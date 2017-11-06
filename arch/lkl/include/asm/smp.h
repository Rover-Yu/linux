#ifndef _ASM_LKL_SMP_H
#define _ASM_LKL_SMP_H
#ifndef __ASSEMBLY__

#ifdef CONFIG_SMP

struct task_struct;
struct cpumask;

static inline void smp_prepare_boot_cpu(void) { }
static inline void smp_prepare_cpus(unsigned int max_cpus) { }

static inline int __cpu_up(unsigned int cpu, struct task_struct *idle) { return 0; }
static inline void smp_send_stop(void) { }
static inline void smp_send_reschedule(int cpu) { }
static inline void smp_cpus_done(unsigned int max_cpus) { }

static inline void arch_send_call_function_single_ipi(int cpu) { }
static inline void arch_send_call_function_ipi_mask(const struct cpumask *mask) { }

#define raw_smp_processor_id()		0		//FIXME

#endif

#endif /* __ASSEMBLY__ */
#endif /* _ASM_LKL_SMP_H */
