#ifndef _ASM_LKL_SYSCALLS_H
#define _ASM_LKL_SYSCALLS_H

int syscalls_init(void);
void syscalls_cleanup(void);
long lkl_syscall(long no, long *params);
void wakeup_idle_host_task(void);

struct secondary_idle_entry {
	int cpu;
};
int smp_idle_host_init(struct secondary_idle_entry *entry);
int idle_host_task_loop(void *unused);

#define sys_mmap sys_mmap_pgoff
#define sys_mmap2 sys_mmap_pgoff
#define sys_clone sys_ni_syscall
#define sys_vfork sys_ni_syscall
#define sys_rt_sigreturn sys_ni_syscall

#include <asm-generic/syscalls.h>

#endif /* _ASM_LKL_SYSCALLS_H */
