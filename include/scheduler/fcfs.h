#pragma once

#include "scheduler.h"
#include "../core/ready_queue.h"

namespace adaptive_sched
{

    // ---------------------------------------------------------------------------
    // FCFSScheduler — First-Come, First-Served (non-preemptive)
    //
    // Maintains a simple FIFO ready queue.  Processes run to completion once
    // selected.  The convoy effect is observable when long CPU-bound jobs block
    // short jobs behind them.
    // ---------------------------------------------------------------------------
    class FCFSScheduler : public IScheduler
    {
    public:
        FCFSScheduler();

        void add_process(ProcessPtr proc) override;
        ProcessPtr get_next_process() override;
        bool handle_preemption(ProcessPtr running,
                               ProcessPtr incoming) override;
        void on_process_complete(ProcessPtr proc) override;

        std::string name() const override { return "FCFS"; }
        SchedulerPolicy policy() const override { return SchedulerPolicy::FCFS; }
        std::size_t ready_count() const override;
        bool has_ready_work() const override;
        std::vector<ProcessPtr> snapshot() const override;

        std::vector<ProcessPtr> drain_queue() override;
        void import_queue(std::vector<ProcessPtr> procs) override;

        const SchedulerStats &stats() const override { return stats_; }
        void finalize_stats(Tick total_ticks) override;

    private:
        ReadyQueue queue_; // FIFO

        mutable std::mutex completed_mtx_;
        std::vector<ProcessPtr> completed_;
    };

} // namespace adaptive_sched