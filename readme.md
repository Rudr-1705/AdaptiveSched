# AdaptiveSched++

A production-grade adaptive CPU scheduling simulator implemented in C++17, with a Python benchmark runner and an interactive HTML dashboard for visualising and comparing scheduler performance.

AdaptiveSched++ simulates a single-core CPU scheduler that **dynamically switches between scheduling algorithms at runtime** based on observed workload characteristics. The codebase is modular, thread-safe, and architecturally faithful to real operating system scheduling frameworks.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Scheduling Algorithms](#scheduling-algorithms)
3. [Adaptive Scheduler Logic](#adaptive-scheduler-logic)
4. [Quick Start — Run From Scratch](#quick-start--run-from-scratch)
5. [Running the Simulator](#running-the-simulator)
6. [Benchmark & Dashboard](#benchmark--dashboard)
7. [Benchmark Results & Analysis](#benchmark-results--analysis)
8. [Output & Metrics](#output--metrics)
9. [Design Decisions](#design-decisions)

---

## Architecture Overview

```
AdaptiveSched++/
├── include/
│   ├── core/
│   │   ├── process.h               # PCB: all per-process state
│   │   ├── state.h                 # ProcessState / WorkloadType / SchedulerPolicy enums
│   │   ├── clock.h                 # Deterministic logical tick clock (singleton)
│   │   └── ready_queue.h           # Thread-safe, multi-order blocking queue
│   ├── scheduler/
│   │   ├── scheduler.h             # IScheduler abstract base + SchedulerStats
│   │   ├── fcfs.h                  # First-Come First-Served
│   │   ├── sjf.h                   # SJF (non-preemptive) + SRTF (preemptive)
│   │   ├── rr.h                    # Round-Robin with configurable quantum
│   │   ├── priority.h              # Preemptive Priority + aging
│   │   ├── mlfq.h                  # Multi-Level Feedback Queue (4 levels)
│   │   └── adaptive.h              # Adaptive meta-scheduler (main highlight)
│   ├── execution/
│   │   └── cpu.h                   # Single-core CPU execution engine
│   ├── metrics/
│   │   └── statistics.h            # Metrics aggregation & reporting
│   └── simulation/
│       ├── process_generator.h     # Static and dynamic process injection
│       ├── simulation_controller.h # Top-level orchestrator
│       └── workload.h              # WorkloadProfile, WorkloadLoader, WorkloadFactory
├── src/                            # Corresponding .cpp implementations
├── workloads/                      # Pre-built workload profiles
├── output/                         # Generated metrics CSV and dashboard HTML
├── benchmark.py                    # Large-scale benchmark runner
├── generate_dashboard.py           # Interactive HTML dashboard generator
├── main.cpp                        # CLI entry point
├── CMakeLists.txt
└── README.md
```

### Thread Model

The simulator runs three concurrent threads:

| Thread | Responsibility |
|--------|---------------|
| **CPU thread** | Executes processes tick-by-tick, advances clock, handles preemption |
| **Generator thread** | Injects processes into the scheduler at their arrival ticks |
| **Monitor thread** | Calls `evaluate_and_adapt()` periodically; detects termination |

All shared state is protected by `std::mutex` and `std::condition_variable`. No busy-waiting. Graceful shutdown drains in-flight processes before stopping.

---

## Scheduling Algorithms

### FCFS — First-Come, First-Served
Non-preemptive FIFO. Processes run to completion once selected. The **convoy effect** is observable in metrics when long jobs precede short ones. Best for batch workloads with homogeneous burst lengths.

### SJF / SRTF — Shortest Job First
- **SJF (non-preemptive):** selects the process with the shortest original burst at each dispatch point.
- **SRTF (preemptive):** preempts the running process when a newly arrived process has a shorter *remaining* time. Optimal for minimising average waiting time but risks starvation of long jobs.

### Round-Robin
Configurable time quantum (default: 4 ticks). Each process gets exactly one quantum before being re-enqueued at the tail. Context-switch overhead (1 tick) is charged per switch. Guarantees bounded response time and strong fairness.

### Priority Scheduling (Preemptive)
Processes ordered by priority number (lower = more urgent). When a higher-priority process arrives, the running process is immediately preempted. **Aging** prevents starvation: processes waiting longer than `AGING_THRESHOLD` ticks receive a +1 priority boost (up to `MAX_PRIORITY_BOOSTS`).

### MLFQ — Multi-Level Feedback Queue
Four queues with descending priority and increasing quanta:

| Level | Order | Quantum |
|-------|-------|---------|
| 0 (highest) | FIFO | 2 ticks |
| 1 | FIFO | 4 ticks |
| 2 | FIFO | 8 ticks |
| 3 (lowest) | FIFO | run-to-completion |

- New processes enter at Level 0.
- Exhausting a quantum causes **demotion** to the next level.
- A higher-level process arriving **preempts** a lower-level running process.
- Every 100 ticks a **global priority boost** resets all processes to Level 0 (anti-starvation).

---

## Adaptive Scheduler Logic

The `AdaptiveScheduler` wraps FCFS, RR, Priority, and MLFQ. It continuously monitors runtime metrics and switches the active policy using a five-step pipeline:

### Step 1 — Sample Metrics (EMA)
Every `EVALUATION_INTERVAL` (20) ticks the engine samples:
- CPU utilisation, throughput, context-switch rate
- Average burst length, waiting time, response time
- Starvation pressure, interactive fraction, queue depth
- MLFQ level-0 occupancy fraction

All values are smoothed using **Exponential Moving Averages** (`α = 0.25`) to filter noise.

### Step 2 — Classify Workload
The metric vector is mapped to one of five workload types:

| Classification | Key Indicators |
|---|---|
| `CPU_BOUND` | High avg burst (>12), high utilisation, few context switches |
| `INTERACTIVE` | High interactive fraction (>40%), low response time, MLFQ L0 heavy |
| `BATCH` | Very long bursts (>20), low throughput, minimal switching |
| `STRESS` | CPU_BOUND AND utilisation > 85% |
| `MIXED` | No dominant signal, OR starvation detected |

### Step 3 — Score Policies
Each candidate policy receives a confidence score `[0, 1]` based on:
- Workload classification match
- Individual metric thresholds (starvation pressure, response time, queue depth)
- Historical trend analysis over the last 8 evaluations
- A small stability bonus for the currently active policy

### Step 4 — Hysteresis
A switch only fires when:
1. The best policy differs from the current one.
2. The best policy's score exceeds the current policy's score by at least `CONFIDENCE_HYSTERESIS = 0.15`.
3. At least `COOLDOWN_TICKS = 40` ticks have elapsed since the last switch.

This three-part guard prevents oscillation and thrashing.

### Step 5 — Migrate
The active scheduler's ready queue is drained and handed off to the new scheduler. Processes preserve their `remaining_time`, `priority`, and `queue_level` through the migration.

---

## Quick Start — Run From Scratch

> **These are the exact steps for anyone who has just cloned this repo on a Windows machine with WSL installed. No prior setup assumed.**

### Prerequisites — Install WSL (if you don't have it)

Open PowerShell as Administrator and run:
```powershell
wsl --install -d Ubuntu
```
Restart your machine after this completes, then open the Ubuntu app to finish setup.

---

### Step 1 — Open WSL and navigate to the project

Open PowerShell, enter WSL, then go to your project folder:

```bash
wsl
cd /mnt/c/Users/<YourUsername>/path/to/AdaptiveSched++
```

> If your WSL uses `/mnt/host/c/` instead of `/mnt/c/`, use that prefix. Run `ls` to confirm you can see `main.cpp`, `benchmark.py`, `CMakeLists.txt`.

---

### Step 2 — Install build tools (one time only)

```bash
sudo apt update
sudo apt install -y cmake g++ build-essential python3 python3-pip
pip3 install plotly
```

> If `sudo` is not found, you are already root — just drop `sudo` from every command.

---

### Step 3 — Build the C++ simulator

```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4
cd ..
```

Verify the binary was created:
```bash
./build/adaptive_sched --help
```

---

### Step 4 — Run the benchmark

```bash
python3 benchmark.py --count 1000
```

Expected output:
```
[1/4] Generating large workload ...
  Generated 1000 processes → workloads/large_benchmark.csv
[2/4] Running Adaptive scheduler ...
      ✓  AvgWT=11660.6  AvgRT=1356.4  Fairness=0.553
[3/4] Running MLFQ-only (no-adaptive) ...
      ✓  AvgWT=14865.7  AvgRT=1655.9  Fairness=0.554
[4/4] Extracting per-policy breakdown ...
  Results saved → output/benchmark_results.json
```

You can change the process count:
```bash
python3 benchmark.py --count 500
python3 benchmark.py --count 2000
```

---

### Step 5 — Generate the interactive dashboard

```bash
python3 generate_dashboard.py
```

This writes `output/adaptivesched_dashboard.html`.

---

### Step 6 — Open the dashboard in your browser

Run this in **PowerShell** (not WSL):
```powershell
start ".\output\adaptivesched_dashboard.html"
```

Or navigate to the `output/` folder in File Explorer and double-click `adaptivesched_dashboard.html`.

---

## Running the Simulator

The simulator binary supports these flags:

```
Usage: ./build/adaptive_sched [OPTIONS]

Options:
  --workload <name>     Built-in profile: interactive, cpu_bound, mixed, stress, batch
  --count <n>           Number of processes to simulate
  --no-adaptive         Disable adaptive switching (runs MLFQ only)
  --export-csv          Export per-process metrics → output/metrics.csv
  --output-dir <path>   Directory for exported files (default: ./output)
  --verbose             Print live monitor snapshots every 100 ticks
  --quiet               Suppress all console output (for scripting)
  --max-ticks <n>       Hard tick limit (0 = run to completion)
  --help
```

### Examples

```bash
# Run built-in mixed workload with verbose output
./build/adaptive_sched --workload mixed --verbose

# Stress test with 200 processes
./build/adaptive_sched --workload stress --count 200 --verbose

# Compare: disable adaptive (MLFQ only)
./build/adaptive_sched --workload mixed --no-adaptive

# Quiet mode for scripting
./build/adaptive_sched --workload mixed --quiet --output-dir ./output/myrun
```

---

## Benchmark & Dashboard

### What `benchmark.py` does
Generates a large synthetic workload with a realistic heterogeneous mix of five process types:

| Type | Share | Burst profile | Behaviour |
|---|---|---|---|
| Interactive | 25% | 1–8 ticks | Yields early, I/O-bound |
| CPU-bound | 25% | 10–80 ticks | Burns full quantum every time |
| Daemon | 20% | 2–20 ticks | Medium burst, background services |
| Burst | 15% | 3–35 ticks | Variable, mixed behaviour |
| Batch | 15% | 20–100 ticks | Very long, bulk processing jobs |

It then runs the simulator **twice on the exact same workload** — once with Adaptive enabled, once with MLFQ fixed (`--no-adaptive`) — and saves full per-process metrics from both runs to `output/benchmark_results.json`.

### What `generate_dashboard.py` does
Reads `benchmark_results.json` and produces a fully self-contained `output/adaptivesched_dashboard.html` with 9 interactive chart sections powered by Plotly.js:

| Section | What it shows |
|---|---|
| KPI Strip | 6 headline metrics with % delta vs MLFQ |
| Summary Bar Chart | Avg Wait, TAT, Response, p95/p99 — side by side |
| Distribution Histograms | Waiting and response time spread across all processes |
| Box Plots by Process Type | Interactive vs CPU-bound response and wait times |
| Per-Process WT Delta | Sorted bar — which processes each scheduler wins |
| Cumulative Throughput | Processes completed over simulation time |
| Policy Pie + Context Switches | Policy usage breakdown; total context switch count |
| Burst vs Wait Scatter | Per-process scatter coloured by process type |
| Full Metrics Table | Every metric with winner highlighted per row |

---

## Benchmark Results & Analysis

Results from a benchmark run of **1,000 processes** on a heterogeneous mixed workload.

### Headline Numbers

| Metric | Adaptive | MLFQ Fixed | Improvement |
|---|---|---|---|
| Avg Waiting Time | **11,660.6** ticks | 14,865.7 ticks | **21.6% lower** |
| Avg Turnaround Time | **11,695.0** ticks | 14,900.1 ticks | **21.5% lower** |
| Avg Response Time | **1,356.4** ticks | 1,655.9 ticks | **18.1% lower** |
| p95 Waiting Time | **30,940** ticks | 32,943 ticks | **6.1% lower** |
| p99 Waiting Time | **32,454** ticks | 33,190 ticks | **2.2% lower** |
| Total Context Switches | **2,267** | 16,416 | **7.2× fewer** |
| Interactive Avg RT | **1,103.4** ticks | 1,346.8 ticks | **18.1% lower** |
| Jain's Fairness Index | 0.5527 | 0.5538 | Comparable |

At 1,000 processes, the Adaptive scheduler wins on **every metric except Jain's Fairness** (where the difference is negligible at 0.002).

### Key Observations

**1. 21.6% lower average waiting time.**
This is the most important headline. On a real system, a 21.6% reduction in average wait means processes spend significantly less time idle. The improvement comes from the adaptive engine recognising the workload is CPU-bound and switching to FCFS, which eliminates the overhead of MLFQ's multi-level queue management for jobs that will never voluntarily yield.

**2. 7.2× fewer context switches (2,267 vs 16,416).**
Context switches are not free — each one requires saving and restoring CPU registers, flushing parts of the TLB, and cache warming costs. Fixed MLFQ generates 16,416 switches because it preempts processes at every quantum boundary across four levels, even when the workload has no short-burst processes that would benefit from it. The adaptive engine, by switching to FCFS when the workload is CPU-bound, eliminates this overhead almost entirely.

**3. Adaptive wins on interactive response time too.**
Unlike the 500-process run where MLFQ had a better interactive response time, at 1,000 processes the Adaptive scheduler achieves **18.1% lower** interactive avg RT (1,103 vs 1,346 ticks). This is because the larger workload gives the monitor thread enough signal to make better classification decisions earlier, and the MLFQ phase at the start of the run handles interactive processes before switching to FCFS for the bulk batch.

**4. Policy split: 63.8% FCFS, 36.2% MLFQ.**
Unlike the 500-process run (95.2% FCFS), the 1,000-process workload has more interactive processes arriving early, so MLFQ stays active for longer before the engine detects the CPU-bound shift and switches. This is the adaptive behaviour working correctly — it uses MLFQ when the mix is heterogeneous and FCFS when the batch phase dominates.

**5. The gap grows with scale.**
At 500 processes, the avg wait improvement was 4.4%. At 1,000 processes it is 21.6%. This is not a coincidence — larger workloads amplify the cost of fixed-policy overhead. MLFQ's level management and priority boosts become increasingly wasteful as more CPU-bound processes accumulate in the lower queues, while the adaptive engine detects this pattern and eliminates the overhead.

### When to use each

| Use Adaptive when... | Use MLFQ fixed when... |
|---|---|
| Workload is mixed or shifts phases mid-run | Workload is uniformly interactive throughout |
| Process count is large (100+) | You need fully predictable, stationary behaviour |
| Minimising wait time and tail latency matters | Response time for the very first tick matters most |
| Context-switch overhead is a concern | Workload characteristics are known in advance |

---

## Output & Metrics

### Console Output (sample)

```
**********************************************************************
*  AdaptiveSched++ — CPU Scheduling Simulator
*  Workload  : mixed
*  Processes : 1000
*  Adaptive  : ENABLED
**********************************************************************

[GEN] Injecting PID=1 (proc_1) at tick=2 burst=4 pri=1 [INTERACTIVE]
...

======================================================================
[ADAPTIVE] *** POLICY SWITCH ***
  Tick       : 1336
  From       : MLFQ
  To         : FCFS
  Confidence : 0.400
  Workload   : CPU_BOUND
  Reason     : cpu-bound workload;
  Metrics    :
    avg_burst=18.4  avg_wait=892.1  cpu_util=0.91
    starvation=0.000  interactive=0.062  csr=2.100
======================================================================

================================================================================
                       SIMULATION SUMMARY
================================================================================
  Processes completed  : 1000
  Avg Waiting Time     : 11660.60 ticks
  Avg Turnaround Time  : 11695.00 ticks
  Avg Response Time    : 1356.45 ticks
  CPU Utilization      : 100.00%
  Context Switches     : 2267
  Jain's Fairness Index: 0.55
  Scheduler Switches   : 2
```

### Exported CSV columns

```
pid, name, arrival, burst, start, completion, waiting, turnaround,
response, priority, queue_level, ctx_switches, interactive,
starvation_ticks, scheduled_by
```

The `scheduled_by` column records which policy (MLFQ, FCFS, ROUND_ROBIN, PRIORITY) was active when each process completed — used for per-policy breakdown in the dashboard.

---

## Design Decisions

**Why logical ticks instead of wall-clock time?**
Determinism and reproducibility. Logical ticks make every run bit-identical given the same seed and allow the simulation to run at maximum machine speed without any real-time dependency.

**Why not use `std::priority_queue` for the ready queue?**
`std::priority_queue` doesn't support O(n) removal by PID, which is needed for preemption. A sorted `std::deque` with `remove()` provides the right balance of insert cost and flexibility.

**Why does the adaptive scheduler start with MLFQ?**
MLFQ is the best general-purpose default — it handles both interactive and CPU-bound workloads without prior knowledge of process behaviour. Specialised policies are only activated when the workload signal is clear and persistent.

**Why EMA rather than a simple window average?**
Exponential Moving Averages weight recent observations more heavily while never discarding old data entirely. This lets the adaptive engine react to workload phase changes without thrashing on transient spikes.

**Why a cooldown + hysteresis guard?**
Without cooldown, the engine could oscillate between two policies every evaluation window. The 40-tick cooldown and 0.15 confidence margin ensure switches carry sufficient evidence and the new policy has time to demonstrate its effect before being reconsidered.

**Why does `--quiet` exist?**
The benchmark runner calls the simulator binary twice as a subprocess. Without quiet mode, both runs would print thousands of log lines to the terminal. `--quiet` redirects all stdout to a null buffer so the benchmark script runs cleanly and only collects the exported per-process metrics.