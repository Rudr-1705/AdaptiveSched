# AdaptiveSched++

A production-grade adaptive CPU scheduling simulator implemented in C++17.

AdaptiveSched++ simulates a single-core CPU scheduler that **dynamically switches between scheduling algorithms at runtime** based on observed workload characteristics. The codebase is modular, thread-safe, and architecturally faithful to real operating system scheduling frameworks.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Scheduling Algorithms](#scheduling-algorithms)
3. [Adaptive Scheduler Logic](#adaptive-scheduler-logic)
4. [Build Instructions](#build-instructions)
5. [Running the Simulator](#running-the-simulator)
6. [Workload Files](#workload-files)
7. [Output & Metrics](#output--metrics)
8. [Design Decisions](#design-decisions)

---

## Architecture Overview

```
AdaptiveSched++/
├── include/
│   ├── core/
│   │   ├── process.h          # PCB: all per-process state
│   │   ├── state.h            # ProcessState / WorkloadType / SchedulerPolicy enums
│   │   ├── clock.h            # Deterministic logical tick clock (singleton)
│   │   └── ready_queue.h      # Thread-safe, multi-order blocking queue
│   ├── scheduler/
│   │   ├── scheduler.h        # IScheduler abstract base + SchedulerStats
│   │   ├── fcfs.h             # First-Come First-Served
│   │   ├── sjf.h              # SJF (non-preemptive) + SRTF (preemptive)
│   │   ├── rr.h               # Round-Robin with configurable quantum
│   │   ├── priority.h         # Preemptive Priority + aging
│   │   ├── mlfq.h             # Multi-Level Feedback Queue (4 levels)
│   │   └── adaptive.h         # Adaptive meta-scheduler (main highlight)
│   ├── execution/
│   │   └── cpu.h              # Single-core CPU execution engine
│   ├── metrics/
│   │   └── statistics.h       # Metrics aggregation & reporting
│   └── simulation/
│       ├── process_generator.h # Static (CSV) and dynamic process injection
│       ├── simulation_controller.h # Top-level orchestrator
│       └── workload.h          # WorkloadProfile, WorkloadLoader, WorkloadFactory
├── src/                        # Corresponding .cpp implementations
├── workloads/                  # Pre-built CSV workloads
├── main.cpp                    # CLI entry point
├── CMakeLists.txt
└── README.md
```

### Thread Model

The simulator runs three concurrent threads:

| Thread               | Responsibility                                                      |
| -------------------- | ------------------------------------------------------------------- |
| **CPU thread**       | Executes processes tick-by-tick, advances clock, handles preemption |
| **Generator thread** | Injects processes into the scheduler at their arrival ticks         |
| **Monitor thread**   | Calls `evaluate_and_adapt()` periodically; detects termination      |

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

| Level       | Order | Quantum           |
| ----------- | ----- | ----------------- |
| 0 (highest) | FIFO  | 2 ticks           |
| 1           | FIFO  | 4 ticks           |
| 2           | FIFO  | 8 ticks           |
| 3 (lowest)  | FIFO  | run-to-completion |

- New processes enter at Level 0.
- Exhausting a quantum causes **demotion** to the next level.
- A higher-level process arriving **preempts** a lower-level running process.
- Every 100 ticks a **global priority boost** resets all processes to Level 0 (anti-starvation, anti-gaming).

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
The metric vector is mapped to one of five workload types using weighted heuristics:

| Classification | Key Indicators                                                     |
| -------------- | ------------------------------------------------------------------ |
| `CPU_BOUND`    | High avg burst (>12), high utilisation, few context switches       |
| `INTERACTIVE`  | High interactive fraction (>40%), low response time, MLFQ L0 heavy |
| `BATCH`        | Very long bursts (>20), low throughput, minimal switching          |
| `STRESS`       | CPU_BOUND AND utilisation > 85%                                    |
| `MIXED`        | No dominant signal, OR starvation detected                         |

### Step 3 — Score Policies
Each candidate policy receives a confidence score `[0, 1]` based on:
- Workload classification match
- Individual metric thresholds (starvation pressure, response time, queue depth)
- Historical trend analysis (`is_trending_up()` over last 8 evaluations)
- A small stability bonus for the currently active policy

### Step 4 — Hysteresis
A switch only fires when:
1. The best policy differs from the current one.
2. The best policy's score exceeds the current policy's score by at least `CONFIDENCE_HYSTERESIS = 0.15`.
3. At least `COOLDOWN_TICKS = 40` ticks have elapsed since the last switch.

This three-part guard prevents oscillation/thrashing.

### Step 5 — Migrate
The active scheduler's ready queue is drained and handed off to the new scheduler via `drain_queue()` / `import_queue()`. Processes preserve their `remaining_time`, `priority`, and `queue_level` through the migration. A `SwitchRecord` is appended to the immutable switch history with full metric snapshot.

### Example Adaptive Decisions

```
[ADAPTIVE] *** POLICY SWITCH ***
  Tick       : 420
  From       : MLFQ
  To         : FCFS
  Confidence : 0.400
  Workload   : CPU_BOUND
  Reason     : cpu-bound workload;
  Metrics    :
    avg_burst=22.4  avg_wait=8.1  cpu_util=0.91
    starvation=0.000  interactive=0.02  csr=0.05
```

---

## Build Instructions

### Prerequisites

| Tool          | Version                                           |
| ------------- | ------------------------------------------------- |
| CMake         | 3.16+                                             |
| GCC           | 9+ (C++17) or Clang 10+ or MSVC 2019+             |
| POSIX threads | (`-lpthread` on Linux, built-in on macOS/Windows) |

### Linux / macOS

```bash
git clone <repo_url>
cd AdaptiveSched++

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Binary is at: build/adaptive_sched
```

### Debug build (with AddressSanitizer)

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### Windows (MSVC)

```powershell
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

---

## Running the Simulator

```
Usage: ./adaptive_sched [OPTIONS]

Options:
  --workload <name>   Profile: interactive, cpu_bound, mixed, stress, batch
  --csv <path>        Load workload from CSV instead of profile
  --count <n>         Override process count
  --no-adaptive       Disable adaptive switching (runs MLFQ only)
  --export-csv        Export per-process metrics → output/metrics.csv
  --export-timeline   Export tick timeline → output/timeline.csv
  --verbose           Print live monitor snapshots every 100 ticks
  --max-ticks <n>     Hard tick limit (0 = run to completion)
  --help
```

### Examples

```bash
# Run built-in interactive workload
./build/adaptive_sched --workload interactive

# Stress test with verbose monitor output
./build/adaptive_sched --workload stress --count 80 --verbose

# Load from CSV, export metrics
./build/adaptive_sched --csv workloads/mixed.csv --export-csv --export-timeline

# Disable adaptive for comparison
./build/adaptive_sched --workload cpu_bound --no-adaptive

# Large batch run
./build/adaptive_sched --workload batch --count 100 --verbose
```

---

## Workload Files

CSV format: `pid,name,arrival,burst,priority,interactive`

| File                        | Description                                            |
| --------------------------- | ------------------------------------------------------ |
| `workloads/interactive.csv` | 40 short-burst UI processes (burst 1–9 ticks)          |
| `workloads/cpu_bound.csv`   | 30 compute-heavy processes (burst 10–70 ticks)         |
| `workloads/mixed.csv`       | 50 processes: compilers, editors, network I/O, daemons |
| `workloads/stress.csv`      | 60 processes with high arrival rate, starvation-prone  |

You can generate custom workloads with the built-in profiles or write your own CSV.

---

## Output & Metrics

### Console Output (sample)

```
**********************************************************************
*  AdaptiveSched++ — CPU Scheduling Simulator
*  Scenario  : mixed_workload
*  Workload  : mixed
*  Processes : 50
*  Adaptive  : ENABLED
**********************************************************************

[GEN] Injecting PID=   1 (shell_cmd) at tick=0 burst=5 pri=2
[GEN] Injecting PID=   2 (compiler) at tick=0 burst=45 pri=6
...

======================================================================
[ADAPTIVE] *** POLICY SWITCH ***
  Tick       : 340
  From       : MLFQ
  To         : ROUND_ROBIN
  Confidence : 0.550
  Workload   : INTERACTIVE
  Reason     : interactive workload; high response time;
======================================================================

================================================================================
                   SIMULATION SUMMARY: mixed_workload
================================================================================
  Processes completed  : 50
  Total simulation time: 4851 ticks
  Avg Waiting Time     : 2341.22 ticks
  Avg Turnaround Time  : 2351.88 ticks
  Avg Response Time    : 2341.22 ticks
  CPU Utilization      : 97.40%
  Throughput           : 0.01 proc/tick
  Context Switches     : 312
  Jain's Fairness Index: 0.72
  Scheduler Switches   : 2
```

### Exported CSV columns

`pid, name, arrival, burst, start, completion, waiting, turnaround, response, priority, queue_level, ctx_switches, interactive, starvation_ticks, scheduled_by`

---

## Design Decisions

**Why logical ticks instead of wall-clock time?**
Determinism and reproducibility. Logical ticks make every run bit-identical given the same seed, and allow the simulation to run at maximum machine speed or throttled to a specific tick rate.

**Why not use `std::priority_queue` for the ready queue?**
`std::priority_queue` doesn't support O(n) removal by PID (needed for preemption). A sorted `std::deque` with `remove()` provides the right balance of insert cost and flexibility.

**Why does the adaptive scheduler start with MLFQ?**
MLFQ is the best general-purpose default: it handles both interactive and CPU-bound workloads without prior knowledge of process behaviour. Specialised policies (FCFS for pure batch, RR for interactive-only) are only activated when the workload signal is clear and persistent.

**Why EMA rather than a simple window average?**
Exponential Moving Averages weight recent observations more heavily while never discarding old data entirely. This lets the adaptive engine react quickly to workload phase changes without thrashing on transient spikes.

**Why a cooldown + hysteresis guard?**
Without cooldown, the engine could oscillate between two policies every evaluation window. The 40-tick cooldown and 0.15 confidence margin ensure that switches carry sufficient evidence and that the new policy has time to demonstrate its effect before being reconsidered.
