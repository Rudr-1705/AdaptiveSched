#pragma once

#include "../core/process.h"
#include "../core/ready_queue.h"
#include "../core/state.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>

namespace adaptive_sched
{

    // ---------------------------------------------------------------------------
    // SchedulerStats — per-scheduler telemetry accumulated at runtime
    // ---------------------------------------------------------------------------
    struct SchedulerStats
    {
        std::atomic<uint64_t> processes_handled{0};
        std::atomic<uint64_t> context_switches{0};
        std::atomic<uint64_t> preemptions{0};
        std::atomic<uint64_t> idle_ticks{0};
        std::atomic<uint64_t> total_cpu_ticks{0};
        std::atomic<uint64_t> starvation_events{0};

        // Non-atomic summary values (written once at completion)
        double avg_waiting_time = 0.0;
        double avg_turnaround_time = 0.0;
        double avg_response_time = 0.0;
        double cpu_utilization = 0.0;
        double throughput = 0.0;
        double fairness_index = 0.0; // Jain's fairness index

        void reset()
        {
            processes_handled.store(0);
            context_switches.store(0);
            preemptions.store(0);
            idle_ticks.store(0);
            total_cpu_ticks.store(0);
            starvation_events.store(0);
        }
    };

    // ---------------------------------------------------------------------------
    // IScheduler — abstract base for all scheduling algorithms
    //
    // Concrete schedulers (FCFS, SJF, RR, Priority, MLFQ) all derive from this.
    // The adaptive scheduler also derives from this so it can be composed.
    // ---------------------------------------------------------------------------
    class IScheduler
    {
    public:
        virtual ~IScheduler() = default;

        // -----------------------------------------------------------------------
        // Core scheduling interface
        // -----------------------------------------------------------------------

        // Submit a newly arrived process to this scheduler
        virtual void add_process(ProcessPtr proc) = 0;

        // Retrieve the next process to run; returns nullptr if idle
        virtual ProcessPtr get_next_process() = 0;

        // Notify that the running process used its full quantum / was preempted
        // Returns: true if a context switch should occur
        virtual bool handle_preemption(ProcessPtr running,
                                       ProcessPtr incoming = nullptr) = 0;

        // Called when a process completes execution
        virtual void on_process_complete(ProcessPtr proc) = 0;

        // Called every tick for schedulers that need periodic bookkeeping
        // (e.g., MLFQ priority boost, aging)
        virtual void on_tick(Tick current_time) {}

        // -----------------------------------------------------------------------
        // Introspection
        // -----------------------------------------------------------------------

        virtual std::string name() const = 0;
        virtual SchedulerPolicy policy() const = 0;
        virtual std::size_t ready_count() const = 0;
        virtual bool has_ready_work() const = 0;
        virtual std::vector<ProcessPtr> snapshot() const = 0;

        // -----------------------------------------------------------------------
        // State migration (used by adaptive scheduler when switching policies)
        // -----------------------------------------------------------------------

        // Export all queued (non-running) processes for handoff
        virtual std::vector<ProcessPtr> drain_queue() = 0;

        // Import processes from a previous scheduler's drain_queue() result
        virtual void import_queue(std::vector<ProcessPtr> procs) = 0;

        // -----------------------------------------------------------------------
        // Metrics
        // -----------------------------------------------------------------------

        virtual const SchedulerStats &stats() const = 0;
        virtual void finalize_stats(Tick total_ticks) = 0;

        // -----------------------------------------------------------------------
        // Optional hook: quantum size (RR / MLFQ expose this)
        // -----------------------------------------------------------------------
        virtual Tick get_quantum() const { return 0; }
        virtual void set_quantum(Tick q) {}

    protected:
        SchedulerStats stats_;

        // Helper: compute and store Jain's fairness index from completed processes
        void compute_fairness(const std::vector<ProcessPtr> &completed);
    };

} // namespace adaptive_sched