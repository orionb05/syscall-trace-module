/*
 * syscall_trace.c
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/seq_file.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
#include <linux/minmax.h>
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
#define HAVE_PROC_OPS
#endif

#define PROCFS_FILENAME "syscall-trace"

#define INPUT_MAX_LEN 33
static char input_buffer[INPUT_MAX_LEN];   

u64 run_until_time = 0;

static struct proc_dir_entry *entry;

#define NUM_BUCKETS 64

struct syscall_stats {
	u64 count;
	u64 total_latency;
	u64 max_latency;
	u64 latency_hist[NUM_BUCKETS];
};

enum syscalls_tracked {
	SYSCALL_READ,
	SYSCALL_WRITE,
	SYSCALL_GETPID,
	NUM_SYSCALLS
};

static struct syscall_stats stats[NUM_SYSCALLS];

static char *syscall_names[NUM_SYSCALLS] = {
	[SYSCALL_READ] = "read",
	[SYSCALL_WRITE] = "write",
	[SYSCALL_GETPID] = "getpid",
};

static void *my_seq_start(struct seq_file *s, loff_t *pos)
{
	// Each sequence prints one syscall's stats
	if (*pos < NUM_SYSCALLS) {
		return &(stats[*pos]);
	}

	*pos = 0;
	return NULL;
}

static void *my_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	(*pos)++;
	if(*pos >= NUM_SYSCALLS){
		return NULL;
	}

	return &(stats[*pos]);
}

static void my_seq_stop(struct seq_file *s, void *v)
{
	/* nothing to do, we use a static value in start() */
}

static int my_seq_show(struct seq_file *s, void *v)
{
	struct syscall_stats *stat = (struct syscall_stats *)v;

	seq_printf(s, "%s:\n", syscall_names[stat - stats]);
	seq_printf(s, "%Ld\n", stat->count);
	seq_printf(s, "%Ld\n", stat->total_latency);
	seq_printf(s, "%Ld\n", stat->max_latency);

	return 0;
}

static struct seq_operations my_seq_ops = {
	.start = my_seq_start,
	.next = my_seq_next,
	.stop = my_seq_stop,
	.show = my_seq_show,
};

static int my_seq_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &my_seq_ops);
};

static ssize_t my_proc_write(struct file *file, const char __user *buff,
                              size_t len, loff_t *off)
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

	// TODO: Clean syscall stats

	// Signal K-probe collection to run for the specified time
	WRITE_ONCE(run_until_time,
           ktime_get_ns() + run_time_seconds * NSEC_PER_SEC);

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

static int __init mod_init(void)
{
	entry = proc_create(PROCFS_FILENAME, 0, NULL, &file_ops);

	if (entry == NULL) {
		pr_info("Error: Could not initialize /proc/%s\n",
			 PROCFS_FILENAME);
		return -ENOMEM;
	}

	proc_set_user(entry, GLOBAL_ROOT_UID, GLOBAL_ROOT_GID);

	pr_info("/proc/%s created\n", PROCFS_FILENAME);
	return 0;
}

static void __exit mod_exit(void)
{
	remove_proc_entry(PROCFS_FILENAME, NULL);
	pr_info("/proc/%s removed\n", PROCFS_FILENAME);
}

module_init(mod_init);
module_exit(mod_exit);

MODULE_LICENSE("GPL");