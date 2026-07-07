#pragma once

#include "../scheduler/adaptive.h"
#include "../execution/cpu.h"
#include "../metrics/statistics.h"
#include "process_generator.h"
#include "workload.h"

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace adaptive_sched
{

    // ---------------------------------------------------------------------------
    // SimConfig — top-level simulation parameters
    // ---------------------------------------------------------------------------
    struct SimConfig
    {
        std::string scenario_name = "default";
        WorkloadProfile workload_profile;
        std::string csv_input_path; // empty = use profile generator
        CPUConfig cpu_config;
        bool enable_adaptive = true;
        bool export_csv = false;
        bool export_timeline = false;
        std::string output_dir = "./output";
        Tick max_ticks = 0;    // 0 = run until all done
        int max_processes = 0; // 0 = unlimited
        bool verbose = false;
        bool quiet = false; // suppress all stdout except errors
    };

    // ---------------------------------------------------------------------------
    // SimulationController — top-level orchestrator
    //
    // Responsibilities:
    //   • Initialize all subsystems (scheduler, CPU, generator, metrics)
    //   • Start threads in correct order
    //   • Run the adaptive monitor loop
    //   • Detect completion and perform graceful shutdown
    //   • Aggregate and display final reports
    // ---------------------------------------------------------------------------
    class SimulationController
    {
    public:
        explicit SimulationController(SimConfig config);
        ~SimulationController();

        // Run the simulation to completion (blocks until done)
        void run();

        // Async start / stop
        void start();
        void stop();
        void wait();

        // Access sub-systems after run (for testing / inspection)
        const StatisticsEngine &stats() const { return *stats_engine_; }
        const AdaptiveScheduler &scheduler() const { return *adaptive_; }

    private:
        SimConfig config_;

        // Sub-systems
        std::unique_ptr<AdaptiveScheduler> adaptive_;
        std::unique_ptr<CPUEngine> cpu_;
        std::unique_ptr<ProcessGenerator> generator_;
        std::unique_ptr<StatisticsEngine> stats_engine_;

        // Monitor thread (runs evaluate_and_adapt periodically)
        std::thread monitor_thread_;
        std::atomic<bool> monitor_running_{false};

        // Shutdown coordination
        std::atomic<bool> simulation_done_{false};
        std::mutex done_mtx_;
        std::condition_variable done_cv_;

        std::atomic<int> completed_count_{0};
        int total_expected_ = 0;

        // -----------------------------------------------------------------------
        // Internal
        // -----------------------------------------------------------------------
        void setup();
        void monitor_loop();
        void on_process_complete(ProcessPtr proc);
        void check_termination();
        void print_banner() const;
        void print_final_report() const;
        void create_output_dir() const;
    };

} // namespace adaptive_sched