#pragma once

#include "scheduler.h"
#include "../core/ready_queue.h"
#include <deque>

namespace adaptive_sched
{

    // ---------------------------------------------------------------------------
    // RRScheduler — Round-Robin with configurable time quantum
    //
    // Each process is given exactly `quantum_` ticks of CPU time.
    // When the quantum expires the process is re-enqueued at the tail of the
    // ready queue, guaranteeing bounded response time and strong fairness.
    //
    // Context-switch overhead (configurable) is charged per switch and counted
    // in idle ticks so that metrics reflect realistic overhead.
    // ---------------------------------------------------------------------------
    class RRScheduler : public IScheduler
    {
    public:
        static constexpr Tick DEFAULT_QUANTUM = 4;
        static constexpr Tick DEFAULT_CTX_SWITCH_COST = 1;

        explicit RRScheduler(Tick quantum = DEFAULT_QUANTUM,
                             Tick ctx_switch_cost = DEFAULT_CTX_SWITCH_COST);

        void add_process(ProcessPtr proc) override;
        ProcessPtr get_next_process() override;
        bool handle_preemption(ProcessPtr running,
                               ProcessPtr incoming) override;
        void on_process_complete(ProcessPtr proc) override;

        std::string name() const override { return "RR"; }
        SchedulerPolicy policy() const override { return SchedulerPolicy::ROUND_ROBIN; }
        std::size_t ready_count() const override;
        bool has_ready_work() const override;
        std::vector<ProcessPtr> snapshot() const override;

        std::vector<ProcessPtr> drain_queue() override;
        void import_queue(std::vector<ProcessPtr> procs) override;

        const SchedulerStats &stats() const override { return stats_; }
        void finalize_stats(Tick total_ticks) override;

        Tick get_quantum() const override { return quantum_; }
        void set_quantum(Tick q) override { quantum_ = q; }

    private:
        Tick quantum_;
        Tick ctx_switch_cost_;

        ReadyQueue queue_; // FIFO — processes re-enqueue at tail

        mutable std::mutex completed_mtx_;
        std::vector<ProcessPtr> completed_;

        // Tracks whether we are in a context-switch overhead window
        bool in_ctx_switch_ = false;
        Tick ctx_switch_done_at_ = 0;
    };

} // namespace adaptive_sched