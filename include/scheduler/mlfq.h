#pragma once

#include "scheduler.h"
#include "../core/ready_queue.h"
#include <array>
#include <chrono>

namespace adaptive_sched
{

    // ---------------------------------------------------------------------------
    // MLFQScheduler — Multi-Level Feedback Queue
    //
    // Queue hierarchy (high → low priority):
    //   Level 0 : RR with short quantum (interactive / I/O-bound jobs)
    //   Level 1 : RR with medium quantum
    //   Level 2 : RR with long quantum
    //   Level 3 : FCFS (CPU-bound / batch jobs that have fallen all the way down)
    //
    // Promotion/Demotion:
    //   • A new process always enters at Level 0.
    //   • If a process exhausts its quantum without blocking, it is demoted one
    //     level on the next dispatch.
    //   • If a process voluntarily yields (blocks for I/O) before exhausting its
    //     quantum, it stays at its current level (or is promoted).
    //
    // Priority Boost (anti-starvation):
    //   Every BOOST_INTERVAL ticks ALL processes are reset to Level 0.  This
    //   prevents indefinite starvation and avoids gaming (processes that sleep
    //   just before quantum exhaustion to avoid demotion).
    //
    // Design mirrors the scheduler described in OSTEP Ch. 8.
    // ---------------------------------------------------------------------------
    class MLFQScheduler : public IScheduler
    {
    public:
        static constexpr std::size_t NUM_LEVELS = 4;
        static constexpr Tick BOOST_INTERVAL = 100; // ticks between global boosts

        // Per-level quanta
        static constexpr std::array<Tick, NUM_LEVELS> LEVEL_QUANTA = {2, 4, 8, 0 /*FCFS*/};

        MLFQScheduler();

        void add_process(ProcessPtr proc) override;
        ProcessPtr get_next_process() override;
        bool handle_preemption(ProcessPtr running,
                               ProcessPtr incoming) override;
        void on_process_complete(ProcessPtr proc) override;
        void on_tick(Tick current_time) override;

        std::string name() const override { return "MLFQ"; }
        SchedulerPolicy policy() const override { return SchedulerPolicy::MLFQ; }
        std::size_t ready_count() const override;
        bool has_ready_work() const override;
        std::vector<ProcessPtr> snapshot() const override;

        std::vector<ProcessPtr> drain_queue() override;
        void import_queue(std::vector<ProcessPtr> procs) override;

        const SchedulerStats &stats() const override { return stats_; }
        void finalize_stats(Tick total_ticks) override;

        Tick get_quantum() const override { return LEVEL_QUANTA[0]; }

        // Expose queue occupancy per level (used by adaptive scheduler metrics)
        std::array<std::size_t, NUM_LEVELS> level_sizes() const;

    private:
        // One ReadyQueue per MLFQ level
        std::array<std::unique_ptr<ReadyQueue>, NUM_LEVELS> levels_;

        mutable std::mutex completed_mtx_;
        std::vector<ProcessPtr> completed_;

        Tick last_boost_tick_ = 0;

        // -----------------------------------------------------------------------
        // Internal helpers
        // -----------------------------------------------------------------------

        // Enqueue a process at its current queue_level
        void enqueue_at_level(ProcessPtr proc);

        // Demote process one level (called when quantum exhausted)
        void demote(ProcessPtr proc);

        // Promote process one level (called when process voluntarily yields)
        void promote(ProcessPtr proc);

        // Reset all processes to level 0 (global priority boost)
        void priority_boost(Tick now);

        // Returns the highest non-empty level index, or NUM_LEVELS if all empty
        std::size_t highest_occupied_level() const;
    };

} // namespace adaptive_sched