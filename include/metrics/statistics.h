#pragma once

#include "../core/process.h"
#include "../core/clock.h"
#include "../scheduler/adaptive.h"

#include <vector>
#include <mutex>
#include <string>
#include <map>
#include <deque>
#include <fstream>

namespace adaptive_sched
{

    // ---------------------------------------------------------------------------
    // ProcessRecord — immutable snapshot of a completed process
    // ---------------------------------------------------------------------------
    struct ProcessRecord
    {
        PID pid;
        std::string name;
        Tick arrival_time;
        Tick burst_time;
        Tick start_time;
        Tick completion_time;
        Tick waiting_time;
        Tick turnaround_time;
        Tick response_time;
        int priority;
        int queue_level;
        uint32_t ctx_switch_count;
        bool is_interactive;
        int starvation_ticks;
        SchedulerPolicy scheduled_by;
    };

    // ---------------------------------------------------------------------------
    // StatisticsEngine — collects, aggregates and reports simulation metrics
    //
    // Thread-safe; can be queried at any time for live snapshots.
    // ---------------------------------------------------------------------------
    class StatisticsEngine
    {
    public:
        StatisticsEngine();

        // -----------------------------------------------------------------------
        // Data ingest
        // -----------------------------------------------------------------------
        void record_process_completion(ProcessPtr proc, SchedulerPolicy by);
        void record_context_switch(Tick at);
        void record_scheduler_switch(const SwitchRecord &rec);
        void record_idle_tick(Tick at);
        void record_busy_tick(Tick at);
        void record_starvation_event(PID pid, Tick at);

        // -----------------------------------------------------------------------
        // Live snapshots
        // -----------------------------------------------------------------------
        double live_avg_waiting_time() const;
        double live_avg_turnaround_time() const;
        double live_avg_response_time() const;
        double live_throughput(Tick window = 50) const;
        double live_cpu_utilization() const;
        double live_fairness_index() const; // Jain's fairness index on waiting times

        std::size_t completed_count() const;

        // -----------------------------------------------------------------------
        // Final report generation
        // -----------------------------------------------------------------------
        void print_summary(const std::string &scenario_name) const;
        void print_per_process_table() const;
        void print_scheduler_transition_log() const;

        void export_csv(const std::string &path) const;
        void export_timeline(const std::string &path) const;

        // -----------------------------------------------------------------------
        // Comparative analysis (called after multiple scheduler runs)
        // -----------------------------------------------------------------------
        static void compare(const std::vector<StatisticsEngine> &runs,
                            const std::vector<std::string> &labels);

    private:
        mutable std::mutex mtx_;

        std::vector<ProcessRecord> completed_;
        std::vector<SwitchRecord> switch_log_;

        // Timeline tick data
        struct TickEntry
        {
            Tick at;
            bool busy;
            PID pid;
        };
        std::deque<TickEntry> timeline_;

        uint64_t total_ticks_ = 0;
        uint64_t busy_ticks_ = 0;
        uint64_t idle_ticks_ = 0;
        uint64_t ctx_switches_ = 0;

        // Per-policy completion counts for comparative analysis
        std::map<SchedulerPolicy, uint64_t> policy_completions_;

        // -----------------------------------------------------------------------
        // Helpers
        // -----------------------------------------------------------------------
        void print_separator(char c = '-', int width = 80) const;
        std::string center(const std::string &s, int width) const;
    };

} // namespace adaptive_sched