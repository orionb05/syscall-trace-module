/*
 * syscall_trace.c
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/version.h>

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/kprobes.h>

#include <linux/sched.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
#include <linux/minmax.h>
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
#define HAVE_PROC_OPS
#endif

#define PROCFS_FILENAME "syscall-trace"

#define INPUT_MAX_LEN 33
static char input_buffer[INPUT_MAX_LEN];

static u64 run_until_time = 0;

static struct proc_dir_entry *entry;

#define NUM_BUCKETS 24
struct syscall_stats {
	u64 count;
	u64 total_latency;
	u64 max_latency;
	u64 latency_hist[NUM_BUCKETS];
};

enum syscalls_tracked { SYSCALL_READ, SYSCALL_MMAP, SYSCALL_FUTEX, SYSCALL_CLONE, NUM_SYSCALLS };

// Each core will track syscalls independently, with global_stats aggregating the sums
DEFINE_PER_CPU(struct syscall_stats, percpu_stats[NUM_SYSCALLS]);
static struct syscall_stats global_stats[NUM_SYSCALLS];

struct probe_wrapper {
    struct kretprobe krp;
    struct syscall_stats __percpu *stats;
};
static struct probe_wrapper probes[NUM_SYSCALLS];

static struct kretprobe *syscall_kretprobe_ptrs[NUM_SYSCALLS];

static const char *syscall_symbols[NUM_SYSCALLS] = {
	[SYSCALL_READ] = "__x64_sys_read",
	[SYSCALL_MMAP] = "__x64_sys_mmap",
	[SYSCALL_FUTEX] = "__x64_sys_futex",
	[SYSCALL_CLONE] = "__x64_sys_clone",
};

static void *my_seq_start(struct seq_file *s, loff_t *pos)
{
	// Each sequence prints one syscall's stats
	if (*pos < NUM_SYSCALLS) {
		return &(global_stats[*pos]);
	}

	*pos = 0;
	return NULL;
}

static void *my_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	(*pos)++;
	if (*pos >= NUM_SYSCALLS) {
		return NULL;
	}

	return &(global_stats[*pos]);
}

static void my_seq_stop(struct seq_file *s, void *v)
{
	/* nothing to do, we use a static value in start() */
}

static int my_seq_show(struct seq_file *s, void *v)
{
	struct syscall_stats *stat = (struct syscall_stats *)v;

	seq_printf(s, "%s\n", syscall_symbols[stat - global_stats]);
	seq_printf(s, "%llu\n", stat->count);
	seq_printf(s, "%llu\n", stat->total_latency);
	seq_printf(s, "%llu\n", stat->max_latency);

	return 0;
}

static struct seq_operations my_seq_ops = {
	.start = my_seq_start,
	.next = my_seq_next,
	.stop = my_seq_stop,
	.show = my_seq_show,
};

// Clear and update previous syscall stats, then start printing results
static int my_seq_open(struct inode *inode, struct file *file)
{
	// Clean previous global stat data
	memset(global_stats, 0, sizeof(global_stats));

	// Aggregate stats from reach core
	int cpu;
	for_each_possible_cpu(cpu) {
		for (int i = 0; i < NUM_SYSCALLS; i++) {
			struct syscall_stats *s = per_cpu_ptr(&percpu_stats[i], cpu);

			global_stats[i].count         += s->count;
			global_stats[i].total_latency += s->total_latency;
			global_stats[i].max_latency    = max(global_stats[i].max_latency, s->max_latency);
			for (int j = 0; j < NUM_BUCKETS; j++)
				global_stats[i].latency_hist[j] += s->latency_hist[j];
		}
	}

	// Begin output for each syscall's stats
	return seq_open(file, &my_seq_ops);
}

// Recieve the collection time, reset the statistics, and start collecting syscall stats
static ssize_t my_proc_write(struct file *file, const char __user *buff, size_t len, loff_t *off)
{
	if (len == 0)
		return -EINVAL;

	if (len >= INPUT_MAX_LEN) {
		pr_info("Input too long; max %d characters\n", INPUT_MAX_LEN - 1);
		len = INPUT_MAX_LEN - 1;
	}

	if (copy_from_user(input_buffer, buff, len))
		return -EFAULT;

	input_buffer[len] = '\0'; // Null-terminate the input string

	*off += len;

	// Convert user input to an integer
	unsigned int run_time_seconds;
	if (kstrtoint(input_buffer, 10, &run_time_seconds))
		return -EINVAL;

	if (run_time_seconds == 0 || run_time_seconds > 1024)
		return -EINVAL;

	// Clean syscall stats
	int cpu;
	for_each_possible_cpu(cpu) {
		for (int i = 0; i < NUM_SYSCALLS; i++) {
			struct syscall_stats *s = per_cpu_ptr(&percpu_stats[i], cpu);
			memset(s, 0, sizeof(*s));
		}
	}

	// Signal probes to run for the specified time
	WRITE_ONCE(run_until_time, ktime_get_ns() + run_time_seconds * NSEC_PER_SEC);

	pr_info("Starting collection at %llu seconds\n", ktime_get_ns() / NSEC_PER_SEC);
	pr_info("Ending at %llu seconds\n", run_until_time / NSEC_PER_SEC);

	return len;
}

#ifdef HAVE_PROC_OPS
static struct proc_ops file_ops = {
	// Used on /proc read
	.proc_open = my_seq_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = seq_release,

	// Used on /proc write
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

static int entry_callback(struct kretprobe_instance *ki, struct pt_regs *regs)
{
	u64 now = ktime_get_ns();
	if (READ_ONCE(run_until_time) == 0 || now > READ_ONCE(run_until_time))
		return 0;

	return 0;
}

static int exit_callback(struct kretprobe_instance *ki, struct pt_regs *regs)
{
	u64 now = ktime_get_ns();
	if (READ_ONCE(run_until_time) == 0 || now > READ_ONCE(run_until_time))
		return 0;

	// Use kretprobe instance to access wrapper struct for syscall_id value
	struct kretprobe *kp = get_kretprobe(ki);
	struct probe_wrapper *pw = container_of(kp, struct probe_wrapper, krp);

	struct syscall_stats *s = this_cpu_ptr(pw->stats);
	s->count++;

	return 0;
}

static int __init mod_init(void)
{
	int ret;

	// Attach probes to beginning and end of each syscall
	for (int i = 0; i < NUM_SYSCALLS; i++) {
		probes[i].krp.kp.symbol_name = syscall_symbols[i];
		probes[i].krp.entry_handler = &entry_callback;
		probes[i].krp.handler = &exit_callback;
		probes[i].krp.data_size = sizeof(u64);

		probes[i].stats = &percpu_stats[i];

		// Store probe pointer for the registration function
		syscall_kretprobe_ptrs[i] = &(probes[i].krp);
	}

	ret = register_kretprobes(syscall_kretprobe_ptrs, NUM_SYSCALLS);
	if (ret)
		goto kretprobe_fail;

	entry = proc_create(PROCFS_FILENAME, 0, NULL, &file_ops);
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

static void __exit mod_exit(void)
{
	remove_proc_entry(PROCFS_FILENAME, NULL);
	pr_info("/proc/%s removed\n", PROCFS_FILENAME);

	unregister_kretprobes(syscall_kretprobe_ptrs, NUM_SYSCALLS);
}

module_init(mod_init);
module_exit(mod_exit);

MODULE_DESCRIPTION("Syscall tracing module for per-CPU syscall latency and count aggregation");
MODULE_AUTHOR("Orion Brown");
MODULE_LICENSE("GPL");