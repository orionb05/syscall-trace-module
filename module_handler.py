class SyscallEntry:
    def __init__(self):
        self.name = ""
        self.count = 0
        self.total_latency = 0
        self.max_latency = 0
        self.latency_hist = []

def main():
    syscall_stats = []

    try:
        with open("/proc/syscall-trace", "r") as file:
            collect_stats(file, syscall_stats)
    except FileNotFoundError:
        print("File not found")
        return

    print_stats(syscall_stats)

def collect_stats(file, syscall_stats):

    while True:
        syscall = SyscallEntry()

        # Read syscall name line
        line = file.readline().strip()
        if not line:
            break

        # Remove the prefix if it exists
        prefix = "__x64_sys_"
        if line.startswith(prefix):
            syscall.name = line[len(prefix):].strip()
        else:
            syscall.name = line.strip()

        # Read simple stats
        count_line = file.readline()
        total_line = file.readline()
        max_line = file.readline()

        if not (count_line and total_line and max_line):
            break  # malformed or EOF

        syscall.count = int(count_line.strip())
        syscall.total_latency = int(total_line.strip())
        syscall.max_latency = int(max_line.strip())

        # Histogram (still optional)
        # hist_line = file.readline().strip()
        # syscall.latency_hist = [int(x) for x in hist_line.split()]

        syscall_stats.append(syscall)

def print_stats(syscall_stats):
    for stat in syscall_stats:
        print(
            f"{stat.name}\n"
            f"{stat.count} {stat.total_latency} {stat.max_latency}"
        )

if __name__ == "__main__":
    main()