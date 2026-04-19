import subprocess
import time
from enum import Enum
import psutil

DURATION = 5

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
    try:
        insert_module()
        start_handler()
        remove_module()
    except Exception as e:
        print("Error:", e)
        return

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

        results = []

        # Run module with varying stress levels
        for intensity_level in range(3):
            try:
                stress_proc = induce_load(stress_type, intensity_level)
                run_module() 

                if stress_proc is not None:
                    stress_proc.wait()
                    
                syscall_stats = collect_stats()
            except Exception as e:
                if stress_proc:
                    stress_proc.terminate()
                raise RuntimeError("failed during module test") from e
            
            results.append(syscall_stats)
            print_stats(syscall_stats)
            
        # TODO: Use a visualizer to print stats

def induce_load(stress_type, intensity_level):

    stress_duration = DURATION + 1

    # Collect system info for dynamic and safe stresses
    cores = psutil.cpu_count(logical=True)
    ram_gb = psutil.virtual_memory().total // (1024 ** 3)

    if intensity_level == 2:
        print("High-intensity run")
        cores = max(1, int(cores))
        ram_gb = max(1, int(ram_gb * 0.6))
    elif intensity_level == 1:
        print("Medium-intensity run")
        cores = max(1, cores // 2)
        ram_gb = max(1, int(ram_gb * 0.3))
    else:
        print("Baseline run (no stress)")
        return None

    # Induce a stress load for the test_type
    match stress_type:
        case StressType.CPU:
            print("Running CPU test...")
            proc = subprocess.Popen(
                ["stress", "--cpu", str(cores), "--timeout", str(stress_duration)],
                stdout=subprocess.DEVNULL,
            )

        case StressType.IO:
            print("Running IO test...")
            proc = subprocess.Popen(
                ["stress", "--io", str(cores), "--timeout", str(stress_duration)],
                stdout=subprocess.DEVNULL,
            )

        case StressType.MEM:
            print("Running Memory test...")
            proc = subprocess.Popen(
                ["stress", "--vm", "1", "--vm-bytes", str(ram_gb)+"G", "--timeout", str(stress_duration)],
                stdout=subprocess.DEVNULL,
            )

        case StressType.MIXED:
            print("Running Mixed test...")
            proc = subprocess.Popen(
                [
                    "stress",
                    "--cpu", str(cores),
                    "--io", str(cores),
                    "--vm", "1",
                    "--vm-bytes", str(ram_gb)+"G",
                    "--timeout", str(stress_duration)
                ],
                stdout=subprocess.DEVNULL,
            )

    return proc

def run_module():

    time.sleep(1)  # let stress ramp up

    # Notify module to start collecting
    try:
        with open("/proc/syscall-trace", "w") as f:
            f.write(str(DURATION))
    except FileNotFoundError:
        raise RuntimeError("syscall-trace proc entry missing")

    time.sleep(DURATION)
    print("Test complete.\n")


def collect_stats():

    stats = []
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
                    raise RuntimeError("misformatted module stat output")

                syscall.count = int(count_line.strip())
                syscall.total_latency = int(total_line.strip())
                syscall.max_latency = int(max_line.strip())

                hist_line = file.readline().strip()
                syscall.latency_hist = [int(x) for x in hist_line.split()]

                stats.append(syscall)

    except FileNotFoundError:
        raise RuntimeError("syscall-trace proc entry missing")
    
    return stats

def insert_module():

    try:
        print("Cleaning project directory...\n")
        subprocess.run(["make", "clean"], check=True)

        print("Regenerating linux module..\n")
        subprocess.run(["make"], check=True)
        
        print("Checking if old module is already loaded...\n")
        remove_module()

        print("Inserting module...\n")
        subprocess.run(["insmod", "syscall_trace.ko"], check=True)

        print("Module inserted!\n")

    except Exception as e:
        print("Error:", e)
        try:
            remove_module()
        except Exception as cleanup_err:
            print("Cleanup error:", cleanup_err)
        raise RuntimeError("failed to insert module") from e

def remove_module():

    try:
        res = subprocess.run(["grep", "syscall_trace", "/proc/modules"], stdout=subprocess.PIPE)
        if res.stdout.split():
            subprocess.run(["rmmod", "syscall_trace.ko"], check=True)
            print("Module has been removed.")

    except Exception as e:
        print("Error:", e)
        raise RuntimeError("failed to remove module")
    
    return 0

def print_stats(syscall_stats):

    for stat in syscall_stats:
        print(f"{stat.name}")
        print(f"{stat.count} {stat.total_latency} {stat.max_latency}")
        print("Histogram:")
        print(" ".join(str(v) for v in stat.latency_hist))
        print()

if __name__ == "__main__":
    main()