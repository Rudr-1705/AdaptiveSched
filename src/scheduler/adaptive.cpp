#include "../../include/scheduler/adaptive.h"
#include "../../include/core/clock.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cassert>

namespace adaptive_sched
{

    // ============================================================================
    // Constructor
    // ============================================================================
    AdaptiveScheduler::AdaptiveScheduler()
        : fcfs_(std::make_unique<FCFSScheduler>()), rr_(std::make_unique<RRScheduler>(4)), priority_(std::make_unique<PriorityScheduler>()), mlfq_(std::make_unique<MLFQScheduler>())
    {
        // Default active policy: MLFQ (best general-purpose starting point)
        active_ = mlfq_.get();

        std::cout << "[ADAPTIVE] Initialized. Starting policy: MLFQ\n";
    }

    // ============================================================================
    // IScheduler delegation
    // ============================================================================
    void AdaptiveScheduler::add_process(ProcessPtr proc)
    {
        std::lock_guard<std::mutex> lk(scheduler_mtx_);
        active_->add_process(proc);
        stats_.processes_handled.fetch_add(1, std::memory_order_relaxed);

        // Update interactive fraction accumulator
        {
            std::lock_guard<std::mutex> al(accum_mtx_);
            accum_.total_processes++;
            if (proc->is_interactive)
                accum_.interactive_count++;
        }
    }

    ProcessPtr AdaptiveScheduler::get_next_process()
    {
        std::lock_guard<std::mutex> lk(scheduler_mtx_);
        return active_->get_next_process();
    }

    bool AdaptiveScheduler::handle_preemption(ProcessPtr running, ProcessPtr incoming)
    {
        std::lock_guard<std::mutex> lk(scheduler_mtx_);
        bool preempted = active_->handle_preemption(running, incoming);
        if (preempted)
        {
            stats_.preemptions.fetch_add(1, std::memory_order_relaxed);
            stats_.context_switches.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> al(accum_mtx_);
            accum_.ctx_switches++;
        }
        return preempted;
    }

    void AdaptiveScheduler::on_process_complete(ProcessPtr proc)
    {
        std::lock_guard<std::mutex> lk(scheduler_mtx_);
        active_->on_process_complete(proc);
    }

    void AdaptiveScheduler::on_tick(Tick current_time)
    {
        {
            std::lock_guard<std::mutex> lk(scheduler_mtx_);
            active_->on_tick(current_time);
        }
        stats_.total_cpu_ticks.fetch_add(1, std::memory_order_relaxed);
    }

    std::size_t AdaptiveScheduler::ready_count() const
    {
        std::lock_guard<std::mutex> lk(scheduler_mtx_);
        return active_->ready_count();
    }

    bool AdaptiveScheduler::has_ready_work() const
    {
        std::lock_guard<std::mutex> lk(scheduler_mtx_);
        return active_->has_ready_work();
    }

    std::vector<ProcessPtr> AdaptiveScheduler::snapshot() const
    {
        std::lock_guard<std::mutex> lk(scheduler_mtx_);
        return active_->snapshot();
    }

    std::vector<ProcessPtr> AdaptiveScheduler::drain_queue()
    {
        std::lock_guard<std::mutex> lk(scheduler_mtx_);
        return active_->drain_queue();
    }

    void AdaptiveScheduler::import_queue(std::vector<ProcessPtr> procs)
    {
        std::lock_guard<std::mutex> lk(scheduler_mtx_);
        active_->import_queue(std::move(procs));
    }

    void AdaptiveScheduler::finalize_stats(Tick total_ticks)
    {
        std::lock_guard<std::mutex> lk(scheduler_mtx_);
        active_->finalize_stats(total_ticks);
        // Copy child stats into our own
        const auto &cs = active_->stats();
        stats_.avg_waiting_time = cs.avg_waiting_time;
        stats_.avg_turnaround_time = cs.avg_turnaround_time;
        stats_.avg_response_time = cs.avg_response_time;
        stats_.cpu_utilization = cs.cpu_utilization;
        stats_.throughput = cs.throughput;
        stats_.fairness_index = cs.fairness_index;
    }

    // ============================================================================
    // Telemetry feed-in (called from CPU thread and completion callbacks)
    // ============================================================================
    void AdaptiveScheduler::record_completion(ProcessPtr proc, Tick wait,
                                              Tick turnaround, Tick response)
    {
        std::lock_guard<std::mutex> al(accum_mtx_);
        accum_.completions++;
        accum_.burst_sum += static_cast<double>(proc->burst_time);
        accum_.wait_sum += static_cast<double>(wait);
        accum_.response_sum += static_cast<double>(response);
        if (proc->aging.starvation_ticks > STARVATION_TICK_LIMIT)
        {
            accum_.starvation_count++;
        }
    }

    void AdaptiveScheduler::record_cpu_sample(bool was_busy)
    {
        std::lock_guard<std::mutex> al(accum_mtx_);
        accum_.cpu_total_ticks++;
        if (was_busy)
            accum_.cpu_busy_ticks++;
    }

    // ============================================================================
    // Main adaptive evaluation (called by monitor thread)
    // ============================================================================
    void AdaptiveScheduler::evaluate_and_adapt(Tick current_time)
    {
        // Check cooldown
        if (current_time < cooldown_expires_at_)
            return;

        // Step 1: Sample metrics
        WorkloadMetrics m = sample_metrics(current_time);

        // Step 2: Classify workload
        m.classified_type = classify_workload(m);

        // Step 3: Score policies
        auto scores = score_policies(m);

        // Step 4: Select best policy
        auto best = select_policy(scores);

        bool switched = false;
        std::string note;

        if (best.has_value())
        {
            // Step 5: Migrate
            note = "Switch triggered: " + best->rationale;
            migrate_to(best->policy, m, best->rationale, best->confidence);
            switched = true;
        }
        else
        {
            note = "No switch: policy confidence below hysteresis threshold";
        }

        // Store metrics snapshot and update history
        {
            std::lock_guard<std::mutex> ml(metrics_mtx_);
            current_metrics_ = m;
            metrics_history_.push_back(m);
            if (metrics_history_.size() > static_cast<std::size_t>(HISTORY_DEPTH))
            {
                metrics_history_.pop_front();
            }
        }

        // Log this decision
        {
            std::lock_guard<std::mutex> ll(log_mtx_);
            decision_log_.push_back({current_time, m, scores, switched, note});
            if (decision_log_.size() > 64)
                decision_log_.pop_front();
        }

        // Reset per-window accumulators
        {
            std::lock_guard<std::mutex> al(accum_mtx_);
            accum_.cpu_busy_ticks = 0;
            accum_.cpu_total_ticks = 0;
            accum_.completions = 0;
            accum_.ctx_switches = 0;
            accum_.burst_sum = 0.0;
            accum_.wait_sum = 0.0;
            accum_.response_sum = 0.0;
            accum_.starvation_count = 0;
            // interactive/total counts persist for lifetime fraction
        }
    }

    // ============================================================================
    // Step 1 — sample_metrics
    // ============================================================================
    WorkloadMetrics AdaptiveScheduler::sample_metrics(Tick now_t) const
    {
        WorkloadMetrics m;
        m.sample_time = now_t;

        RawAccumulators snap;
        {
            std::lock_guard<std::mutex> al(accum_mtx_);
            snap = accum_;
        }

        // CPU utilization (within this window)
        double util = (snap.cpu_total_ticks > 0)
                          ? static_cast<double>(snap.cpu_busy_ticks) / snap.cpu_total_ticks
                          : 0.0;

        // Per-completion averages
        double avg_burst = (snap.completions > 0) ? snap.burst_sum / snap.completions : 0.0;
        double avg_wait = (snap.completions > 0) ? snap.wait_sum / snap.completions : 0.0;
        double avg_resp = (snap.completions > 0) ? snap.response_sum / snap.completions : 0.0;

        // Throughput
        double tput = static_cast<double>(snap.completions) / EVALUATION_INTERVAL;

        // Context switch rate
        double csr = static_cast<double>(snap.ctx_switches) / EVALUATION_INTERVAL;

        // Interactive fraction
        double ifrac = (snap.total_processes > 0)
                           ? static_cast<double>(snap.interactive_count) / snap.total_processes
                           : 0.0;

        // Queue depth
        double qdepth;
        {
            std::lock_guard<std::mutex> sl(scheduler_mtx_);
            qdepth = static_cast<double>(active_->ready_count());
        }

        // MLFQ level-0 fraction (how much interactive work is in top queue)
        double l0frac = 0.0;
        {
            std::lock_guard<std::mutex> sl(scheduler_mtx_);
            if (active_ == mlfq_.get())
            {
                auto sizes = mlfq_->level_sizes();
                std::size_t total_q = 0;
                for (auto s : sizes)
                    total_q += s;
                if (total_q > 0)
                    l0frac = static_cast<double>(sizes[0]) / total_q;
            }
        }

        // Starvation pressure
        double starvation = (snap.completions > 0)
                                ? static_cast<double>(snap.starvation_count) / snap.completions
                                : 0.0;

        // EMA updates
        {
            std::lock_guard<std::mutex> ml(metrics_mtx_);
            m.avg_burst_length = ema(current_metrics_.avg_burst_length, avg_burst, EMA_ALPHA);
            m.avg_waiting_time = ema(current_metrics_.avg_waiting_time, avg_wait, EMA_ALPHA);
            m.avg_response_time = ema(current_metrics_.avg_response_time, avg_resp, EMA_ALPHA);
            m.cpu_utilization = ema(current_metrics_.cpu_utilization, util, EMA_ALPHA);
            m.context_switch_rate = ema(current_metrics_.context_switch_rate, csr, EMA_ALPHA);
            m.throughput_rate = ema(current_metrics_.throughput_rate, tput, EMA_ALPHA);
            m.starvation_pressure = ema(current_metrics_.starvation_pressure, starvation, EMA_ALPHA);
            m.interactive_fraction = ema(current_metrics_.interactive_fraction, ifrac, EMA_ALPHA);
            m.queue_depth_avg = ema(current_metrics_.queue_depth_avg, qdepth, EMA_ALPHA);
            m.mlfq_level0_fraction = ema(current_metrics_.mlfq_level0_fraction, l0frac, EMA_ALPHA);
        }

        return m;
    }

    // ============================================================================
    // Step 2 — classify_workload
    // ============================================================================
    WorkloadType AdaptiveScheduler::classify_workload(const WorkloadMetrics &m) const
    {
        // Heuristic thresholds (tunable)
        constexpr double CPU_BURST_THRESHOLD = 12.0;    // avg burst > 12 → CPU-bound
        constexpr double INTERACTIVE_THRESHOLD = 0.4;   // >40% interactive fraction
        constexpr double STARVATION_THRESHOLD_V = 0.15; // >15% starvation → urgent
        constexpr double HIGH_UTIL_THRESHOLD = 0.85;    // >85% util → stress

        double cpu_score = 0.0;
        double int_score = 0.0;
        double batch_score = 0.0;

        // CPU-bound indicators
        if (m.avg_burst_length > CPU_BURST_THRESHOLD)
            cpu_score += 0.4;
        if (m.cpu_utilization > HIGH_UTIL_THRESHOLD)
            cpu_score += 0.3;
        if (m.context_switch_rate < 0.1)
            cpu_score += 0.2; // few switches = non-preemptive
        if (m.queue_depth_avg > 5.0)
            cpu_score += 0.1;

        // Interactive indicators
        if (m.interactive_fraction > INTERACTIVE_THRESHOLD)
            int_score += 0.5;
        if (m.avg_response_time < 5.0)
            int_score += 0.3;
        if (m.mlfq_level0_fraction > 0.5)
            int_score += 0.2;

        // Batch indicators
        if (m.avg_burst_length > 20.0)
            batch_score += 0.5;
        if (m.throughput_rate < 0.05)
            batch_score += 0.3;
        if (m.context_switch_rate < 0.05)
            batch_score += 0.2;

        // Normalize scores
        const WorkloadMetrics &mm = m;

        if (m.starvation_pressure > STARVATION_THRESHOLD_V)
        {
            // Starvation overrides classification — needs anti-starvation scheduler
            return WorkloadType::MIXED;
        }

        if (cpu_score > int_score && cpu_score > batch_score)
        {
            return (m.cpu_utilization > HIGH_UTIL_THRESHOLD)
                       ? WorkloadType::STRESS
                       : WorkloadType::CPU_BOUND;
        }
        if (int_score > cpu_score && int_score > batch_score)
        {
            return WorkloadType::INTERACTIVE;
        }
        if (batch_score > 0.6)
        {
            return WorkloadType::BATCH;
        }
        return WorkloadType::MIXED;
    }

    // ============================================================================
    // Step 3 — score_policies
    //
    // Each policy receives a confidence score [0,1] reflecting how well it is
    // expected to handle the current workload.  Scores are derived from:
    //   - Workload classification
    //   - Current metric values
    //   - Historical trends
    //   - Switching penalties (penalise recently exited policies)
    // ============================================================================
    std::vector<PolicyScore> AdaptiveScheduler::score_policies(const WorkloadMetrics &m) const
    {
        std::vector<PolicyScore> scores;

        auto current_policy = active_policy();

        // -----------------------------------------------------------------
        // FCFS score
        // -----------------------------------------------------------------
        {
            double s = 0.0;
            std::string rationale;

            // FCFS excels with batch, non-preemptive CPU-bound workloads
            if (m.classified_type == WorkloadType::BATCH)
            {
                s += 0.6;
                rationale += "batch workload; ";
            }
            if (m.classified_type == WorkloadType::CPU_BOUND)
            {
                s += 0.4;
                rationale += "cpu-bound workload; ";
            }

            // Penalise FCFS when starvation is detected
            if (m.starvation_pressure > 0.1)
            {
                s -= 0.4;
                rationale += "starvation risk; ";
            }

            // Penalise when interactive processes are present
            if (m.interactive_fraction > 0.3)
            {
                s -= 0.3;
                rationale += "interactive processes present; ";
            }

            // Convoy effect mitigation: if queue depth is high and burst long, FCFS is bad
            if (m.queue_depth_avg > 8.0 && m.avg_burst_length > 10.0)
            {
                s -= 0.2;
            }

            // Bonus if already running FCFS (avoid unnecessary switch)
            if (current_policy == SchedulerPolicy::FCFS)
                s += 0.05;

            s = std::max(0.0, std::min(1.0, s));
            scores.push_back({SchedulerPolicy::FCFS, s, rationale.empty() ? "baseline" : rationale});
        }

        // -----------------------------------------------------------------
        // RR score
        // -----------------------------------------------------------------
        {
            double s = 0.0;
            std::string rationale;

            if (m.classified_type == WorkloadType::INTERACTIVE)
            {
                s += 0.55;
                rationale += "interactive workload; ";
            }
            if (m.avg_response_time > 15.0)
            {
                s += 0.25;
                rationale += "high response time; ";
            }
            if (m.interactive_fraction > 0.4)
            {
                s += 0.20;
                rationale += "high interactive fraction; ";
            }

            // RR performs poorly for pure CPU-bound (context switch overhead)
            if (m.classified_type == WorkloadType::CPU_BOUND)
            {
                s -= 0.35;
                rationale += "cpu-bound workload; ";
            }
            if (m.classified_type == WorkloadType::BATCH)
            {
                s -= 0.30;
            }

            // High context switch rate may mean quantum is too small → already in RR, needs tuning
            if (m.context_switch_rate > 0.5 && current_policy == SchedulerPolicy::ROUND_ROBIN)
            {
                s -= 0.15;
                rationale += "excessive context switches; ";
            }

            if (current_policy == SchedulerPolicy::ROUND_ROBIN)
                s += 0.05;

            s = std::max(0.0, std::min(1.0, s));
            scores.push_back({SchedulerPolicy::ROUND_ROBIN, s, rationale.empty() ? "baseline" : rationale});
        }

        // -----------------------------------------------------------------
        // Priority score
        // -----------------------------------------------------------------
        {
            double s = 0.0;
            std::string rationale;

            if (m.starvation_pressure > 0.10)
            {
                s += 0.45;
                rationale += "starvation detected; ";
            }
            if (m.classified_type == WorkloadType::MIXED)
            {
                s += 0.20;
                rationale += "mixed workload; ";
            }

            // Priority is great when different urgency classes need differentiation
            if (m.avg_waiting_time > 20.0)
            {
                s += 0.15;
                rationale += "high avg wait; ";
            }
            if (m.queue_depth_avg > 4.0)
            {
                s += 0.10;
                rationale += "queue depth high; ";
            }

            // Trending starvation pressure is a strong signal
            if (is_trending_up([](const WorkloadMetrics &mm)
                               { return mm.starvation_pressure; }))
            {
                s += 0.20;
                rationale += "starvation trending up; ";
            }

            // Penalise for CPU-bound + stress (aging overhead wasted)
            if (m.classified_type == WorkloadType::STRESS)
            {
                s -= 0.15;
            }

            if (current_policy == SchedulerPolicy::PRIORITY)
                s += 0.05;

            s = std::max(0.0, std::min(1.0, s));
            scores.push_back({SchedulerPolicy::PRIORITY, s, rationale.empty() ? "baseline" : rationale});
        }

        // -----------------------------------------------------------------
        // MLFQ score — general-purpose; highest baseline
        // -----------------------------------------------------------------
        {
            double s = 0.15; // MLFQ has a non-zero baseline (good default)
            std::string rationale = "good general-purpose scheduler; ";

            if (m.classified_type == WorkloadType::MIXED)
            {
                s += 0.40;
                rationale += "mixed workload; ";
            }
            if (m.classified_type == WorkloadType::INTERACTIVE)
            {
                s += 0.30;
                rationale += "interactive workload; ";
            }
            if (m.starvation_pressure > 0.05)
            {
                s += 0.15;
                rationale += "starvation risk (boost handles it); ";
            }

            // MLFQ handles heterogeneous workloads best
            if (m.interactive_fraction > 0.2 && m.interactive_fraction < 0.8)
            {
                s += 0.10;
                rationale += "heterogeneous process mix; ";
            }

            // Penalise for pure batch / CPU-bound (overhead of level management)
            if (m.classified_type == WorkloadType::BATCH)
            {
                s -= 0.20;
            }
            if (m.classified_type == WorkloadType::CPU_BOUND)
            {
                s -= 0.10;
            }

            if (current_policy == SchedulerPolicy::MLFQ)
                s += 0.05;

            s = std::max(0.0, std::min(1.0, s));
            scores.push_back({SchedulerPolicy::MLFQ, s, rationale});
        }

        // Sort descending by confidence
        std::sort(scores.begin(), scores.end(),
                  [](const PolicyScore &a, const PolicyScore &b)
                  { return a.confidence > b.confidence; });

        return scores;
    }

    // ============================================================================
    // Step 4 — select_policy (with hysteresis)
    // ============================================================================
    std::optional<PolicyScore>
    AdaptiveScheduler::select_policy(const std::vector<PolicyScore> &scores) const
    {
        if (scores.empty())
            return std::nullopt;

        const auto &best = scores.front();
        auto current_pol = active_policy();

        // Don't switch to the same policy
        if (best.policy == current_pol)
            return std::nullopt;

        // Find current policy's score
        double current_score = 0.0;
        for (const auto &s : scores)
        {
            if (s.policy == current_pol)
            {
                current_score = s.confidence;
                break;
            }
        }

        // Require hysteresis margin
        if (best.confidence - current_score < CONFIDENCE_HYSTERESIS)
        {
            return std::nullopt;
        }

        return best;
    }

    // ============================================================================
    // Step 5 — migrate_to
    // ============================================================================
    void AdaptiveScheduler::migrate_to(SchedulerPolicy target,
                                       const WorkloadMetrics &m,
                                       const std::string &reason,
                                       double confidence)
    {
        std::lock_guard<std::mutex> lk(scheduler_mtx_);

        IScheduler *new_sched = scheduler_for(target);
        if (!new_sched || new_sched == active_)
            return;

        // Drain current scheduler's ready queue
        auto migrating = active_->drain_queue();

        SchedulerPolicy from = active_->policy();

        // Hand off processes to new scheduler
        new_sched->import_queue(std::move(migrating));
        active_ = new_sched;

        Tick t = now();
        cooldown_expires_at_ = t + COOLDOWN_TICKS;
        last_switch_tick_ = t;

        // Record the switch
        SwitchRecord rec{t, from, target, m, reason, confidence};
        {
            std::lock_guard<std::mutex> sl(switch_history_mtx_);
            switch_history_.push_back(rec);
        }

        std::cout << std::string(70, '=') << "\n"
                  << "[ADAPTIVE] *** POLICY SWITCH ***\n"
                  << "  Tick       : " << t << "\n"
                  << "  From       : " << to_string(from) << "\n"
                  << "  To         : " << to_string(target) << "\n"
                  << "  Confidence : " << std::fixed << std::setprecision(3) << confidence << "\n"
                  << "  Workload   : " << to_string(m.classified_type) << "\n"
                  << "  Reason     : " << reason << "\n"
                  << "  Metrics    :\n"
                  << "    avg_burst=" << std::setprecision(1) << m.avg_burst_length
                  << "  avg_wait=" << m.avg_waiting_time
                  << "  cpu_util=" << std::setprecision(2) << m.cpu_utilization << "\n"
                  << "    starvation=" << std::setprecision(3) << m.starvation_pressure
                  << "  interactive=" << m.interactive_fraction
                  << "  csr=" << m.context_switch_rate << "\n"
                  << std::string(70, '=') << "\n";
    }

    // ============================================================================
    // Helpers
    // ============================================================================
    IScheduler *AdaptiveScheduler::scheduler_for(SchedulerPolicy p) const
    {
        switch (p)
        {
        case SchedulerPolicy::FCFS:
            return fcfs_.get();
        case SchedulerPolicy::ROUND_ROBIN:
            return rr_.get();
        case SchedulerPolicy::PRIORITY:
            return priority_.get();
        case SchedulerPolicy::MLFQ:
            return mlfq_.get();
        default:
            return nullptr;
        }
    }

    bool AdaptiveScheduler::is_trending_up(
        std::function<double(const WorkloadMetrics &)> getter, int window) const
    {
        std::lock_guard<std::mutex> ml(metrics_mtx_);
        if (static_cast<int>(metrics_history_.size()) < 2)
            return false;

        int n = std::min(window, static_cast<int>(metrics_history_.size()));
        auto begin = metrics_history_.end() - n;

        double prev = getter(*begin);
        int increases = 0;
        for (auto it = begin + 1; it != metrics_history_.end(); ++it)
        {
            double cur = getter(*it);
            if (cur > prev)
                ++increases;
            prev = cur;
        }
        // Trending up if more than half the steps increased
        return increases > (n - 1) / 2;
    }

    // ============================================================================
    // Introspection
    // ============================================================================
    const WorkloadMetrics &AdaptiveScheduler::current_metrics() const
    {
        std::lock_guard<std::mutex> lk(metrics_mtx_);
        return current_metrics_;
    }

    const std::vector<SwitchRecord> &AdaptiveScheduler::switch_history() const
    {
        std::lock_guard<std::mutex> lk(switch_history_mtx_);
        return switch_history_;
    }

    SchedulerPolicy AdaptiveScheduler::active_policy() const
    {
        std::lock_guard<std::mutex> lk(scheduler_mtx_);
        return active_->policy();
    }

    IScheduler *AdaptiveScheduler::active_scheduler() const
    {
        std::lock_guard<std::mutex> lk(scheduler_mtx_);
        return active_;
    }

    std::string AdaptiveScheduler::decision_trace(int n) const
    {
        std::ostringstream oss;
        std::lock_guard<std::mutex> ll(log_mtx_);

        int start = std::max(0, static_cast<int>(decision_log_.size()) - n);
        oss << "=== Adaptive Decision Trace (last " << n << " evaluations) ===\n";
        for (int i = start; i < static_cast<int>(decision_log_.size()); ++i)
        {
            const auto &d = decision_log_[i];
            oss << "  Tick " << std::setw(6) << d.at
                << " | Workload=" << to_string(d.metrics.classified_type)
                << " | Switched=" << (d.switched ? "YES" : "no ")
                << " | " << d.note << "\n";
            if (!d.scores.empty())
            {
                oss << "    Scores:";
                for (const auto &sc : d.scores)
                {
                    oss << " [" << to_string(sc.policy) << "="
                        << std::fixed << std::setprecision(2) << sc.confidence << "]";
                }
                oss << "\n";
            }
        }
        return oss.str();
    }

} // namespace adaptive_sched