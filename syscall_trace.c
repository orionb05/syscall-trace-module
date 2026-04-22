/*
 * syscall_trace.c
 *
 * Kernel module that traces system call latency and frequency using kretprobes.
 * Maintains per-CPU statistics for read, mmap, futex, and openat syscalls with
 * latency histograms, and exposes aggregated data through /proc/syscall-trace.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/printk.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
#include <linux/minmax.h>
#endif

MODULE_AUTHOR("Orion Brown");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Syscall tracing module for per-CPU syscall latency and count aggregation");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
#define HAVE_PROC_OPS
#endif

// Proc interface configuration
#define PROCFS_FILENAME "syscall-trace"
#define INPUT_MAX_LEN 33

// Syscall tracing configuration
#define NUM_BUCKETS 16
static const u64 bucket_bounds[NUM_BUCKETS] = { 1e3,   2e3,   4e3, 8e3, 16e3, 32e3, 64e3, 128e3,
						256e3, 512e3, 1e6, 2e6, 4e6,  8e6,  16e6, U64_MAX };

// System calls to be tracked
enum syscalls_tracked { SYSCALL_READ, SYSCALL_MMAP, SYSCALL_FUTEX, SYSCALL_OPENAT, NUM_SYSCALLS };

static const char *syscall_symbols[NUM_SYSCALLS] = {
	[SYSCALL_READ] = "__x64_sys_read",
	[SYSCALL_MMAP] = "__x64_sys_mmap",
	[SYSCALL_FUTEX] = "__x64_sys_futex",
	[SYSCALL_OPENAT] = "__x64_sys_openat",
};

// Statistics structure for a single syscall (latency, count, histogram)
struct syscall_stats {
	u64 count;
	u64 total_latency;
	u64 max_latency;
	u64 latency_hist[NUM_BUCKETS];
};

// Per-CPU statistics for each tracked syscall
DEFINE_PER_CPU(struct syscall_stats, percpu_stats[NUM_SYSCALLS]);
static struct syscall_stats global_stats[NUM_SYSCALLS];

// Wrapper containing kretprobe and associated metadata
struct probe_wrapper {
	struct kretprobe krp;
	struct syscall_stats __percpu *stats;
	int syscall_id;
};
static struct probe_wrapper probes[NUM_SYSCALLS];

// Data structure attached to each kretprobe instance
struct probe_data {
	u64 entry_time;
	u8 valid;
};

// Array of probe pointers for batch registration
static struct kretprobe *syscall_kretprobe_ptrs[NUM_SYSCALLS];

// State variables controlling tracing window and target syscall
static u64 run_start_time = 0;
static u64 run_end_time = 0;
static int run_syscall_id;
static int generator_pid;

// Input buffer for /proc writes
static char input_buffer[INPUT_MAX_LEN];

// Proc file entry
static struct proc_dir_entry *entry;

// Seq file operations for /proc interface
static void *my_seq_start(struct seq_file *s, loff_t *pos)
{
	// Return the global stats for the current tracked syscall
	if (*pos == 0)
		return &global_stats[run_syscall_id];
	return NULL;
}

static void *my_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	// Single-entry seq file: only one syscall displayed at a time
	return NULL;
}

static void my_seq_stop(struct seq_file *s, void *v)
{
	// Nothing to do; we use static values in start()
}

static int my_seq_show(struct seq_file *s, void *v)
{
	struct syscall_stats *stat = (struct syscall_stats *)v;

	seq_printf(s, "%s\n", syscall_symbols[stat - global_stats]);
	seq_printf(s, "%llu\n", stat->count);
	seq_printf(s, "%llu\n", stat->total_latency);
	seq_printf(s, "%llu\n", stat->max_latency);

	for (int j = 0; j < NUM_BUCKETS; j++)
		seq_printf(s, "%llu ", stat->latency_hist[j]);
	seq_putc(s, '\n');

	return 0;
}

static struct seq_operations my_seq_ops = {
	.start = my_seq_start,
	.next = my_seq_next,
	.stop = my_seq_stop,
	.show = my_seq_show,
};

// Aggregates per-CPU stats to global stats and prepares for /proc read
static int my_seq_open(struct inode *inode, struct file *file)
{
	// Clear global stats
	memset(global_stats, 0, sizeof(global_stats));

	// Aggregate stats from each CPU core
	int cpu;
	for_each_possible_cpu(cpu) {
		for (int i = 0; i < NUM_SYSCALLS; i++) {
			struct syscall_stats *s = per_cpu_ptr(&percpu_stats[i], cpu);

			global_stats[i].count += s->count;
			global_stats[i].total_latency += s->total_latency;
			global_stats[i].max_latency =
				max(global_stats[i].max_latency, s->max_latency);
			for (int j = 0; j < NUM_BUCKETS; j++)
				global_stats[i].latency_hist[j] += s->latency_hist[j];
		}
	}

	return seq_open(file, &my_seq_ops);
}

// Handles /proc writes: syscall_id,run_time_seconds[,generator_pid]
static ssize_t my_proc_write(struct file *file, const char __user *buff, size_t len, loff_t *off)
{
	if (*off != 0)
		return -EINVAL;

	if (len == 0)
		return -EINVAL;

	if (len >= INPUT_MAX_LEN) {
		pr_info("Input too long; max %d characters\n", INPUT_MAX_LEN - 1);
		len = INPUT_MAX_LEN - 1;
	}

	if (copy_from_user(input_buffer, buff, len))
		return -EFAULT;

	// Null-terminate the input string
	input_buffer[len] = '\0';

	*off += len;

	char *p = input_buffer;
	char *tok;

	// Parse syscall_id
	tok = strsep(&p, ",");
	if (!tok)
		return -EINVAL;

	if (kstrtoint(tok, 10, &run_syscall_id))
		return -EINVAL;

	if (run_syscall_id < 0 || run_syscall_id >= NUM_SYSCALLS)
		return -EINVAL;

	// Parse run_time_seconds
	tok = strsep(&p, ",");
	if (!tok)
		return -EINVAL;

	unsigned int run_time_seconds;
	if (kstrtoint(tok, 10, &run_time_seconds))
		return -EINVAL;

	if (run_time_seconds <= 0 || run_time_seconds > 1024)
		return -EINVAL;

	// Parse optional generator_pid
	tok = strsep(&p, ",");
	if (tok) {
		if (kstrtoint(tok, 10, &generator_pid))
			return -EINVAL;
		if (generator_pid < 0)
			return -EINVAL;
	}

	// Clear per-CPU statistics
	int cpu;
	for_each_possible_cpu(cpu) {
		for (int i = 0; i < NUM_SYSCALLS; i++) {
			struct syscall_stats *s = per_cpu_ptr(&percpu_stats[i], cpu);
			memset(s, 0, sizeof(*s));
		}
	}

	// Set tracing time window
	u64 now = ktime_get_ns();
	WRITE_ONCE(run_start_time, now);
	WRITE_ONCE(run_end_time, now + run_time_seconds * NSEC_PER_SEC);

	pr_info("Syscall_trace starting at time: %llu\n", run_start_time);
	pr_info("Ending at time: %llu\n", run_end_time);

	return len;
}

// /proc file operations (version-dependent structure)
#ifdef HAVE_PROC_OPS
static struct proc_ops file_ops = {
	.proc_open = my_seq_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = seq_release,
	.proc_write = my_proc_write,
};

#else
static const struct file_operations file_ops = {
	.open = my_seq_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
	.write = my_proc_write,
};
#endif

// Kretprobe entry handler: records entry time if conditions are met
static int entry_callback(struct kretprobe_instance *ki, struct pt_regs *regs)
{
	struct kretprobe *kp = get_kretprobe(ki);
	struct probe_wrapper *pw = container_of(kp, struct probe_wrapper, krp);
	struct probe_data *d = (struct probe_data *)ki->data;

	u64 start = READ_ONCE(run_start_time);
	u64 end = READ_ONCE(run_end_time);
	u64 now = ktime_get_ns();

	// Check if this probe invocation should be tracked:
	// - Match generator PID (if specified)
	// - Match target syscall ID
	// - Fall within tracing time window
	bool invalid =
		((generator_pid != 0 && current->pid != generator_pid) ||
		 (pw->syscall_id != READ_ONCE(run_syscall_id)) ||
		 (now < start) || (now > end));

	if (!invalid) {
		d->entry_time = now;
		d->valid = 1;
	}

	return 0;
}

// Kretprobe exit handler: calculates latency and updates statistics
static int exit_callback(struct kretprobe_instance *ki, struct pt_regs *regs)
{
	u64 now = ktime_get_ns();
	u64 start = READ_ONCE(run_start_time);
	u64 end = READ_ONCE(run_end_time);

	struct kretprobe *kp = get_kretprobe(ki);
	struct probe_wrapper *pw = container_of(kp, struct probe_wrapper, krp);
	struct probe_data *d = (struct probe_data *)ki->data;

	// Skip if entry handler didn't mark as valid, or entry/exit time is outside window
	bool invalid = ((d->valid != 1) ||
			(d->entry_time < start || now > end));

	if (!invalid) {
		struct syscall_stats *s = this_cpu_ptr(pw->stats);

		u64 latency = now - d->entry_time;

		s->count++;
		s->total_latency += latency;
		s->max_latency = max(s->max_latency, latency);

		// Place latency into appropriate histogram bucket
		int bucket = 0;
		while (latency > bucket_bounds[bucket])
			bucket++;
		s->latency_hist[bucket]++;
	}

	// Always clear valid flag to avoid stale data
	d->valid = 0;

	return 0;
}

// Module initialization: registers kretprobes and creates /proc interface
static int __init mod_init(void)
{
	int ret;

	// Configure and store kretprobe structures
	for (int i = 0; i < NUM_SYSCALLS; i++) {
		probes[i].krp.kp.symbol_name = syscall_symbols[i];
		probes[i].krp.entry_handler = &entry_callback;
		probes[i].krp.handler = &exit_callback;
		probes[i].krp.data_size = sizeof(struct probe_data);

		probes[i].stats = &percpu_stats[i];
		probes[i].syscall_id = i;

		syscall_kretprobe_ptrs[i] = &(probes[i].krp);
	}

	// Register kretprobes for all tracked syscalls
	ret = register_kretprobes(syscall_kretprobe_ptrs, NUM_SYSCALLS);
	if (ret)
		goto kretprobe_fail;

	// Create /proc interface
	entry = proc_create(PROCFS_FILENAME, 0666, NULL, &file_ops);
	if (!entry) {
		ret = -ENOMEM;
		goto proc_fail;
	}
	pr_info("/proc/%s created\n", PROCFS_FILENAME);

	return 0;

proc_fail:
	pr_info("Error: Could not initialize /proc/%s\n", PROCFS_FILENAME);
	unregister_kretprobes(syscall_kretprobe_ptrs, NUM_SYSCALLS);

kretprobe_fail:
	pr_info("Error: Could not register kretprobes\n");
	return ret;
}

// Module exit: cleans up /proc interface and unregisters kretprobes
static void __exit mod_exit(void)
{
	remove_proc_entry(PROCFS_FILENAME, NULL);
	pr_info("/proc/%s removed\n", PROCFS_FILENAME);

	unregister_kretprobes(syscall_kretprobe_ptrs, NUM_SYSCALLS);
}

module_init(mod_init);
module_exit(mod_exit);