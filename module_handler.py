import subprocess
import time
from enum import Enum
import psutil

DURATION = 8

class SyscallSymbols(Enum):
    READ = 0
    MMAP = 1
    FUTEX = 2
    OPENAT = 3

class SyscallEntry:
    def __init__(self):
        self.intensity = 0
        self.symbol = ""
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

        # Collect syscall of interest from user
        print("Syscall to test: \n0. read\n1. mmap\n2. futex\n3. openat")
        while True:
            user_input = input("Enter the syscall number (e to exit): ").strip()
            if user_input == "e":
                return
            try:
                syscall = SyscallSymbols(int(user_input))
                break
            except ValueError:
                print("Invalid input, please try again.")

        results = []

        # Run module with varying stress levels
        for intensity_level in range(3):
            try:
                run_with_load(syscall, intensity_level)
                entry = collect_stats()
                entry.intensity = intensity_level
                results.append(entry)
            except Exception as e:
                raise RuntimeError(f"Failed during module test (level {intensity_level})") from e
            
        # TODO: Use a visualizer to print stats

        print_results(results)

def induce_load(intensity_level):

    # Collect system info for dynamic and safe stresses
    cores = psutil.cpu_count(logical=True)
    ram_gb = psutil.virtual_memory().total // (1024 ** 3)

    # Induce a stress load based on the intesnity
    if intensity_level == 2:
        print("Inducing high-intensity stress...")
        cores = max(1, int(cores))
        ram_gb = max(1, int(ram_gb * 0.7))
    elif intensity_level == 1:
        print("Inducing medium-intensity stress...")
        cores = max(1, cores // 2)
        ram_gb = max(1, int(ram_gb * 0.4))
    else:
        print("Inducing low-intensity (no stress)...")
        return None
    
    proc = subprocess.Popen(
        [
            "stress",
            "--cpu", str(cores),
            "--io", str(cores),
            "--vm", "1",
            "--vm-bytes", str(ram_gb)+"G",
            "--timeout", str(DURATION + 1)
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )

    time.sleep(1)  # let stress ramp up

    return proc

def hammer_syscall(syscall):

    # Activate the syscall repeatedly
    match syscall:
        case SyscallSymbols.READ:
            print("Inducing read() syscall activity...")
            proc = subprocess.Popen(
                ["bash", "-c", f"timeout {DURATION}s bash -c 'while :; do dd if=/dev/zero of=/dev/null bs=4K count=1 status=none; done'"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

        case SyscallSymbols.MMAP:
            print("Inducing mmap() syscall activity...")
            proc = subprocess.Popen(
                ["bash", "-c", f"timeout {DURATION}s bash -c 'while :; do dd if=/dev/zero of=/tmp/mmap_test bs=4K count=1 status=none; rm -f /tmp/mmap_test; done'"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

        case SyscallSymbols.FUTEX:
            print("Inducing futex() syscall activity...")
            proc = subprocess.Popen(
                ["bash", "-c", f"timeout {DURATION}s bash -c 'python3 - <<\"EOF\"\nimport threading, time\nlock = threading.Lock()\nend = time.time() + {DURATION}\nwhile time.time() < end:\n    with lock:\n        pass\nEOF'"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

        case SyscallSymbols.OPENAT:
            print("Inducing openat() syscall activity...")
            proc = subprocess.Popen(
                ["bash", "-c", f"timeout {DURATION}s bash -c 'while :; do fd=$(mktemp); cat /dev/null > \"$fd\"; rm -f \"$fd\"; done'"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

    return proc

def run_with_load(syscall, intensity_level):

    # Start stress first for buildup then repeatedly activate the syscall
    stress_proc = induce_load(intensity_level)
    hammer_proc = hammer_syscall(syscall)

    try:
        run_module(syscall)
        # Wait for both subprocesses if they exist
        for proc in (stress_proc, hammer_proc):
            if proc:
                proc.wait()
    finally:
        # Ensure cleanup even if an exception occurs
        for proc in (stress_proc, hammer_proc):
            if proc and proc.poll() is None:
                proc.terminate()

def run_module(syscall):
    # Notify module to start collecting
    print("Setup complete, enabling module to start collection...\n")

    try:
        with open("/proc/syscall-trace", "w") as f:
            f.write(f"{syscall.value},{DURATION}")
    except FileNotFoundError:
        raise RuntimeError("syscall-trace proc entry missing")

    time.sleep(DURATION)
    print("Module has finished.\n")


def collect_stats():

    try:
        with open("/proc/syscall-trace", "r") as file:
            entry = SyscallEntry()

            line = file.readline().strip()
            if not line:
                return

            prefix = "__x64_sys_"
            entry.symbol = line[len(prefix):].strip() if line.startswith(prefix) else line

            count_line = file.readline()
            total_line = file.readline()
            max_line = file.readline()

            if not (count_line and total_line and max_line):
                raise RuntimeError("misformatted module stat output")

            entry.count = int(count_line.strip())
            entry.total_latency = int(total_line.strip())
            entry.max_latency = int(max_line.strip())

            hist_line = file.readline().strip()
            entry.latency_hist = [int(x) for x in hist_line.split()]

    except FileNotFoundError:
        raise RuntimeError("syscall-trace proc entry missing")
    
    return entry

def insert_module():

    try:
        print("Cleaning project directory...\n")
        subprocess.run(["make", "clean"], check=True, stdout=subprocess.DEVNULL,)

        print("Regenerating linux module..\n")
        subprocess.run(["make"], check=True, stdout=subprocess.DEVNULL,)
        
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
            subprocess.run(["rmmod", "syscall_trace.ko"], check=True, stdout=subprocess.DEVNULL)
            print("Module has been removed.")

    except Exception as e:
        print("Error:", e)
        raise RuntimeError("failed to remove module")
    
    return 0

def print_results(results):

    print(f"Printing results for the {results[0].symbol} syscall\n.")

    for entry in results:
        print(f"For intensity level {entry.intensity}:")
        print(f"Count: {entry.count}. Total Latency: {entry.total_latency}. Max Latency: {entry.max_latency}.")
        print("Histogram:")
        print(" ".join(str(v) for v in entry.latency_hist))
        print()

if __name__ == "__main__":
    main()