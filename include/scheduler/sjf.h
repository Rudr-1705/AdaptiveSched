#pragma once

#include "scheduler.h"
#include "../core/ready_queue.h"
#include <vector>

namespace adaptive_sched
{

    // ---------------------------------------------------------------------------
    // SJFScheduler — Shortest Job First / Shortest Remaining Time First
    //
    // Non-preemptive mode (SJF):
    //   Selects the process with the smallest *burst_time* at each dispatch point.
    //
    // Preemptive mode (SRTF):
    //   At each add_process() call a preemption check is made: if the incoming
    //   process has a shorter *remaining_time* than the currently running process,
    //   handle_preemption() returns true and the CPU engine preempts.
    //
    // Both modes can exhibit starvation of long processes when short processes
    // continuously arrive — this is intentionally visible in metrics.
    // ---------------------------------------------------------------------------
    class SJFScheduler : public IScheduler
    {
    public:
        enum class Mode
        {
            NON_PREEMPTIVE,
            SRTF
        };

        explicit SJFScheduler(Mode mode = Mode::NON_PREEMPTIVE);

        void add_process(ProcessPtr proc) override;
        ProcessPtr get_next_process() override;
        bool handle_preemption(ProcessPtr running,
                               ProcessPtr incoming) override;
        void on_process_complete(ProcessPtr proc) override;
        void on_tick(Tick current_time) override;

        std::string name() const override;
        SchedulerPolicy policy() const override;
        std::size_t ready_count() const override;
        bool has_ready_work() const override;
        std::vector<ProcessPtr> snapshot() const override;

        std::vector<ProcessPtr> drain_queue() override;
        void import_queue(std::vector<ProcessPtr> procs) override;

        const SchedulerStats &stats() const override { return stats_; }
        void finalize_stats(Tick total_ticks) override;

        Mode get_mode() const { return mode_; }

    private:
        Mode mode_;
        ReadyQueue queue_; // ordered by remaining burst time

        mutable std::mutex completed_mtx_;
        std::vector<ProcessPtr> completed_;

        // Age-based starvation prevention for long processes
        static constexpr int STARVATION_THRESHOLD = 50; // ticks
        void check_starvation(Tick now);
    };

} // namespace adaptive_sched