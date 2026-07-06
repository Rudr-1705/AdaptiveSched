#include "../../include/simulation/workload.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <random>
#include <cmath>

namespace adaptive_sched
{

    // ============================================================================
    // WorkloadLoader::from_csv
    // Expected format: pid,name,arrival,burst,priority,interactive
    // Lines starting with '#' are comments; first line (header) is skipped.
    // ============================================================================
    std::vector<ProcessDescriptor> WorkloadLoader::from_csv(const std::string &path)
    {
        std::ifstream f(path);
        if (!f.is_open())
            throw std::runtime_error("Cannot open workload file: " + path);

        std::vector<ProcessDescriptor> descs;
        std::string line;
        bool first = true;

        while (std::getline(f, line))
        {
            // Strip CR (Windows line endings)
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            if (first)
            {
                first = false;
                // Skip header line (starts with non-digit)
                if (!std::isdigit(static_cast<unsigned char>(line[0])))
                    continue;
            }

            std::istringstream ss(line);
            std::string tok;
            ProcessDescriptor d;

            try
            {
                std::getline(ss, tok, ',');
                d.pid = static_cast<PID>(std::stoul(tok));
                std::getline(ss, tok, ',');
                d.name = tok;
                std::getline(ss, tok, ',');
                d.arrival = static_cast<Tick>(std::stoull(tok));
                std::getline(ss, tok, ',');
                d.burst = static_cast<Tick>(std::stoull(tok));
                std::getline(ss, tok, ',');
                d.priority = std::stoi(tok);
                if (std::getline(ss, tok, ','))
                    d.interactive = (tok == "1" || tok == "true");
            }
            catch (const std::exception &e)
            {
                std::cerr << "[WorkloadLoader] Skipping malformed line: " << line
                          << " (" << e.what() << ")\n";
                continue;
            }

            descs.push_back(d);
        }

        std::cout << "[WorkloadLoader] Loaded " << descs.size()
                  << " processes from " << path << "\n";
        return descs;
    }

    void WorkloadLoader::to_csv(const std::vector<ProcessDescriptor> &descs,
                                const std::string &path)
    {
        std::ofstream f(path);
        if (!f)
        {
            std::cerr << "[WorkloadLoader] Cannot write to " << path << "\n";
            return;
        }
        f << "pid,name,arrival,burst,priority,interactive\n";
        for (const auto &d : descs)
        {
            f << d.pid << ","
              << d.name << ","
              << d.arrival << ","
              << d.burst << ","
              << d.priority << ","
              << (d.interactive ? 1 : 0) << "\n";
        }
    }

    // ============================================================================
    // WorkloadLoader::generate
    // Procedurally generates processes according to a WorkloadProfile using a
    // seeded RNG so runs are reproducible.
    // ============================================================================
    std::vector<ProcessDescriptor> WorkloadLoader::generate(const WorkloadProfile &profile,
                                                            uint32_t seed)
    {
        std::mt19937 engine(seed);

        // Burst time: normal distribution, clamped to [burst_min, burst_max]
        std::normal_distribution<double> burst_dist(profile.burst_mean, profile.burst_stddev);
        // Inter-arrival time: exponential (Poisson process)
        std::exponential_distribution<double> arr_dist(profile.arrival_rate);
        // Priority: uniform integer
        std::uniform_int_distribution<int> pri_dist(profile.priority_min, profile.priority_max);
        // Interactive flag: Bernoulli
        std::bernoulli_distribution interactive_dist(profile.interactive_fraction);

        std::vector<ProcessDescriptor> descs;
        Tick current_arrival = 0;
        int count = (profile.total_count > 0) ? profile.total_count : 50;

        for (int i = 0; i < count; ++i)
        {
            ProcessDescriptor d;
            d.pid = static_cast<PID>(i + 1);
            d.name = "proc_" + std::to_string(i + 1);

            // Arrival time: accumulate inter-arrivals
            double inter_arr = arr_dist(engine);
            current_arrival += static_cast<Tick>(std::max(1.0, inter_arr));
            d.arrival = current_arrival;

            // Burst time: clamp to [burst_min, burst_max]
            double burst_raw = burst_dist(engine);
            Tick burst = static_cast<Tick>(std::round(burst_raw));
            burst = std::max(profile.burst_min, std::min(profile.burst_max, burst));
            d.burst = burst;

            d.priority = pri_dist(engine);
            d.interactive = interactive_dist(engine);

            descs.push_back(d);
        }

        return descs;
    }

    // ============================================================================
    // Predefined profiles
    // ============================================================================
    WorkloadProfile WorkloadLoader::interactive_profile()
    {
        WorkloadProfile p;
        p.name = "interactive";
        p.type = WorkloadType::INTERACTIVE;
        p.burst_mean = 3.0;
        p.burst_stddev = 1.5;
        p.burst_min = 1;
        p.burst_max = 10;
        p.arrival_rate = 0.5; // frequent arrivals
        p.priority_min = 0;
        p.priority_max = 5;
        p.interactive_fraction = 0.75;
        p.total_count = 50;
        return p;
    }

    WorkloadProfile WorkloadLoader::cpu_bound_profile()
    {
        WorkloadProfile p;
        p.name = "cpu_bound";
        p.type = WorkloadType::CPU_BOUND;
        p.burst_mean = 20.0;
        p.burst_stddev = 8.0;
        p.burst_min = 8;
        p.burst_max = 60;
        p.arrival_rate = 0.15; // infrequent arrivals
        p.priority_min = 0;
        p.priority_max = 10;
        p.interactive_fraction = 0.05;
        p.total_count = 40;
        return p;
    }

    WorkloadProfile WorkloadLoader::mixed_profile()
    {
        WorkloadProfile p;
        p.name = "mixed";
        p.type = WorkloadType::MIXED;
        p.burst_mean = 10.0;
        p.burst_stddev = 8.0;
        p.burst_min = 1;
        p.burst_max = 50;
        p.arrival_rate = 0.30;
        p.priority_min = 0;
        p.priority_max = 10;
        p.interactive_fraction = 0.40;
        p.total_count = 60;
        return p;
    }

    WorkloadProfile WorkloadLoader::stress_profile()
    {
        WorkloadProfile p;
        p.name = "stress";
        p.type = WorkloadType::STRESS;
        p.burst_mean = 15.0;
        p.burst_stddev = 10.0;
        p.burst_min = 1;
        p.burst_max = 80;
        p.arrival_rate = 0.6; // very high arrival rate
        p.priority_min = 0;
        p.priority_max = 15;
        p.interactive_fraction = 0.30;
        p.total_count = 100;
        return p;
    }

    WorkloadProfile WorkloadLoader::batch_profile()
    {
        WorkloadProfile p;
        p.name = "batch";
        p.type = WorkloadType::BATCH;
        p.burst_mean = 30.0;
        p.burst_stddev = 10.0;
        p.burst_min = 15;
        p.burst_max = 80;
        p.arrival_rate = 0.10;
        p.priority_min = 3;
        p.priority_max = 8;
        p.interactive_fraction = 0.0;
        p.total_count = 30;
        return p;
    }

    // ============================================================================
    // WorkloadFactory
    // ============================================================================
    ProcessPtr WorkloadFactory::make(const ProcessDescriptor &desc)
    {
        return std::make_shared<Process>(
            desc.pid, desc.name, desc.arrival, desc.burst, desc.priority, desc.interactive);
    }

    std::vector<ProcessPtr> WorkloadFactory::make_all(const std::vector<ProcessDescriptor> &descs)
    {
        std::vector<ProcessPtr> procs;
        procs.reserve(descs.size());
        for (const auto &d : descs)
            procs.push_back(make(d));
        return procs;
    }

} // namespace adaptive_sched
