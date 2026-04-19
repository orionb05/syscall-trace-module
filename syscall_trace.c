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

static u64 run_start_time = 0;
static u64 run_end_time = 0;
static int run_syscall_id;

static struct proc_dir_entry *entry;

#define NUM_BUCKETS 24
static const u64 bucket_bounds[NUM_BUCKETS] = {
    1e3, 2e3, 4e3, 8e3, 16e3, 32e3, 64e3, 128e3,
    256e3, 512e3, 1e6, 2e6, 4e6, 8e6, 16e6, 32e6,
    64e6, 128e6, 256e6, 512e6, 1e9, 2e9, 4e9, U64_MAX
};

struct syscall_stats {
	u64 count;
	u64 total_latency;
	u64 max_latency;
	u64 latency_hist[NUM_BUCKETS];
};

enum syscalls_tracked { SYSCALL_READ, SYSCALL_MMAP, SYSCALL_FUTEX, SYSCALL_OPENAT, NUM_SYSCALLS };

// Each core will track syscalls independently, with global_stats aggregating the sums
DEFINE_PER_CPU(struct syscall_stats, percpu_stats[NUM_SYSCALLS]);
static struct syscall_stats global_stats[NUM_SYSCALLS];

struct probe_wrapper {
    struct kretprobe krp;
    struct syscall_stats __percpu *stats;
	int syscall_id;
};
static struct probe_wrapper probes[NUM_SYSCALLS];

struct probe_data {
    u64 entry_time;
    u8 valid;
};

static struct kretprobe *syscall_kretprobe_ptrs[NUM_SYSCALLS];

static const char *syscall_symbols[NUM_SYSCALLS] = {
	[SYSCALL_READ] = "__x64_sys_read",
	[SYSCALL_MMAP] = "__x64_sys_mmap",
	[SYSCALL_FUTEX] = "__x64_sys_futex",
	[SYSCALL_OPENAT] = "__x64_sys_openat",
};

static void *my_seq_start(struct seq_file *s, loff_t *pos)
{
    if (*pos == 0)
        return &global_stats[run_syscall_id];
    return NULL;
}

static void *my_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	return NULL;
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

	input_buffer[len] = '\0'; // Null-terminate the input string

	*off += len;

	// Collect and store user input
	char *p = input_buffer;
	char *tok;

	// Grab the syscall_id first
	tok = strsep(&p, ",");
	if (!tok)
		return -EINVAL;

	if (kstrtoint(tok, 10, &run_syscall_id))
		return -EINVAL;

	if (run_syscall_id < 0 || run_syscall_id >= NUM_SYSCALLS)
		return -EINVAL;

	// Grab the run time
	tok = strsep(&p, ",");
	if (!tok)
		return -EINVAL;

	unsigned int run_time_seconds;
	if (kstrtoint(tok, 10, &run_time_seconds))
		return -EINVAL;

	if (run_time_seconds <= 0 || run_time_seconds > 1024)
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
	u64 now = ktime_get_ns();
	WRITE_ONCE(run_start_time, now);
	WRITE_ONCE(run_end_time, now + run_time_seconds * NSEC_PER_SEC);

	pr_info("Syscall_trace starting at time: %llu\n", run_start_time);
	pr_info("Ending at time: %llu\n", run_end_time);

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
	u64 start = READ_ONCE(run_start_time);
    u64 end = READ_ONCE(run_end_time);
	u64 now = ktime_get_ns();

	if (now < start || now > end)
		return 0;

	struct kretprobe *kp = get_kretprobe(ki);
    struct probe_wrapper *pw = container_of(kp, struct probe_wrapper, krp);
	if (pw->syscall_id != READ_ONCE(run_syscall_id))
		return 0;

	struct probe_data *d = (struct probe_data *)ki->data;
	d->entry_time = now;
	d->valid = 1;

	return 0;
}

static int exit_callback(struct kretprobe_instance *ki, struct pt_regs *regs)
{
	u64 now = ktime_get_ns();
	u64 start = READ_ONCE(run_start_time);
    u64 end = READ_ONCE(run_end_time);

	// Drop syscalls that arent the focus
	struct kretprobe *kp = get_kretprobe(ki);
	struct probe_wrapper *pw = container_of(kp, struct probe_wrapper, krp);
	if (pw->syscall_id != READ_ONCE(run_syscall_id))
		return 0;

    struct probe_data *d = (struct probe_data *)ki->data;

	// Drop syscalls that skipped the entry handler
	if (!d->valid)
        return 0;

	// Drop syscalls that started before or exit after this run
	bool in_window = d->entry_time >= start && now <= end;

	// Update stats for a valid syscall
    if (in_window) {
		struct syscall_stats *s = this_cpu_ptr(pw->stats);

        u64 latency = now - d->entry_time;

        s->count++;
        s->total_latency += latency;

		if (latency > (u64)1.5e9)
			pr_info("syscall exit: pid=%d comm=%s latency=%llu ns\n", current->pid, current->comm, latency);

        s->max_latency = max(s->max_latency, latency);

		int bucket = 0;
		while (latency > bucket_bounds[bucket])
			bucket++;
		s->latency_hist[bucket]++;
    }

    // Always clear to avoid stale valid/start_time
    d->valid = 0;

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
		probes[i].krp.data_size = sizeof(struct probe_data);

		probes[i].stats = &percpu_stats[i];

		probes[i].syscall_id = i;

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