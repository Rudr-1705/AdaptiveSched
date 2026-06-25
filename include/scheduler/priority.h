#pragma once

#include "scheduler.h"
#include "../core/ready_queue.h"

namespace adaptive_sched
{

    // ---------------------------------------------------------------------------
    // PriorityScheduler — Preemptive Priority Scheduling with Aging
    //
    // Processes are ordered by priority (lower number = higher urgency).
    // When a higher-priority process arrives, the running process is preempted.
    //
    // Anti-starvation mechanism:
    //   A background aging pass (driven by on_tick) periodically increases the
    //   effective priority of waiting processes that have not been scheduled for
    //   AGING_THRESHOLD ticks.  This prevents indefinite starvation of low-
    //   priority processes.
    // ---------------------------------------------------------------------------
    class PriorityScheduler : public IScheduler
    {
    public:
        static constexpr int AGING_THRESHOLD = 20;     // ticks before boost
        static constexpr int AGING_BOOST = 1;          // priority units per boost
        static constexpr int MAX_PRIORITY_BOOSTS = 10; // cap boosts to avoid inversion

        PriorityScheduler();

        void add_process(ProcessPtr proc) override;
        ProcessPtr get_next_process() override;
        bool handle_preemption(ProcessPtr running,
                               ProcessPtr incoming) override;
        void on_process_complete(ProcessPtr proc) override;
        void on_tick(Tick current_time) override;

        std::string name() const override { return "PRIORITY"; }
        SchedulerPolicy policy() const override { return SchedulerPolicy::PRIORITY; }
        std::size_t ready_count() const override;
        bool has_ready_work() const override;
        std::vector<ProcessPtr> snapshot() const override;

        std::vector<ProcessPtr> drain_queue() override;
        void import_queue(std::vector<ProcessPtr> procs) override;

        const SchedulerStats &stats() const override { return stats_; }
        void finalize_stats(Tick total_ticks) override;

    private:
        ReadyQueue queue_; // ordered by effective priority

        mutable std::mutex completed_mtx_;
        std::vector<ProcessPtr> completed_;

        // Apply one round of aging to all waiting processes
        void apply_aging(Tick now);

        // Rebalance queue after an aging pass (priorities may have changed)
        void rebalance();
    };

} // namespace adaptive_sched