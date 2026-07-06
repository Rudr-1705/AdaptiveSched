#pragma once

#include "../core/process.h"
#include <string>
#include <vector>
#include <random>
#include <functional>

namespace adaptive_sched
{

    // ---------------------------------------------------------------------------
    // ProcessDescriptor — lightweight specification for a process before creation
    // ---------------------------------------------------------------------------
    struct ProcessDescriptor
    {
        PID pid = 0;
        std::string name;
        Tick arrival = 0;
        Tick burst = 0;
        int priority = 0;
        bool interactive = false;
    };

    // ---------------------------------------------------------------------------
    // WorkloadProfile — describes the statistical shape of a workload
    // ---------------------------------------------------------------------------
    struct WorkloadProfile
    {
        std::string name;
        WorkloadType type;

        // Burst time distribution parameters
        double burst_mean = 8.0;
        double burst_stddev = 4.0;
        Tick burst_min = 1;
        Tick burst_max = 50;

        // Inter-arrival time distribution (exponential)
        double arrival_rate = 0.3; // processes per tick (lambda)

        // Priority distribution
        int priority_min = 0;
        int priority_max = 10;

        // Fraction of processes flagged interactive
        double interactive_fraction = 0.0;

        // Total processes to generate (0 = unlimited)
        int total_count = 50;
    };

    // ---------------------------------------------------------------------------
    // WorkloadLoader — reads workload descriptors from CSV files
    //
    // CSV format:
    //   pid,name,arrival,burst,priority,interactive
    //   1,proc_1,0,10,5,0
    //   ...
    // ---------------------------------------------------------------------------
    class WorkloadLoader
    {
    public:
        static std::vector<ProcessDescriptor> from_csv(const std::string &path);
        static void to_csv(const std::vector<ProcessDescriptor> &desc,
                           const std::string &path);

        // Built-in workload generators (no file needed)
        static std::vector<ProcessDescriptor> generate(const WorkloadProfile &profile,
                                                       uint32_t seed = 42);

        // Predefined profiles
        static WorkloadProfile interactive_profile();
        static WorkloadProfile cpu_bound_profile();
        static WorkloadProfile mixed_profile();
        static WorkloadProfile stress_profile();
        static WorkloadProfile batch_profile();

    private:
        static std::mt19937 &rng(uint32_t seed = 0);
    };

    // ---------------------------------------------------------------------------
    // WorkloadFactory — converts descriptors into live Process objects
    // ---------------------------------------------------------------------------
    class WorkloadFactory
    {
    public:
        static ProcessPtr make(const ProcessDescriptor &desc);
        static std::vector<ProcessPtr> make_all(const std::vector<ProcessDescriptor> &descs);
    };

} // namespace adaptive_sched