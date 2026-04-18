import subprocess
import time
from enum import Enum

class StressType(Enum):
    CPU = 1
    IO = 2
    MEM = 3
    MIXED = 4

class SyscallEntry:

    def __init__(self):
        self.name = ""
        self.count = 0
        self.total_latency = 0
        self.max_latency = 0
        self.latency_hist = []

def main():

    if insert_module() != 0:
        return
    
    # TODO: get_system_info() for tailored stress calls

    start_handler()

    remove_module()

def start_handler():

    # Handler loop
    while True:

        # Collect test type from user
        print("Test types: \n1. CPU bound\n2. IO bound\n3. Memory Bound\n4. Mixed")
        while True:
            user_input = input("Enter the test number (0 to exit): ").strip()
            if user_input == "0":
                return
            try:
                stress_type = StressType(int(user_input))
                break
            except ValueError:
                print("Invalid input, please try again.")

        # Perhaps make this user-configurable
        run_time = 5
        
        # Induce load and start module collection
        if run_module(stress_type, run_time) != 0:
            break

        # Retrive syscall stats from test run
        syscall_stats = []
        if collect_stats(syscall_stats) != 0:
            break
        
        # TODO: Use a visualizer to print stats

        # DEBUGGING: Print stats directly for sanity check
        print_stats(syscall_stats)

def run_module(stress_type, duration):

    # Induce a load for the test_type
    match stress_type:
        case StressType.CPU:
            print("Running CPU test...")
            proc = subprocess.Popen(
                ["stress", "--cpu", "2", "--timeout", str(duration)],
                stdout=subprocess.DEVNULL,
            )

        case StressType.IO:
            print("Running IO test...")
            proc = subprocess.Popen(
                ["stress", "--io", "2", "--timeout", str(duration)],
                stdout=subprocess.DEVNULL,
            )

        case StressType.MEM:
            print("Running Memory test...")
            proc = subprocess.Popen(
                ["stress", "--vm", "1", "--vm-bytes", "2G", "--timeout", str(duration)],
                stdout=subprocess.DEVNULL,
            )

        case StressType.MIXED:
            print("Running Mixed test...")
            proc = subprocess.Popen(
                [
                    "stress",
                    "--cpu", "1",
                    "--io", "1",
                    "--vm", "1",
                    "--vm-bytes", "2G",
                    "--timeout", str(duration)
                ],
                stdout=subprocess.DEVNULL,
            )

    # Notify module to start collecting
    try:
        with open("/proc/syscall-trace", "w") as f:
            f.write(str(duration))
    except FileNotFoundError:
        print("File not found")
        return 1
    
    time.sleep(duration)
    proc.wait()

    print("Test complete.\n")

    return 0

def collect_stats(syscall_stats):

    try:
        with open("/proc/syscall-trace", "r") as file:
            while True:
                syscall = SyscallEntry()

                line = file.readline().strip()
                if not line:
                    break

                prefix = "__x64_sys_"
                syscall.name = line[len(prefix):].strip() if line.startswith(prefix) else line

                count_line = file.readline()
                total_line = file.readline()
                max_line = file.readline()

                if not (count_line and total_line and max_line):
                    return 1

                syscall.count = int(count_line.strip())
                syscall.total_latency = int(total_line.strip())
                syscall.max_latency = int(max_line.strip())

                # Histogram (still optional)
                # hist_line = file.readline().strip()
                # syscall.latency_hist = [int(x) for x in hist_line.split()]

                syscall_stats.append(syscall)

    except FileNotFoundError:
        print("File not found")
        return 1

    return 0

def insert_module():

    try:
        print("Cleaning project directory...\n")
        subprocess.run(["make", "clean"], check=True)

        print("Regenerating linux module..\n")
        subprocess.run(["make"], check=True)
        
        print("Checking if old module is already loaded...\n")
        if remove_module() != 0:
            return 1

        print("Inserting module...\n")
        subprocess.run(["insmod", "syscall_trace.ko"], check=True)

        print("Module inserted!\n")

    except subprocess.CalledProcessError:
        print("Module insertion failed.")
        remove_module()
        return 1
    
    return 0

def remove_module():

    try:
        res = subprocess.run(["grep", "syscall_trace", "/proc/modules"], stdout=subprocess.PIPE)
        if res.stdout.split():
            subprocess.run(["rmmod", "syscall_trace.ko"], check=True)
            print("Module has been removed.")

    except subprocess.CalledProcessError:
        print("Module removal failed.")
        return 1
    
    return 0

def print_stats(syscall_stats):

    for stat in syscall_stats:
        print(
            f"{stat.name}\n"
            f"{stat.count} {stat.total_latency} {stat.max_latency}"
        )

if __name__ == "__main__":
    main()