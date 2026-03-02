# Syscall Latency Tracer
A lightweight Linux kernel module and user‑space tools for measuring system call latency under different system load conditions. The module hooks selected syscalls, timestamps entry/exit, aggregates basic statistics, and exposes them through /proc for collection and analysis.

## Feature Goals
* Kernel module that instruments syscalls using tracepoints or kprobes

* High‑resolution timestamping of syscall entry/exit

* Aggregated metrics: count, avg/min/max latency, simple tail estimate

* /proc interface for reading metrics from user space

* Microbenchmark program to generate controlled syscall workloads

* Compatible with external load tools (stress-ng, fio, dd)
