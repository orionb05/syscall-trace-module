#!/usr/bin/env python3
#
# module_handler.py
#
# Test harness for syscall_trace kernel module. Manages module installation,
# generates syscall load at varying stress levels, collects statistics, and
# produces graphs and logs for analysis.

import subprocess
import time
from enum import Enum
from pathlib import Path

import psutil
import matplotlib.pyplot as plt
import numpy as np

# Test configuration
DURATION = 8
PROJECT_ROOT = Path(__file__).resolve().parent
RESULTS_DIR = PROJECT_ROOT / "results"
RESULTS_DIR.mkdir(exist_ok=True)

# System calls tracked by the kernel module
class SyscallSymbols(Enum):
    READ = 0
    MMAP = 1
    FUTEX = 2
    OPENAT = 3

# Latency histogram bucket boundaries (nanoseconds)
bucket_bounds = [
    "1e3", "2e3", "4e3", "8e3", "16e3",
    "32e3", "64e3", "128e3", "256e3", "512e3",
    "1e6", "2e6", "4e6", "8e6", "16e6", "32e6"
]

# Data structure to hold syscall statistics from module
class SyscallEntry:
    def __init__(self):
        self.intensity = 0
        self.symbol = ""
        self.count = 0
        self.total_latency = 0
        self.avg_latency = 0
        self.max_latency = 0
        self.latency_hist = []



# Main entry point
def main():
    try:
        insert_module()
        start_handler()
        remove_module()
    except Exception as e:
        print("Error:", e)
        return


# Interactive testing loop
def start_handler():
    # User-controlled handler loop
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

        # Run module with varying stress levels (0, 1, 2)
        for intensity_level in range(3):
            try:
                run_with_load(syscall, intensity_level)
                entry = collect_stats()
                entry.intensity = intensity_level
                results.append(entry)
            except Exception as e:
                raise RuntimeError(f"Failed during module test (level {intensity_level})") from e

        print_results(results)
        graph_results(results)



# Load and stress generation
def induce_load(intensity_level):
    # Retrieve system capabilities for safe stress scaling
    cores = psutil.cpu_count(logical=True)
    ram_gb = psutil.virtual_memory().total // (1024 ** 3)

    # Scale stress parameters based on intensity level
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
    
    # Launch stress process
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

    time.sleep(1)  # Allow stress to ramp up

    return proc


def generate_syscalls(syscall):
    # Launch syscall generator for the target syscall
    match syscall:
        case SyscallSymbols.READ:
            print("Generating read() syscalls...")
            return subprocess.Popen(["./generators/sysgen_read", str(DURATION)])

        case SyscallSymbols.MMAP:
            print("Generating mmap() syscalls...")
            return subprocess.Popen(["./generators/sysgen_mmap", str(DURATION)])

        case SyscallSymbols.FUTEX:
            print("Generating futex() syscalls...")
            return subprocess.Popen(["./generators/sysgen_futex", str(DURATION)])

        case SyscallSymbols.OPENAT:
            print("Generating openat() syscalls...")
            return subprocess.Popen(["./generators/sysgen_openat", str(DURATION)])


def run_with_load(syscall, intensity_level):
    # Coordinate stress process and syscall generator
    stress_proc = induce_load(intensity_level)
    generator_proc = generate_syscalls(syscall)

    try:
        run_module(syscall, generator_proc.pid)
        for proc in (stress_proc, generator_proc):
            if proc:
                proc.wait()
    finally:
        # Ensure cleanup even if an exception occurs
        for proc in (stress_proc, generator_proc):
            if proc and proc.poll() is None:
                proc.terminate()


# Module communication
def run_module(syscall, generator_pid):
    # Signal module to start collection with target syscall and generator PID
    print("Setup complete, enabling module to start collection...\n")

    try:
        with open("/proc/syscall-trace", "w") as f:
            f.write(f"{syscall.value},{DURATION},{generator_pid}")
    except FileNotFoundError:
        raise RuntimeError("syscall-trace proc entry missing")

    time.sleep(DURATION)
    print("Module has finished.\n")


# Statistics collection
def collect_stats():
    # Read aggregated statistics from kernel module
    try:
        with open("/proc/syscall-trace", "r") as file:
            entry = SyscallEntry()

            # Parse syscall symbol name
            line = file.readline().strip()
            if not line:
                return

            prefix = "__x64_sys_"
            entry.symbol = line[len(prefix):].strip() if line.startswith(prefix) else line

            # Parse count, latency totals
            count_line = file.readline()
            total_line = file.readline()
            max_line = file.readline()

            if not (count_line and total_line and max_line):
                raise RuntimeError("misformatted module stat output")

            entry.count = int(count_line.strip())
            entry.total_latency = int(total_line.strip())
            entry.max_latency = int(max_line.strip())

            # Calculate average latency
            entry.avg_latency = entry.total_latency // entry.count if entry.count else 0

            # Parse histogram data
            hist_line = file.readline().strip()
            entry.latency_hist = [int(x) for x in hist_line.split()]

    except FileNotFoundError:
        raise RuntimeError("syscall-trace proc entry missing")
    
    return entry



# Module management
def insert_module():
    # Build and install the kernel module
    try:
        print("Cleaning project directory...\n")
        subprocess.run(["make", "clean"], check=True, stdout=subprocess.DEVNULL,)

        print("Regenerating linux module..\n")
        subprocess.run(["make"], check=True, stdout=subprocess.DEVNULL,)
        
        print("Checking if old module is already loaded...\n")
        remove_module()

        print("Inserting module...\n")
        subprocess.run(["sudo", "insmod", "syscall_trace.ko"], check=True)

        print("Module inserted!\n")

    except Exception as e:
        print("Error:", e)
        try:
            remove_module()
        except Exception as cleanup_err:
            print("Cleanup error:", cleanup_err)
        raise RuntimeError("failed to insert module") from e


def remove_module():
    # Unload the kernel module if present
    try:
        res = subprocess.run(["grep", "syscall_trace", "/proc/modules"], stdout=subprocess.PIPE)
        if res.stdout.split():
            subprocess.run(["sudo", "rmmod", "syscall_trace.ko"], check=True, stdout=subprocess.DEVNULL)
            print("Module has been removed.")

    except Exception as e:
        print("Error:", e)
        raise RuntimeError("failed to remove module")
    
    return 0



# Output and visualization
def graph_results(results):
    # Generate and save comparison graphs
    plot_avg_and_max(results)
    plt.savefig(f"{RESULTS_DIR}/avg_and_max.png", dpi=150)

    plot_histograms(results)
    plt.savefig(f"{RESULTS_DIR}/histograms.png", dpi=150)

    plt.show()


def plot_histograms(entries):
    # Create histogram comparison across stress levels
    num_buckets = len(bucket_bounds)
    x = np.arange(num_buckets)
    width = 0.25

    fig, ax = plt.subplots(figsize=(14, 6))

    # Plot histogram for each intensity level
    for i, entry in enumerate(entries):
        ax.bar(
            x + i * width,
            entry.latency_hist,
            width,
            label=f"Intensity {entry.intensity}",
        )

    ax.set_xticks(x + width)
    ax.set_xticklabels(bucket_bounds, rotation=45, ha="right")

    ax.set_yscale("log")

    ax.set_xlabel("Latency Bucket")
    ax.set_ylabel("Count (log scale)")
    ax.set_title(f"Latency Histogram Comparison for {entries[0].symbol}")
    ax.legend()
    fig.tight_layout()


def plot_avg_and_max(results):
    # Create comparison plots for average and maximum latencies
    intensities = [entry.intensity for entry in results]
    avg_latencies = [entry.avg_latency for entry in results]
    max_latencies = [entry.max_latency for entry in results]

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8), sharex=True)

    ax1.set_title(f"Latency for {results[0].symbol} syscall")

    # Plot average latencies
    ax1.plot(intensities, avg_latencies, marker="o", color="blue")
    ax2.set_xlabel("Intensity Level")
    ax1.set_ylabel("Average Latency (ns)")
    ax1.set_xticks(intensities)

    # Plot maximum latencies on log scale
    ax2.plot(intensities, max_latencies, marker="o", color="red")
    ax2.set_xlabel("Intensity Level")
    ax2.set_ylabel("Max Latency (ns)")
    ax2.set_xticks(intensities)
    ax2.set_yscale("log")

    # Annotate maximum points with values
    for x, y in zip(intensities, max_latencies):
        ax2.text(
            x, y * 1.1,
            f"{y:.2e}",
            ha="center", va="bottom",
            fontsize=9, color="red"
        )

    fig.tight_layout()


def print_results(results):
    # Print results to console and log file
    log_path = RESULTS_DIR / "results.log"

    lines = []
    lines.append(f"Printing results for the {results[0].symbol} syscall.\n")

    # Format results for each intensity level
    for entry in results:
        lines.append(f"For intensity level {entry.intensity}:")
        lines.append(
            f"Latency Count: {entry.count}. Total: {entry.total_latency}. "
            f"Average: {entry.avg_latency}. Max: {entry.max_latency}.")
        lines.append("Histogram:")
        lines.append(" | ".join(f"{count} <{label}" for count, label in zip(entry.latency_hist, bucket_bounds)))
        lines.append("")

    # Print to stdout
    for line in lines:
        print(line)

    # Append to log file
    with open(log_path, "a") as f:
        for line in lines:
            f.write(line + "\n")


if __name__ == "__main__":
    main()