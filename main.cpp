#include "include/simulation/simulation_controller.h"
#include "include/simulation/workload.h"

#include <iostream>
#include <string>
#include <map>
#include <functional>
#include <cstring>
#include <streambuf>

using namespace adaptive_sched;

// Null stream buffer to suppress output in quiet mode
class NullStreambuf : public std::streambuf {
protected:
    int overflow(int c) override { return c; }
};

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------
static void print_usage(const char *prog)
{
    std::cout << "\n"
              << "Usage: " << prog << " [OPTIONS]\n\n"
              << "Options:\n"
              << "  --workload <name>   Workload profile: interactive, cpu_bound, mixed,\n"
              << "                      stress, batch  (default: mixed)\n"
              << "  --csv <path>        Load workload from CSV file instead of profile\n"
              << "  --count <n>         Number of processes to simulate (default: from profile)\n"
              << "  --no-adaptive       Disable adaptive switching (runs MLFQ only)\n"
              << "  --export-csv        Export per-process metrics to output/metrics.csv\n"
              << "  --export-timeline   Export tick timeline to output/timeline.csv\n"
              << "  --verbose           Print live monitor snapshots every 100 ticks\n"
              << "  --max-ticks <n>     Hard simulation tick limit (0 = run to completion)\n"
              << "  --help              Show this help\n\n"
              << "Examples:\n"
              << "  " << prog << " --workload interactive\n"
              << "  " << prog << " --workload stress --count 100 --verbose\n"
              << "  " << prog << " --csv workloads/mixed.csv --export-csv\n\n";
}

// ---------------------------------------------------------------------------
// Parse arguments into SimConfig
// ---------------------------------------------------------------------------
static SimConfig parse_args(int argc, char **argv)
{
    SimConfig cfg;
    cfg.scenario_name = "adaptive_simulation";
    cfg.workload_profile = WorkloadLoader::mixed_profile();
    cfg.enable_adaptive = true;
    cfg.verbose = false;

    // Default CPU config
    cfg.cpu_config.context_switch_overhead = 1;
    cfg.cpu_config.tick_sleep_us = 0; // maximum speed
    cfg.cpu_config.preemption_enabled = true;
    cfg.cpu_config.log_every_tick = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h")
        {
            print_usage(argv[0]);
            std::exit(0);
        }
        else if (arg == "--quiet")
        {
            cfg.quiet = true;
        }
        else if (arg == "--output-dir" && i + 1 < argc)
        {
            cfg.output_dir = argv[++i];
        }
        else if (arg == "--no-adaptive")
        {
            cfg.enable_adaptive = false;
        }
        else if (arg == "--export-csv")
        {
            cfg.export_csv = true;
        }
        else if (arg == "--export-timeline")
        {
            cfg.export_timeline = true;
        }
        else if (arg == "--verbose")
        {
            cfg.verbose = true;
        }
        else if (arg == "--workload" && i + 1 < argc)
        {
            std::string wl = argv[++i];
            if (wl == "interactive")
            {
                cfg.workload_profile = WorkloadLoader::interactive_profile();
                cfg.scenario_name = "interactive_workload";
            }
            else if (wl == "cpu_bound")
            {
                cfg.workload_profile = WorkloadLoader::cpu_bound_profile();
                cfg.scenario_name = "cpu_bound_workload";
            }
            else if (wl == "mixed")
            {
                cfg.workload_profile = WorkloadLoader::mixed_profile();
                cfg.scenario_name = "mixed_workload";
            }
            else if (wl == "stress")
            {
                cfg.workload_profile = WorkloadLoader::stress_profile();
                cfg.scenario_name = "stress_workload";
            }
            else if (wl == "batch")
            {
                cfg.workload_profile = WorkloadLoader::batch_profile();
                cfg.scenario_name = "batch_workload";
            }
            else
            {
                std::cerr << "Unknown workload: " << wl << "\n";
                print_usage(argv[0]);
                std::exit(1);
            }
        }
        else if (arg == "--csv" && i + 1 < argc)
        {
            cfg.csv_input_path = argv[++i];
            cfg.scenario_name = "csv_workload";
        }
        else if (arg == "--count" && i + 1 < argc)
        {
            cfg.max_processes = std::stoi(argv[++i]);
            cfg.workload_profile.total_count = cfg.max_processes;
        }
        else if (arg == "--max-ticks" && i + 1 < argc)
        {
            cfg.max_ticks = static_cast<Tick>(std::stoull(argv[++i]));
        }
        else
        {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            std::exit(1);
        }
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char **argv)
{
    SimConfig cfg = parse_args(argc, argv);

    // Suppress all stdout in quiet mode (errors still go to stderr)
    NullStreambuf null_buf;
    std::streambuf *orig_cout = nullptr;
    if (cfg.quiet) {
        orig_cout = std::cout.rdbuf(&null_buf);
    }

    try
    {
        SimulationController controller(cfg);
        controller.run();
    }
    catch (const std::exception &e)
    {
        if (orig_cout) std::cout.rdbuf(orig_cout);
        std::cerr << "[FATAL] " << e.what() << "\n";
        return 1;
    }

    if (orig_cout) std::cout.rdbuf(orig_cout);
    return 0;
}