#include <linux/kernel.h>
#include <linux/mm_types.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/rcupdate.h>
#include <linux/sched/stat.h>
#include <linux/sched/debug.h>
#include <linux/sched/mm.h>
#include <linux/cpu.h>
#include <linux/kthread.h>
#include <linux/interrupt.h>
#include <linux/tick.h>
#include <asm/host_ops.h>
#include <asm/cpu.h>
#include <asm/thread_info.h>
#include <asm/unistd.h>
#include <asm/sched.h>
#include <asm/syscalls.h>


/*
 * This structure is used to get access to the "LKL CPU" that allows us to run
 * Linux code. Because we have to deal with various synchronization requirements
 * between idle thread, system calls, interrupts, "reentrancy", CPU shutdown,
 * imbalance wake up (i.e. acquire the CPU from one thread and release it from
 * another), we can't use a simple synchronization mechanism such as (recursive)
 * mutex or semaphore. Instead, we use a mutex and a bunch of status data plus a
 * semaphore.
 */
struct lkl_cpu {
	/* lock that protects the CPU status data */
	struct lkl_mutex *lock;
	/*
	 * Since we must free the cpu lock during shutdown we need a
	 * synchronization algorithm between lkl_cpu_shutdown() and the CPU
	 * access functions since lkl_cpu_get() gets called from thread
	 * destructor callback functions which may be scheduled after
	 * lkl_cpu_shutdown() has freed the cpu lock.
	 *
	 * An atomic counter is used to keep track of the number of running
	 * CPU access functions and allow the shutdown function to wait for
	 * them.
	 *
	 * The shutdown functions adds MAX_THREADS to this counter which allows
	 * the CPU access functions to check if the shutdown process has
	 * started.
	 *
	 * This algorithm assumes that we never have more the MAX_THREADS
	 * requesting CPU access.
	 */
	#define MAX_THREADS 1000000
	unsigned int shutdown_gate;
	bool irqs_pending;
	/* no of threads waiting the CPU */
	unsigned int sleepers;
	/* no of times the current thread got the CPU */
	unsigned int count;
	/* current thread that owns the CPU */
	lkl_thread_t owner;
	/* semaphore for threads waiting the CPU */
	struct lkl_sem *sem;
	/* semaphore used for shutdown */
	struct lkl_sem *shutdown_sem;
} cpus[NR_CPUS];

static int max_cpu_no;
static struct completion enter_idle[NR_CPUS];

int __thread lkl_tls_cpu;

lkl_thread_t lkl_cpu_owner(int cpu)
{
	return cpus[cpu].owner;
}

unsigned int lkl_cpu_count(int cpu)
{
	return cpus[cpu].count;
}

void lkl_set_current_cpu(int cpu)
{
	lkl_tls_cpu = cpu;
}

static inline struct lkl_cpu *current_cpu(void)
{
	return &cpus[raw_smp_processor_id()];
}

static int __cpu_try_get_lock(int cpu_no, int n)
{
	lkl_thread_t self;
	struct lkl_cpu *cpu = &cpus[cpu_no];

	if (__sync_fetch_and_add(&cpu->shutdown_gate, n) >= MAX_THREADS)
		return -2;

	lkl_ops->mutex_lock(cpu->lock);

	if (cpu->shutdown_gate >= MAX_THREADS)
		return -1;

	self = lkl_ops->thread_self();

	if (cpu->owner && !lkl_ops->thread_equal(cpu->owner, self))
		return 0;

	cpu->owner = self;
	cpu->count++;

	return 1;
}

static void __cpu_try_get_unlock(int cpu_no, int lock_ret, int n)
{
	struct lkl_cpu *cpu = &cpus[cpu_no];

	if (lock_ret >= -1)
		lkl_ops->mutex_unlock(cpu->lock);
	__sync_fetch_and_sub(&cpu->shutdown_gate, n);
}

void lkl_cpu_change_owner(int cpu_no, lkl_thread_t owner)
{
	struct lkl_cpu *cpu = &cpus[cpu_no];

	lkl_ops->mutex_lock(cpu->lock);
	if (cpu->count > 1) {
		lkl_bug("%s: bad count while changing owner: count=%lx owner=%lx self=%lx\n",
				__func__, cpu->count, cpu->owner, lkl_ops->thread_self());
	}
	cpu->owner = owner;
	lkl_ops->mutex_unlock(cpu->lock);
}

int __lkl_cpu_get(int cpu_no)
{
	struct lkl_cpu *cpu = &cpus[cpu_no];
	int ret;

	ret = __cpu_try_get_lock(cpu_no, 1);

	while (ret == 0) {
		cpu->sleepers++;
		__cpu_try_get_unlock(cpu_no, ret, 0);
		lkl_ops->sem_down(cpu->sem);
		ret = __cpu_try_get_lock(cpu_no, 0);
	}

	__cpu_try_get_unlock(cpu_no, ret, 1);

	return ret;
}

void __lkl_cpu_put(int cpu_no)
{
	struct lkl_cpu *cpu = &cpus[cpu_no];

	lkl_ops->mutex_lock(cpu->lock);

	if (!cpu->count || !cpu->owner ||
	    !lkl_ops->thread_equal(cpu->owner, lkl_ops->thread_self())) {
		dump_stack();
		lkl_bug("CPU%d/%s: unbalanced put count=%lx owner=%lx current=%p/%d/%s/h-tid=%lx self=%lx\n",
				cpu_no, __func__, cpu->count, cpu->owner,
				current, current->pid, current->comm, task_thread_info(current)->tid,
				lkl_ops->thread_self());
	}

	while (cpu->irqs_pending && !irqs_disabled()) {
		cpu->irqs_pending = false;
		lkl_ops->mutex_unlock(cpu->lock);
		run_irqs();
		lkl_ops->mutex_lock(cpu->lock);
	}

	if (test_ti_thread_flag(current_thread_info(), TIF_HOST_THREAD) &&
	    !single_task_running() && cpu->count == 1) {
		if (in_interrupt())
			lkl_bug("%s: in interrupt\n", __func__);
		lkl_ops->mutex_unlock(cpu->lock);
		thread_sched_jb();
		return;
	}

	if (--(cpu->count) > 0) {
		lkl_ops->mutex_unlock(cpu->lock);
		return;
	}

	if (cpu->sleepers) {
		cpu->sleepers--;
		lkl_ops->sem_up(cpu->sem);
	}

	cpu->owner = 0;

	lkl_ops->mutex_unlock(cpu->lock);
}

int lkl_cpu_get(void)
{
	return __lkl_cpu_get(smp_processor_id());
}

void lkl_cpu_put(void)
{
	__lkl_cpu_put(smp_processor_id());
}

int lkl_cpu_try_run_irq(int irq)
{
	int ret;

	ret = __cpu_try_get_lock(raw_smp_processor_id(), 1);
	if (!ret) {
		set_irq_pending(irq);
		current_cpu()->irqs_pending = true;
	}
	__cpu_try_get_unlock(raw_smp_processor_id(), ret, 1);

	return ret;
}

void lkl_cpu_shutdown(void)
{
	int cpu;

	for (cpu=0; cpu<NR_CPUS; cpu++)
		__sync_fetch_and_add(&cpus[cpu].shutdown_gate, MAX_THREADS);
}

void lkl_cpu_wait_shutdown(void)
{
	struct lkl_cpu *cpu = current_cpu();

	lkl_ops->sem_down(cpu->shutdown_sem);
	lkl_ops->sem_free(cpu->shutdown_sem);
}

static void lkl_cpu_cleanup(bool shutdown)
{
	int no;
	struct lkl_cpu *cpu;

	for (no=0; no<max_cpu_no; no++) {
		cpu = &cpus[no];
		if (!cpu->shutdown_gate)
			break;

		while (__sync_fetch_and_add(&cpu->shutdown_gate, 0) > MAX_THREADS)
			;

		if (shutdown)
			lkl_ops->sem_up(cpu->shutdown_sem);
		else if (cpu->shutdown_sem) {
			lkl_ops->sem_free(cpu->shutdown_sem);
			cpu->shutdown_sem = NULL;
		}
		if (cpu->sem) {
			lkl_ops->sem_free(cpu->sem);
			cpu->sem = NULL;
		}
		if (cpu->lock) {
			lkl_ops->mutex_free(cpu->lock);
			cpu->lock = NULL;
		}
	}
}

void arch_cpu_idle(void)
{
	int cpu_no = raw_smp_processor_id();
	struct lkl_cpu *cpu = &cpus[cpu_no];

	if (cpu->shutdown_gate >= MAX_THREADS) {

		lkl_ops->mutex_lock(cpu->lock);
		while (cpu->sleepers--)
			lkl_ops->sem_up(cpu->sem);
		lkl_ops->mutex_unlock(cpu->lock);

		lkl_cpu_cleanup(true);

		lkl_ops->thread_exit();
	}
	/* enable irqs now to allow direct irqs to run */
	local_irq_enable();

	/* switch to idle_host_task */
	wakeup_idle_host_task();
}

int lkl_cpu_init(void)
{
	int no;
	struct lkl_cpu *cpu;

	if (lkl_ops->sysconf)
		max_cpu_no = lkl_ops->sysconf(0); /* _SC_NPROCESSORS_CONF, FIXME: hardcode here  */
	else
		max_cpu_no = NR_CPUS;
	if (max_cpu_no < 0)
		return -max_cpu_no;
	lkl_printf("Setup %d LKL CPUs\n", max_cpu_no);

	for (no=0; no<max_cpu_no && no<NR_CPUS; no++) {
		cpu = &cpus[no];

		cpu->lock = lkl_ops->mutex_alloc(0);
		cpu->sem = lkl_ops->sem_alloc(0);
		cpu->shutdown_sem = lkl_ops->sem_alloc(0);

		if (!cpu->lock || !cpu->sem || !cpu->shutdown_sem) {
			lkl_cpu_cleanup(false);
			return -ENOMEM;
		}
	}

	return 0;
}

void lkl_smp_init_secondary_idle(void)
{
	int cpu;

	for (cpu=1; cpu<NR_CPUS; ++cpu) {
		init_completion(&enter_idle[cpu]);
	}
}

void lkl_smp_enter_secondary_idle(void)
{
	int cpu;

	for (cpu=1; cpu<NR_CPUS; ++cpu) {
		complete(&enter_idle[cpu]);
	}
}

int lkl_smp_init(void)
{
	int i;

	for (i=0; i<max_cpu_no; i++) {
		set_cpu_possible(i, true);
		set_cpu_present(i, true);
	}
	lkl_smp_init_secondary_idle();

	return 0;
}

int __cpu_up(unsigned int cpu, struct task_struct *idle)
{
	struct thread_info *ti = task_thread_info(idle);

	set_cpu_online(cpu, true);
	lkl_set_current(cpu, idle);
	set_task_cpu(idle, cpu);
	lkl_ops->sem_up(ti->sched_sem);
	return 0;
}

/* pure idle kthread, please see:
	init_idle() -> fork_idle() and LKL:copy_thread()
 */
int lkl_start_secondary(void *unused)
{
	int cpu;
	struct secondary_idle_entry entry;

	cpu = smp_processor_id();
	preempt_disable();
	notify_cpu_starting(cpu);
	mmgrab(&init_mm);
	current->active_mm = &init_mm;

	entry.cpu = cpu;
	lkl_cpu_get();
	if (smp_idle_host_init(&entry)) {
		lkl_printf("CPU%d idle host thread create failed\n", cpu);
		panic("smp_idle_host_init() error\n");
		return -1;
	}

	local_irq_enable();

	lkl_cpu_clock_init(cpu);
	cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);
	return 0;
}

typedef enum {
	LKL_IPI_EXIT		= 1UL<<0,
	LKL_IPI_RESCHED		= 1UL<<1,
	LKL_IPI_CALLFUNC	= 1UL<<2,
	LKL_IPI_TICKBC		= 1UL<<3,
	LKL_IPI_LAST		= LKL_IPI_TICKBC,
	LKL_IPI_MASK		= (LKL_IPI_LAST-1)|LKL_IPI_LAST,
} lkl_ipi_type;

struct lkl_ipi_gate {
	struct lkl_mutex *lock;
	struct lkl_sem *sched;
	lkl_thread_t thread;
	unsigned long pending;
} lkl_ipi_gate[NR_CPUS];

static void lkl_ipi_gate_close(int last)
{
	long cpu;

	struct lkl_ipi_gate *g;

	for (cpu=0; cpu<NR_CPUS; cpu++) {
		g = &lkl_ipi_gate[cpu];

		if (g->thread) {
			lkl_ops->mutex_lock(g->lock);
			g->pending |= LKL_IPI_EXIT;
			lkl_ops->mutex_unlock(g->lock);

			lkl_ops->sem_up(g->sched);
			lkl_ops->thread_join(g->thread);
		}
		if (g->lock)
			lkl_ops->mutex_free(g->lock);
		if (g->sched)
			lkl_ops->sem_free(g->sched);
		if (!g->lock && !g->sched && !g->thread)
			break;
	}
	free_irq(LKL_IRQ_IPI, NULL);
}

/* this needs be called out of irq_enter()/irq_exit() pair */
void lkl_scheduler_ipi(void)
{
	int cpu = smp_processor_id();
	struct lkl_ipi_gate *g;
	long pending;

	g = &lkl_ipi_gate[cpu];
	if (!g->lock)
		return;

	lkl_ops->mutex_lock(g->lock);
	pending = g->pending;
	g->pending &= ~LKL_IPI_RESCHED;
	lkl_ops->mutex_unlock(g->lock);

	if (pending & LKL_IPI_RESCHED) {
		scheduler_ipi();
	}
}

/* this needs be called in irq_enter()/irq_exit() pair */
void lkl_ipi(void)
{
	int cpu = smp_processor_id();
	struct lkl_ipi_gate *g;
	long pending;

	g = &lkl_ipi_gate[cpu];
	if (!g->lock)
		return;

	lkl_ops->mutex_lock(g->lock);
	pending = g->pending;
	g->pending &= ~(LKL_IPI_CALLFUNC|LKL_IPI_TICKBC);
	lkl_ops->mutex_unlock(g->lock);

	if (pending & LKL_IPI_CALLFUNC) {
		generic_smp_call_function_interrupt();
	}

	if (pending & LKL_IPI_TICKBC) {
		tick_receive_broadcast();
	}
}

static irqreturn_t ipi_handler(int irq, void *dev_id)
{
	return IRQ_HANDLED;
}

extern struct task_struct *idle_host_tasks[NR_CPUS];

static void lkl_ipi_thread(void *arg)
{
	long cpu = (long)arg;
	long pending;
	struct lkl_ipi_gate *g;

	g = &lkl_ipi_gate[cpu];
	while (1) {
		lkl_ops->sem_down(g->sched);

		pending = g->pending;
		if (pending & LKL_IPI_EXIT)
			break;

		lkl_trigger_irq(cpu, LKL_IRQ_IPI);
		if (!(pending & (LKL_IPI_RESCHED)))
			continue;

		if (current == idle_host_tasks[cpu]) {
			struct thread_info *ti = task_thread_info(current);
			int ret;

			ret = __cpu_try_get_lock(cpu, 1);
			if (ret<=0) {
				__cpu_try_get_unlock(cpu, ret, 1);
				if (!ret)
					continue;
				else
					break;
			}

			cpus[cpu].owner = ti->tid;

			local_irq_disable();
			rcu_note_context_switch();
			local_irq_enable();
			__cpu_try_get_unlock(cpu, ret, 1);

			lkl_ops->sem_up(ti->sched_sem); /* idle host task will unlock this cpu */
		}
	}

	lkl_ops->thread_exit();
}

static int lkl_ipi_gate_open(void)
{
	long cpu, ret;
	struct lkl_ipi_gate *g;

	ret = request_irq(LKL_IRQ_IPI, ipi_handler, IRQF_NO_THREAD, "LKL IPI", NULL);
	if (ret)
		return ret;

	for (cpu=0; cpu<NR_CPUS; cpu++) {
		g = &lkl_ipi_gate[cpu];

		memset(g, 0, sizeof(struct lkl_ipi_gate));
		g->lock = lkl_ops->mutex_alloc(0);
		g->sched = lkl_ops->sem_alloc(0);
		if (g->lock && g->sched)
			g->thread = lkl_ops->thread_create(lkl_ipi_thread, (void*)cpu);
		if (!g->lock || !g->sched || !g->thread)
			break;
	}

	if (cpu>=NR_CPUS)
		return 0;

	lkl_ipi_gate_close(cpu);
	return -ENODEV;
}

void smp_prepare_cpus(unsigned int max_cpus)
{
	if (lkl_ipi_gate_open())
		panic("lkl_ipi_gate_open() failed\n");
}

static inline void lkl_wake_up_ipi_thread(int cpu, lkl_ipi_type code)
{
	struct lkl_ipi_gate *g;

	if (code & ~LKL_IPI_MASK)
		return;

	g = &lkl_ipi_gate[cpu];
	lkl_ops->mutex_lock(g->lock);
	g->pending |= code;
	lkl_ops->mutex_unlock(g->lock);

	lkl_ops->sem_up(g->sched);
}

void lkl_tick_broadcast(int cpu)
{
	lkl_wake_up_ipi_thread(cpu, LKL_IPI_TICKBC);
}

void smp_send_reschedule(int cpu)
{
	lkl_wake_up_ipi_thread(cpu, LKL_IPI_RESCHED);
}

void arch_send_call_function_single_ipi(int cpu)
{
	lkl_wake_up_ipi_thread(cpu, LKL_IPI_CALLFUNC);
}

void arch_send_call_function_ipi_mask(const struct cpumask *mask)
{
	int cpu;

	for_each_cpu(cpu, mask) {
		arch_send_call_function_single_ipi(cpu);
	}
}
