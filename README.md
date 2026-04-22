# Syscall Latency Tracer

A Linux kernel module that measures system call latency under varying system load. The module instruments four critical syscalls (read, mmap, futex, openat), maintains per-CPU statistics with latency histograms, and exposes results through `/proc/syscall-trace`.

## Overview

**Kernel Module**: Traces four syscalls using kretprobes, records nanosecond-precision entry/exit timestamps, and maintains per-CPU latency statistics.

**Test Harness**: Automates module installation, generates syscall loads at three stress levels (baseline, medium, high), collects statistics, and produces comparison graphs.

**Syscall Generators**: Custom programs that repeatedly invoke target syscalls to generate predictable workloads.

## Building

### Prerequisites

- Linux kernel headers
- `make` and `gcc`
- Python 3.x with `psutil`, `matplotlib`, `numpy`
- `stress` tool for load generation
- sudo/root access

### Compile

```bash
make clean
make
```

Outputs: `syscall_trace.ko`, `generators/sysgen_*`

## Usage

### Quick Start

```bash
sudo python3 module_handler.py
```

Follow the prompts to:
1. Select a syscall (0: read, 1: mmap, 2: futex, 3: openat)
2. Run tests at three stress levels
3. View graphs and results in `results/`

### Manual Testing

```bash
# Insert module
sudo insmod syscall_trace.ko

# Start collection: syscall_id,duration_seconds,generator_pid
echo "0,8,1234" > /proc/syscall-trace  # Trace read() for 8s from PID 1234
echo "1,8,0" > /proc/syscall-trace     # Trace mmap() for 8s from any process

# Read results
cat /proc/syscall-trace

# Remove module
sudo rmmod syscall_trace.ko
```

## Output

### Results Directory

- **results.log** - Test results with statistics for each stress level
- **avg_and_max.png** - Average and maximum latency comparison
- **histograms.png** - Latency histograms across stress levels

### Statistics Format

```
read                    # Syscall name
12345                   # Call count
987654321               # Total latency (ns)
56789                   # Maximum latency (ns)
100 200 150 ... 50      # Histogram: count in 16 latency buckets
```

### Latency Buckets

Buckets represent latency thresholds in nanoseconds:
```
1e3, 2e3, 4e3, 8e3, 16e3, 32e3, 64e3, 128e3,
256e3, 512e3, 1e6, 2e6, 4e6, 8e6, 16e6, U64_MAX
```

Each bucket shows the count of syscalls with latency ≤ that threshold.

## How It Works

The kernel module uses **kretprobes** to hook syscall entry and exit:

- **Entry handler**: Records timestamp if conditions match (target syscall, PID filter, tracing window)
- **Exit handler**: Calculates latency, updates statistics (count, total, max, histogram bucket)
- **Per-CPU stats**: Each core tracks independently to avoid lock contention
- **Aggregation**: On `/proc` read, results from all cores are summed

## Debugging

View kernel messages:

```bash
dmesg | tail -20
```

The module logs:
- Module insertion/removal
- Collection window times
- Errors during probe registration

## Extending

### Add a New Syscall

1. Update `enum syscalls_tracked` in `syscall_trace.c`
2. Add symbol to `syscall_symbols[]` (use `__x64_sys_*` naming)
3. Create generator in `generators/`
4. Update `SyscallSymbols` enum and `generate_syscalls()` in `module_handler.py`
5. Rebuild: `make clean && make`

## System Requirements

- Linux 4.x+ with kprobes support
- x86-64 architecture
- Kernel headers installed

## License

GPL

## Author

Orion Brown

