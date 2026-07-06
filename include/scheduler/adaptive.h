#pragma once

#include "scheduler.h"
#include "fcfs.h"
#include "rr.h"
#include "priority.h"
#include "mlfq.h"
#include "../core/state.h"

#include <deque>
#include <atomic>
#include <mutex>
#include <memory>
#include <string>
#include <array>
#include <map>
#include <functional>
#include <optional>

namespace adaptive_sched
{

    // ---------------------------------------------------------------------------
    // WorkloadMetrics — rolling snapshot of runtime observations used by the
    //                   adaptive decision engine
    // ---------------------------------------------------------------------------
    struct WorkloadMetrics
    {
        // Raw samples (exponentially weighted moving averages)
        double avg_burst_length = 0.0;     // EMA of process burst lengths
        double avg_waiting_time = 0.0;     // EMA of waiting times
        double avg_response_time = 0.0;    // EMA of response times
        double cpu_utilization = 0.0;      // fraction of ticks with work
        double context_switch_rate = 0.0;  // switches per tick
        double throughput_rate = 0.0;      // completions per SAMPLE_WINDOW ticks
        double starvation_pressure = 0.0;  // fraction of processes past threshold
        double interactive_fraction = 0.0; // fraction of processes flagged interactive
        double queue_depth_avg = 0.0;      // EMA of ready-queue depth
        double mlfq_level0_fraction = 0.0; // fraction of processes sitting at L0

        // Derived classification scores [0,1]
        double cpu_bound_score = 0.0;
        double interactive_score = 0.0;
        double batch_score = 0.0;
        double stress_score = 0.0;

        WorkloadType classified_type = WorkloadType::MIXED;

        Tick sample_time = 0;
    };

    // ---------------------------------------------------------------------------
    // PolicyScore — confidence that a given policy is optimal right now
    // ---------------------------------------------------------------------------
    struct PolicyScore
    {
        SchedulerPolicy policy;
        double confidence; // [0, 1]
        std::string rationale;
    };

    // ---------------------------------------------------------------------------
    // SwitchRecord — immutable log entry created when a policy switch occurs
    // ---------------------------------------------------------------------------
    struct SwitchRecord
    {
        Tick at;
        SchedulerPolicy from;
        SchedulerPolicy to;
        WorkloadMetrics metrics_snapshot;
        std::string reason;
        double confidence;
    };

    // ---------------------------------------------------------------------------
    // AdaptiveScheduler — runtime-adaptive scheduling policy controller
    //
    // Architecture:
    //   The adaptive scheduler owns four concrete child schedulers (FCFS, RR,
    //   Priority, MLFQ) and at any given tick delegates all add/get/preemption
    //   calls to exactly one "active" child.
    //
    //   A separate monitor thread (driven by the SimulationController) calls
    //   evaluate_and_adapt() every EVALUATION_INTERVAL ticks.  This function:
    //     1. Samples runtime metrics from the active child and CPU engine.
    //     2. Classifies the current workload using weighted heuristics.
    //     3. Scores all candidate policies against the classified workload.
    //     4. Applies hysteresis and cooldown to avoid thrashing.
    //     5. If a better policy is found with sufficient confidence margin,
    //        migrates the ready-queue state and switches the active child.
    //
    //   Hysteresis prevents oscillation: the new policy's score must exceed the
    //   current policy's score by at least CONFIDENCE_HYSTERESIS, and at least
    //   COOLDOWN_TICKS must have passed since the last switch.
    // ---------------------------------------------------------------------------
    class AdaptiveScheduler : public IScheduler
    {
    public:
        // Tuning constants — separated so they can be tweaked without recompiling logic
        static constexpr Tick EVALUATION_INTERVAL = 20;       // ticks between evaluations
        static constexpr Tick COOLDOWN_TICKS = 40;            // minimum ticks between switches
        static constexpr double CONFIDENCE_HYSTERESIS = 0.15; // required margin for a switch
        static constexpr double EMA_ALPHA = 0.25;             // smoothing factor for EMAs
        static constexpr int HISTORY_DEPTH = 8;               // evaluation window for trending
        static constexpr int STARVATION_TICK_LIMIT = 80;      // ticks before starvation penalty

        AdaptiveScheduler();

        // -----------------------------------------------------------------------
        // IScheduler interface — delegate to active child
        // -----------------------------------------------------------------------
        void add_process(ProcessPtr proc) override;
        ProcessPtr get_next_process() override;
        bool handle_preemption(ProcessPtr running,
                               ProcessPtr incoming) override;
        void on_process_complete(ProcessPtr proc) override;
        void on_tick(Tick current_time) override;

        std::string name() const override { return "ADAPTIVE"; }
        SchedulerPolicy policy() const override { return SchedulerPolicy::ADAPTIVE; }
        std::size_t ready_count() const override;
        bool has_ready_work() const override;
        std::vector<ProcessPtr> snapshot() const override;

        std::vector<ProcessPtr> drain_queue() override;
        void import_queue(std::vector<ProcessPtr> procs) override;

        const SchedulerStats &stats() const override { return stats_; }
        void finalize_stats(Tick total_ticks) override;

        // -----------------------------------------------------------------------
        // Adaptive intelligence API (called by monitor thread)
        // -----------------------------------------------------------------------

        // Main evaluation entry point: sample → classify → score → maybe switch
        void evaluate_and_adapt(Tick current_time);

        // Feed a newly completed process into the metrics pipeline
        void record_completion(ProcessPtr proc, Tick wait, Tick turnaround, Tick response);

        // Feed CPU utilization sample (called by CPU thread each tick)
        void record_cpu_sample(bool was_busy);

        // -----------------------------------------------------------------------
        // Introspection / reporting
        // -----------------------------------------------------------------------
        const WorkloadMetrics &current_metrics() const;
        const std::vector<SwitchRecord> &switch_history() const;
        SchedulerPolicy active_policy() const;
        IScheduler *active_scheduler() const;

        // Human-readable decision trace (last N evaluations)
        std::string decision_trace(int n = 10) const;

    private:
        // -----------------------------------------------------------------------
        // Child schedulers (always alive; only one is "active" at a time)
        // -----------------------------------------------------------------------
        std::unique_ptr<FCFSScheduler> fcfs_;
        std::unique_ptr<RRScheduler> rr_;
        std::unique_ptr<PriorityScheduler> priority_;
        std::unique_ptr<MLFQScheduler> mlfq_;

        IScheduler *active_ = nullptr;
        mutable std::mutex scheduler_mtx_;

        // -----------------------------------------------------------------------
        // Metric state
        // -----------------------------------------------------------------------
        WorkloadMetrics current_metrics_;
        mutable std::mutex metrics_mtx_;

        // Circular history buffer of the last HISTORY_DEPTH metric snapshots
        std::deque<WorkloadMetrics> metrics_history_;

        // Raw accumulators between evaluation windows
        struct RawAccumulators
        {
            uint64_t cpu_busy_ticks = 0;
            uint64_t cpu_total_ticks = 0;
            uint64_t completions = 0;
            uint64_t ctx_switches = 0;
            double burst_sum = 0.0;
            double wait_sum = 0.0;
            double response_sum = 0.0;
            uint64_t starvation_count = 0;
            uint64_t interactive_count = 0;
            uint64_t total_processes = 0;
        };
        RawAccumulators accum_;
        mutable std::mutex accum_mtx_;

        // -----------------------------------------------------------------------
        // Switch governance
        // -----------------------------------------------------------------------
        Tick last_switch_tick_ = 0;
        bool in_cooldown_ = false;
        Tick cooldown_expires_at_ = 0;

        mutable std::mutex switch_history_mtx_;
        std::vector<SwitchRecord> switch_history_;

        // -----------------------------------------------------------------------
        // Internal algorithms
        // -----------------------------------------------------------------------

        // Step 1 — build WorkloadMetrics from accumulators + active child stats
        WorkloadMetrics sample_metrics(Tick now) const;

        // Step 2 — classify workload type from metric vector
        WorkloadType classify_workload(const WorkloadMetrics &m) const;

        // Step 3 — score all candidate policies
        std::vector<PolicyScore> score_policies(const WorkloadMetrics &m) const;

        // Step 4 — select best policy respecting hysteresis
        std::optional<PolicyScore> select_policy(const std::vector<PolicyScore> &scores) const;

        // Step 5 — execute migration to new policy
        void migrate_to(SchedulerPolicy target, const WorkloadMetrics &m,
                        const std::string &reason, double confidence);

        // Return child scheduler pointer by policy enum
        IScheduler *scheduler_for(SchedulerPolicy p) const;

        // EMA update helper
        static double ema(double current, double new_sample, double alpha)
        {
            return (1.0 - alpha) * current + alpha * new_sample;
        }

        // Trending: is a metric rising over the last N samples?
        bool is_trending_up(std::function<double(const WorkloadMetrics &)> getter,
                            int window = HISTORY_DEPTH) const;

        // -----------------------------------------------------------------------
        // Logging
        // -----------------------------------------------------------------------
        struct DecisionLog
        {
            Tick at;
            WorkloadMetrics metrics;
            std::vector<PolicyScore> scores;
            bool switched;
            std::string note;
        };
        mutable std::mutex log_mtx_;
        std::deque<DecisionLog> decision_log_;
    };

} // namespace adaptive_sched