#include "../../include/metrics/statistics.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <cmath>
#include <numeric>
#include <algorithm>

namespace adaptive_sched
{

    StatisticsEngine::StatisticsEngine() = default;

    // ============================================================================
    // Data ingest
    // ============================================================================
    void StatisticsEngine::record_process_completion(ProcessPtr proc, SchedulerPolicy by)
    {
        std::lock_guard<std::mutex> lk(mtx_);

        ProcessRecord rec{
            proc->pid,
            proc->name,
            proc->arrival_time,
            proc->burst_time,
            proc->start_time,
            proc->completion_time,
            proc->waiting_time,
            proc->turnaround_time,
            proc->response_time,
            proc->priority,
            proc->queue_level,
            proc->ctx_switch.switch_count,
            proc->is_interactive,
            proc->aging.starvation_ticks,
            by};
        completed_.push_back(rec);
        policy_completions_[by]++;

        timeline_.push_back({proc->completion_time, true, proc->pid});
        if (timeline_.size() > 10000)
            timeline_.pop_front(); // cap timeline
    }

    void StatisticsEngine::record_context_switch(Tick at)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ctx_switches_++;
    }

    void StatisticsEngine::record_scheduler_switch(const SwitchRecord &rec)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        switch_log_.push_back(rec);
    }

    void StatisticsEngine::record_idle_tick(Tick at)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        idle_ticks_++;
        total_ticks_++;
        timeline_.push_back({at, false, 0});
    }

    void StatisticsEngine::record_busy_tick(Tick at)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        busy_ticks_++;
        total_ticks_++;
    }

    void StatisticsEngine::record_starvation_event(PID pid, Tick at)
    {
        // Could extend to track per-process starvation history
        (void)pid;
        (void)at;
    }

    // ============================================================================
    // Live snapshots
    // ============================================================================
    double StatisticsEngine::live_avg_waiting_time() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (completed_.empty())
            return 0.0;
        double sum = 0;
        for (const auto &r : completed_)
            sum += r.waiting_time;
        return sum / completed_.size();
    }

    double StatisticsEngine::live_avg_turnaround_time() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (completed_.empty())
            return 0.0;
        double sum = 0;
        for (const auto &r : completed_)
            sum += r.turnaround_time;
        return sum / completed_.size();
    }

    double StatisticsEngine::live_avg_response_time() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (completed_.empty())
            return 0.0;
        double sum = 0;
        for (const auto &r : completed_)
            sum += r.response_time;
        return sum / completed_.size();
    }

    double StatisticsEngine::live_throughput(Tick window) const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (total_ticks_ == 0)
            return 0.0;
        return static_cast<double>(completed_.size()) / static_cast<double>(total_ticks_);
    }

    double StatisticsEngine::live_cpu_utilization() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (total_ticks_ == 0)
            return 0.0;
        return static_cast<double>(busy_ticks_) / static_cast<double>(total_ticks_);
    }

    double StatisticsEngine::live_fairness_index() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (completed_.size() < 2)
            return 1.0;
        double sum = 0, sumSq = 0;
        for (const auto &r : completed_)
        {
            double w = static_cast<double>(r.waiting_time);
            sum += w;
            sumSq += w * w;
        }
        double n = completed_.size();
        return (sumSq < 1e-9) ? 1.0 : (sum * sum) / (n * sumSq);
    }

    std::size_t StatisticsEngine::completed_count() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return completed_.size();
    }

    // ============================================================================
    // Reports
    // ============================================================================
    void StatisticsEngine::print_summary(const std::string &scenario_name) const
    {
        std::lock_guard<std::mutex> lk(mtx_);

        print_separator('=');
        std::cout << center("SIMULATION SUMMARY: " + scenario_name, 80) << "\n";
        print_separator('=');

        if (completed_.empty())
        {
            std::cout << "  No processes completed.\n";
            return;
        }

        double wt = 0, tat = 0, rt = 0;
        for (const auto &r : completed_)
        {
            wt += r.waiting_time;
            tat += r.turnaround_time;
            rt += r.response_time;
        }
        double n = static_cast<double>(completed_.size());

        double util = (total_ticks_ > 0) ? static_cast<double>(busy_ticks_) / total_ticks_ : 0.0;
        double tput = (total_ticks_ > 0) ? n / static_cast<double>(total_ticks_) : 0.0;

        // Jain's fairness
        double sum_w = wt, sum_sq = 0;
        for (const auto &r : completed_)
        {
            double w = r.waiting_time;
            sum_sq += w * w;
        }
        double jain = (sum_sq < 1e-9) ? 1.0 : (sum_w * sum_w) / (n * sum_sq);

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  Processes completed  : " << (int)n << "\n"
                  << "  Total simulation time: " << total_ticks_ << " ticks\n"
                  << "  Avg Waiting Time     : " << wt / n << " ticks\n"
                  << "  Avg Turnaround Time  : " << tat / n << " ticks\n"
                  << "  Avg Response Time    : " << rt / n << " ticks\n"
                  << "  CPU Utilization      : " << util * 100.0 << "%\n"
                  << "  Throughput           : " << tput << " proc/tick\n"
                  << "  Context Switches     : " << ctx_switches_ << "\n"
                  << "  Jain's Fairness Index: " << jain << "\n"
                  << "  Scheduler Switches   : " << switch_log_.size() << "\n";

        if (!policy_completions_.empty())
        {
            std::cout << "\n  Completions by policy:\n";
            for (const auto &[pol, cnt] : policy_completions_)
            {
                std::cout << "    " << std::setw(12) << to_string(pol) << " : " << cnt << "\n";
            }
        }

        print_separator('-');
    }

    void StatisticsEngine::print_per_process_table() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (completed_.empty())
            return;

        print_separator('=');
        std::cout << center("PER-PROCESS METRICS", 80) << "\n";
        print_separator('-');
        std::cout << std::left
                  << std::setw(6) << "PID"
                  << std::setw(14) << "Name"
                  << std::setw(8) << "Arr"
                  << std::setw(8) << "Burst"
                  << std::setw(8) << "Start"
                  << std::setw(8) << "Done"
                  << std::setw(8) << "WT"
                  << std::setw(8) << "TAT"
                  << std::setw(8) << "RT"
                  << std::setw(6) << "Pri"
                  << std::setw(6) << "CTX"
                  << "Scheduled-by\n";
        print_separator('-');

        for (const auto &r : completed_)
        {
            std::cout << std::left
                      << std::setw(6) << r.pid
                      << std::setw(14) << r.name.substr(0, 13)
                      << std::setw(8) << r.arrival_time
                      << std::setw(8) << r.burst_time
                      << std::setw(8) << (r.start_time == UINT64_MAX ? 0 : r.start_time)
                      << std::setw(8) << r.completion_time
                      << std::setw(8) << r.waiting_time
                      << std::setw(8) << r.turnaround_time
                      << std::setw(8) << r.response_time
                      << std::setw(6) << r.priority
                      << std::setw(6) << r.ctx_switch_count
                      << to_string(r.scheduled_by) << "\n";
        }
        print_separator('=');
    }

    void StatisticsEngine::print_scheduler_transition_log() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (switch_log_.empty())
        {
            std::cout << "  [No scheduler transitions occurred]\n";
            return;
        }

        print_separator('=');
        std::cout << center("SCHEDULER TRANSITION LOG", 80) << "\n";
        print_separator('-');

        for (const auto &rec : switch_log_)
        {
            std::cout << "  Tick " << std::setw(6) << rec.at
                      << "  " << std::setw(10) << to_string(rec.from)
                      << " --> " << std::setw(10) << to_string(rec.to)
                      << "  conf=" << std::fixed << std::setprecision(2) << rec.confidence
                      << "  [" << to_string(rec.metrics_snapshot.classified_type) << "]"
                      << "\n          reason: " << rec.reason << "\n";
        }
        print_separator('=');
    }

    void StatisticsEngine::export_csv(const std::string &path) const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::ofstream f(path);
        if (!f)
        {
            std::cerr << "Cannot open " << path << " for writing\n";
            return;
        }
        f << "pid,name,arrival,burst,start,completion,waiting,turnaround,response,"
             "priority,queue_level,ctx_switches,interactive,starvation_ticks,scheduled_by\n";
        for (const auto &r : completed_)
        {
            f << r.pid << ","
              << r.name << ","
              << r.arrival_time << ","
              << r.burst_time << ","
              << (r.start_time == UINT64_MAX ? 0 : r.start_time) << ","
              << r.completion_time << ","
              << r.waiting_time << ","
              << r.turnaround_time << ","
              << r.response_time << ","
              << r.priority << ","
              << r.queue_level << ","
              << r.ctx_switch_count << ","
              << r.is_interactive << ","
              << r.starvation_ticks << ","
              << to_string(r.scheduled_by) << "\n";
        }
        std::cout << "  Exported metrics to: " << path << "\n";
    }

    void StatisticsEngine::export_timeline(const std::string &path) const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::ofstream f(path);
        if (!f)
            return;
        f << "tick,busy,pid\n";
        for (const auto &e : timeline_)
        {
            f << e.at << "," << e.busy << "," << e.pid << "\n";
        }
        std::cout << "  Exported timeline to: " << path << "\n";
    }

    void StatisticsEngine::compare(const std::vector<StatisticsEngine> &runs,
                                   const std::vector<std::string> &labels)
    {
        if (runs.empty())
            return;
        std::cout << "\n"
                  << std::string(90, '=') << "\n";
        std::cout << "  COMPARATIVE ANALYSIS\n";
        std::cout << std::string(90, '-') << "\n";
        std::cout << std::left
                  << std::setw(16) << "Scenario"
                  << std::setw(10) << "Avg WT"
                  << std::setw(10) << "Avg TAT"
                  << std::setw(10) << "Avg RT"
                  << std::setw(10) << "Util%"
                  << std::setw(10) << "Tput"
                  << std::setw(10) << "Fairness"
                  << "\n";
        std::cout << std::string(90, '-') << "\n";

        for (std::size_t i = 0; i < runs.size(); ++i)
        {
            const std::string &label = (i < labels.size()) ? labels[i] : "run_" + std::to_string(i);
            double wt = runs[i].live_avg_waiting_time();
            double tat = runs[i].live_avg_turnaround_time();
            double rt = runs[i].live_avg_response_time();
            double ut = runs[i].live_cpu_utilization() * 100.0;
            double tp = runs[i].live_throughput();
            double fi = runs[i].live_fairness_index();

            std::cout << std::left << std::fixed << std::setprecision(2)
                      << std::setw(16) << label
                      << std::setw(10) << wt
                      << std::setw(10) << tat
                      << std::setw(10) << rt
                      << std::setw(10) << ut
                      << std::setw(10) << tp
                      << std::setw(10) << fi
                      << "\n";
        }
        std::cout << std::string(90, '=') << "\n";
    }

    // ============================================================================
    // Formatting helpers
    // ============================================================================
    void StatisticsEngine::print_separator(char c, int width) const
    {
        std::cout << std::string(width, c) << "\n";
    }

    std::string StatisticsEngine::center(const std::string &s, int width) const
    {
        int pad = std::max(0, width - static_cast<int>(s.size()));
        int lpad = pad / 2;
        int rpad = pad - lpad;
        return std::string(lpad, ' ') + s + std::string(rpad, ' ');
    }

} // namespace adaptive_sched