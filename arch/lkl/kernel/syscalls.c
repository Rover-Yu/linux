#include <linux/stat.h>
#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/jhash.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/net.h>
#include <linux/task_work.h>
#include <linux/syscalls.h>
#include <linux/kthread.h>
#include <linux/irqflags.h>
#include <linux/platform_device.h>
#include <asm/host_ops.h>
#include <asm/syscalls.h>
#include <asm/syscalls_32.h>
#include <asm/cpu.h>
#include <asm/sched.h>

static asmlinkage long sys_virtio_mmio_device_add(long base, long size,
						  unsigned int irq);

typedef long (*syscall_handler_t)(long arg1, ...);

#undef __SYSCALL
#define __SYSCALL(nr, sym) [nr] = (syscall_handler_t)sym,

syscall_handler_t syscall_table[__NR_syscalls] = {
	[0 ... __NR_syscalls - 1] =  (syscall_handler_t)sys_ni_syscall,
#include <asm/unistd.h>

#if __BITS_PER_LONG == 32
#include <asm/unistd_32.h>
#endif
};

static long run_syscall(long no, long *params)
{
	long ret;

	if (no < 0 || no >= __NR_syscalls)
		return -ENOSYS;

	ret = syscall_table[no](params[0], params[1], params[2], params[3],
				params[4], params[5]);

	task_work_run();

	return ret;
}


#define CLONE_FLAGS (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_THREAD |	\
		     CLONE_SIGHAND | SIGCHLD)

static int host_task_id;
static struct task_struct *host0;

static int new_host_task(struct task_struct **task)
{
	pid_t pid;

	switch_to_host_task(host0);

	pid = kernel_thread(host_task_stub, NULL, CLONE_FLAGS);
	if (pid < 0)
		return pid;

	rcu_read_lock();
	*task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();

	host_task_id++;

	snprintf((*task)->comm, sizeof((*task)->comm), "host%d", host_task_id);

	return 0;
}
static void exit_task(void)
{
	do_exit(0);
}

static void del_host_task(void *arg)
{
	struct task_struct *task = (struct task_struct *)arg;
	struct thread_info *ti = task_thread_info(task);

	if (lkl_cpu_get() < 0)
		return;

	switch_to_host_task(task);
	host_task_id--;
	set_ti_thread_flag(ti, TIF_SCHED_JB);
	lkl_ops->jmp_buf_set(&ti->sched_jb, exit_task);
}

static struct lkl_tls_key *task_key;

long lkl_syscall(long no, long *params)
{
	struct task_struct *task = host0;
	long ret;

	lkl_set_current_cpu(task_cpu(task));
	ret = lkl_cpu_get();
	if (ret < 0)
		return ret;

	if (lkl_ops->tls_get) {
		task = lkl_ops->tls_get(task_key);
		if (!task) {
			ret = new_host_task(&task);
			if (ret)
				goto out;
			lkl_ops->tls_set(task_key, task);
		}
		if (task_cpu(task) != smp_processor_id()) {
			lkl_cpu_put();
			lkl_set_current_cpu(task_cpu(task));
			ret = lkl_cpu_get();
			if (ret < 0)
				return ret;
		}
	}

	switch_to_host_task(task);

	ret = run_syscall(no, params);

	if (no == __NR_reboot) {
		thread_sched_jb();
		return ret;
	}

out:
	lkl_cpu_put();

	return ret;
}

struct task_struct *idle_host_tasks[NR_CPUS];

/* called from idle, don't failed, don't block */
void wakeup_idle_host_task(void)
{
	int cpu = smp_processor_id();

	if (!need_resched() && idle_host_tasks[cpu]) {
		wake_up_process(idle_host_tasks[cpu]);
	}
}

static int secondary_idle_host_task_loop(void *voidp)
{
	struct secondary_idle_entry *arg = voidp;
	int cpu = arg->cpu;
	int local_cpu;
	struct thread_info *ti = task_thread_info(current);

	while (1) {
		local_cpu = raw_smp_processor_id();
		if (local_cpu == cpu) {
			break;
		}
		set_current_state(TASK_UNINTERRUPTIBLE);
		kthread_bind(current, cpu);
		schedule_timeout(10);	/* TODO: any better fix ? */
		if (idle_host_tasks[cpu] == NULL)
			return 0;
	}

	snprintf(current->comm, TASK_COMM_LEN, "idle_host_task%d", arg->cpu);
	set_ti_thread_flag(task_thread_info(current), TIF_HOST_THREAD);
//	rcu_idle_enter();
	for (;;) {
		lkl_cpu_put();

		lkl_ops->sem_down(ti->sched_sem);

		if (idle_host_tasks[cpu] == NULL) {
			lkl_ops->thread_exit();
			return 0;
		}

		if (irqs_disabled())	/*  IPI may wakeup this under enabled IRQ */
			schedule_tail(ti->prev_sched);
	}
}

int smp_idle_host_init(struct secondary_idle_entry *arg)
{
	struct task_struct *tsk;
	pid_t pid;

	/* We are at the Middle Age, the kthread_create*API is not safe for us */
	pid = kernel_thread(secondary_idle_host_task_loop,
					(void*)arg, CLONE_FLAGS);
	if (pid < 0)
		return pid;

	rcu_read_lock();
	tsk = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();
	idle_host_tasks[arg->cpu] = tsk;

	return 0;
}

int idle_host_task_loop(void *unused)
{
	struct thread_info *ti = task_thread_info(current);

	snprintf(current->comm, sizeof(current->comm), "idle_host_task0");
	set_thread_flag(TIF_HOST_THREAD);
	lkl_cpu_clock_init(0);

	for (;;) {
//		rcu_idle_enter();
		lkl_cpu_put();
		lkl_ops->sem_down(ti->sched_sem);

		if (idle_host_tasks[0] == NULL) {
			lkl_ops->thread_exit();
			return 0;
		}

		if (irqs_disabled())	/*  IPI may wakeup this under enabled IRQ */
			schedule_tail(ti->prev_sched);
//		rcu_idle_exit();
	}
}

int syscalls_init(void)
{
	pid_t pid;
	struct task_struct *task;

	snprintf(current->comm, sizeof(current->comm), "syscalls_init()");
	set_thread_flag(TIF_HOST_THREAD);
	host0 = current;

	if (lkl_ops->tls_alloc) {
		task_key = lkl_ops->tls_alloc(del_host_task);
		if (!task_key)
			return -1;
	}

	pid = kernel_thread(idle_host_task_loop, NULL, CLONE_FLAGS);
	if (pid < 0) {
		if (lkl_ops->tls_free)
			lkl_ops->tls_free(task_key);
		return -1;
	}
	rcu_read_lock();
	task = find_task_by_pid_ns(pid, &init_pid_ns);
	rcu_read_unlock();
	idle_host_tasks[0] = task;

	snprintf(current->comm, sizeof(current->comm), "host0");

	return 0;
}

void syscalls_cleanup(void)
{
	int cpu;

	for (cpu=0; cpu<NR_CPUS; cpu++) {
		if (idle_host_tasks[cpu]) {
			struct thread_info *ti = task_thread_info(idle_host_tasks[cpu]);

			idle_host_tasks[cpu] = NULL;
			lkl_ops->sem_up(ti->sched_sem);
			lkl_ops->thread_join(ti->tid);
		}
	}

	if (lkl_ops->tls_free)
		lkl_ops->tls_free(task_key);
}

SYSCALL_DEFINE3(virtio_mmio_device_add, long, base, long, size, unsigned int,
		irq)
{
	struct platform_device *pdev;
	int ret;

	struct resource res[] = {
		[0] = {
		       .start = base,
		       .end = base + size - 1,
		       .flags = IORESOURCE_MEM,
		       },
		[1] = {
		       .start = irq,
		       .end = irq,
		       .flags = IORESOURCE_IRQ,
		       },
	};

	pdev = platform_device_alloc("virtio-mmio", PLATFORM_DEVID_AUTO);
	if (!pdev) {
		dev_err(&pdev->dev, "%s: Unable to device alloc for virtio-mmio\n", __func__);
		return -ENOMEM;
	}

	ret = platform_device_add_resources(pdev, res, ARRAY_SIZE(res));
	if (ret) {
		dev_err(&pdev->dev, "%s: Unable to add resources for %s%d\n", __func__, pdev->name, pdev->id);
		goto exit_device_put;
	}

	ret = platform_device_add(pdev);
	if (ret < 0) {
		dev_err(&pdev->dev, "%s: Unable to add %s%d\n", __func__, pdev->name, pdev->id);
		goto exit_release_pdev;
	}

	return pdev->id;

exit_release_pdev:
	platform_device_del(pdev);
exit_device_put:
	platform_device_put(pdev);

	return ret;
}
