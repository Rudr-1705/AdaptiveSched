#include "../../include/simulation/simulation_controller.h"
#include "../../include/core/clock.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <thread>

namespace adaptive_sched
{

    SimulationController::SimulationController(SimConfig config)
        : config_(std::move(config))
    {
    }

    SimulationController::~SimulationController()
    {
        stop();
        if (monitor_thread_.joinable())
            monitor_thread_.join();
    }

    // ============================================================================
    // run — blocking entry point
    // ============================================================================
    void SimulationController::run()
    {
        setup();
        print_banner();
        start();
        wait();
        print_final_report();
    }

    // ============================================================================
    // setup — initialise all subsystems
    // ============================================================================
    void SimulationController::setup()
    {
        // Reset simulation clock for a clean run
        SimClock::instance().reset();

        // Create adaptive scheduler
        adaptive_ = std::make_unique<AdaptiveScheduler>();

        // Create CPU engine
        cpu_ = std::make_unique<CPUEngine>(*adaptive_, config_.cpu_config);

        // Register CPU completion callback
        cpu_->on_complete([this](ProcessPtr proc)
                          { on_process_complete(proc); });

        // Create statistics engine
        stats_engine_ = std::make_unique<StatisticsEngine>();

        // Load workload
        std::vector<ProcessDescriptor> descs;
        if (!config_.csv_input_path.empty())
        {
            descs = WorkloadLoader::from_csv(config_.csv_input_path);
        }
        else
        {
            descs = WorkloadLoader::generate(config_.workload_profile,
                                             /*seed=*/42);
        }

        total_expected_ = static_cast<int>(descs.size());
        if (config_.max_processes > 0)
            total_expected_ = std::min(total_expected_, config_.max_processes);

        // Trim if needed
        if (config_.max_processes > 0 && static_cast<int>(descs.size()) > config_.max_processes)
            descs.resize(static_cast<std::size_t>(config_.max_processes));

        // Build injection callback
        auto inject_cb = [this](ProcessPtr proc)
        {
            adaptive_->add_process(proc);
        };

        // Create process generator (static mode from descriptors)
        generator_ = std::make_unique<ProcessGenerator>(std::move(descs), inject_cb);
    }

    // ============================================================================
    // start — fire all threads
    // ============================================================================
    void SimulationController::start()
    {
        simulation_done_.store(false, std::memory_order_release);
        completed_count_.store(0, std::memory_order_release);

        // 1. Start adaptive monitor thread
        monitor_running_.store(true, std::memory_order_release);
        monitor_thread_ = std::thread(&SimulationController::monitor_loop, this);

        // 2. Start process generator
        generator_->start();

        // 3. Start CPU (last — everything else must be ready)
        cpu_->start();
    }

    // ============================================================================
    // stop — graceful shutdown
    // ============================================================================
    void SimulationController::stop()
    {
        simulation_done_.store(true, std::memory_order_release);

        monitor_running_.store(false, std::memory_order_release);
        done_cv_.notify_all();

        if (generator_)
            generator_->stop();
        if (cpu_)
            cpu_->stop();
    }

    // ============================================================================
    // wait — block until simulation completes
    // ============================================================================
    void SimulationController::wait()
    {
        std::unique_lock<std::mutex> lk(done_mtx_);
        done_cv_.wait(lk, [this]
                      { return simulation_done_.load(std::memory_order_acquire); });

        // Graceful teardown
        stop();
        if (generator_)
            generator_->join();
        if (cpu_)
            cpu_->join();
        if (monitor_thread_.joinable())
            monitor_thread_.join();
    }

    // ============================================================================
    // monitor_loop — adaptive evaluation + optional tick cap
    // ============================================================================
    void SimulationController::monitor_loop()
    {
        constexpr std::chrono::milliseconds MONITOR_SLEEP{2};

        while (monitor_running_.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(MONITOR_SLEEP);

            Tick current = SimClock::instance().now();

            // Tick cap
            if (config_.max_ticks > 0 && current >= config_.max_ticks)
            {
                std::cout << "[MONITOR] Max ticks (" << config_.max_ticks << ") reached. Stopping.\n";
                {
                    std::lock_guard<std::mutex> lk(done_mtx_);
                    simulation_done_.store(true, std::memory_order_release);
                }
                done_cv_.notify_all();
                return;
            }

            // Feed adaptive scheduler its evaluation window
            if (config_.enable_adaptive)
            {
                adaptive_->evaluate_and_adapt(current);

                // Log scheduler transitions to statistics engine
                const auto &history = adaptive_->switch_history();
                static std::size_t last_recorded = 0;
                if (history.size() > last_recorded)
                {
                    for (std::size_t i = last_recorded; i < history.size(); ++i)
                        stats_engine_->record_scheduler_switch(history[i]);
                    last_recorded = history.size();
                }
            }

            // Periodic live snapshot to console (every ~100 ticks)
            if (current > 0 && current % 100 == 0 && config_.verbose)
            {
                std::cout << "[MONITOR] tick=" << current
                          << " completed=" << completed_count_.load()
                          << "/" << total_expected_
                          << " cpu_util=" << std::fixed << std::setprecision(1)
                          << cpu_->utilization() * 100.0 << "%"
                          << " active=" << to_string(adaptive_->active_policy())
                          << " ready=" << adaptive_->ready_count()
                          << "\n";
            }
        }
    }

    // ============================================================================
    // on_process_complete — called from CPU completion callback
    // ============================================================================
    void SimulationController::on_process_complete(ProcessPtr proc)
    {
        // Record in statistics engine
        stats_engine_->record_process_completion(proc, adaptive_->active_policy());
        stats_engine_->record_busy_tick(proc->completion_time);

        int done = completed_count_.fetch_add(1, std::memory_order_acq_rel) + 1;

        check_termination();
    }

    // ============================================================================
    // check_termination — decide if simulation is done
    // ============================================================================
    void SimulationController::check_termination()
    {
        if (simulation_done_.load(std::memory_order_acquire))
            return;

        int done = completed_count_.load(std::memory_order_acquire);

        // All expected processes completed
        if (done >= total_expected_ && total_expected_ > 0)
        {
            // Wait a brief moment to let any in-flight ticks flush
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            std::cout << "\n[SIM] All " << done << " processes completed.\n";
            {
                std::lock_guard<std::mutex> lk(done_mtx_);
                simulation_done_.store(true, std::memory_order_release);
            }
            done_cv_.notify_all();
        }
    }

    // ============================================================================
    // print_banner
    // ============================================================================
    void SimulationController::print_banner() const
    {
        if (config_.quiet)
            return;
        std::cout << "\n"
                  << std::string(70, '*') << "\n"
                  << "*  AdaptiveSched++ — CPU Scheduling Simulator\n"
                  << "*  Scenario  : " << config_.scenario_name << "\n"
                  << "*  Workload  : " << config_.workload_profile.name << "\n"
                  << "*  Processes : " << total_expected_ << "\n"
                  << "*  Adaptive  : " << (config_.enable_adaptive ? "ENABLED" : "DISABLED") << "\n"
                  << std::string(70, '*') << "\n\n";
    }

    // ============================================================================
    // print_final_report
    // ============================================================================
    void SimulationController::print_final_report() const
    {
        Tick total = cpu_->total_ticks();
        adaptive_->finalize_stats(total);

        if (!config_.quiet)
        {
            std::cout << "\n";
            stats_engine_->print_summary(config_.scenario_name);
            stats_engine_->print_per_process_table();
            stats_engine_->print_scheduler_transition_log();
            std::cout << "\n"
                      << adaptive_->decision_trace(20) << "\n";
        }

        if (config_.export_csv || config_.export_timeline)
            create_output_dir();

        if (config_.export_csv)
            stats_engine_->export_csv(config_.output_dir + "/metrics.csv");

        if (config_.export_timeline)
            stats_engine_->export_timeline(config_.output_dir + "/timeline.csv");
    }

    void SimulationController::create_output_dir() const
    {
        std::error_code ec;
        std::filesystem::create_directories(config_.output_dir, ec);
        if (ec)
            std::cerr << "[SIM] Warning: could not create output dir: " << ec.message() << "\n";
    }

} // namespace adaptive_sched