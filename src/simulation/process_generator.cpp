#include "../../include/simulation/process_generator.h"
#include "../../include/core/clock.h"
#include "../../include/simulation/workload.h"

#include <iostream>
#include <algorithm>
#include <random>
#include <chrono>
#include <climits>

namespace adaptive_sched
{

    // ============================================================================
    // STATIC mode constructor
    // ============================================================================
    ProcessGenerator::ProcessGenerator(std::vector<ProcessDescriptor> descriptors,
                                       InjectionCallback inject_cb)
        : mode_(Mode::STATIC),
          descriptors_(std::move(descriptors)),
          inject_cb_(std::move(inject_cb)),
          seed_(42)
    {
        // Sort by arrival time so we inject in order
        std::sort(descriptors_.begin(), descriptors_.end(),
                  [](const ProcessDescriptor &a, const ProcessDescriptor &b)
                  { return a.arrival < b.arrival; });
    }

    // ============================================================================
    // DYNAMIC mode constructor
    // ============================================================================
    ProcessGenerator::ProcessGenerator(WorkloadProfile profile,
                                       InjectionCallback inject_cb,
                                       uint32_t seed)
        : mode_(Mode::DYNAMIC),
          profile_(std::move(profile)),
          inject_cb_(std::move(inject_cb)),
          seed_(seed)
    {
    }

    ProcessGenerator::~ProcessGenerator()
    {
        stop();
        join();
    }

    void ProcessGenerator::start()
    {
        running_.store(true, std::memory_order_release);
        if (mode_ == Mode::STATIC)
            gen_thread_ = std::thread(&ProcessGenerator::run_static, this);
        else
            gen_thread_ = std::thread(&ProcessGenerator::run_dynamic, this);
    }

    void ProcessGenerator::stop()
    {
        running_.store(false, std::memory_order_release);
        cv_.notify_all();
    }

    void ProcessGenerator::join()
    {
        if (gen_thread_.joinable())
            gen_thread_.join();
    }

    std::size_t ProcessGenerator::pending_count() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return descriptors_.size() - generated_.load(std::memory_order_relaxed);
    }

    // ============================================================================
    // STATIC mode: inject processes at their specified arrival ticks
    // ============================================================================
    void ProcessGenerator::run_static()
    {
        std::size_t idx = 0;
        while (running_.load(std::memory_order_acquire) && idx < descriptors_.size())
        {
            const auto &desc = descriptors_[idx];

            // Wait until simulation clock reaches arrival tick
            wait_until(desc.arrival);

            if (!running_.load(std::memory_order_acquire))
                break;

            auto proc = WorkloadFactory::make(desc);
            std::cout << "[GEN] Injecting PID=" << proc->pid
                      << " (" << proc->name << ")"
                      << " at tick=" << proc->arrival_time
                      << " burst=" << proc->burst_time
                      << " pri=" << proc->priority
                      << (proc->is_interactive ? " [INTERACTIVE]" : "")
                      << "\n";

            inject_cb_(proc);
            generated_.fetch_add(1, std::memory_order_relaxed);
            ++idx;
        }

        std::cout << "[GEN] Static generator done. Total injected: "
                  << generated_.load() << "\n";
    }

    // ============================================================================
    // DYNAMIC mode: generate processes on-the-fly according to profile
    // ============================================================================
    void ProcessGenerator::run_dynamic()
    {
        std::mt19937 engine(seed_);
        std::exponential_distribution<double> arr_dist(profile_.arrival_rate);
        std::normal_distribution<double> burst_dist(profile_.burst_mean, profile_.burst_stddev);
        std::uniform_int_distribution<int> pri_dist(profile_.priority_min, profile_.priority_max);
        std::bernoulli_distribution interactive_dist(profile_.interactive_fraction);

        int max_count = (profile_.total_count > 0) ? profile_.total_count : INT_MAX;
        int generated_count = 0;

        while (running_.load(std::memory_order_acquire) && generated_count < max_count)
        {
            // Wait for next inter-arrival interval
            double inter_arr = arr_dist(engine);
            Tick wait_ticks = static_cast<Tick>(std::max(1.0, inter_arr));
            Tick inject_at = SimClock::instance().now() + wait_ticks;

            wait_until(inject_at);

            if (!running_.load(std::memory_order_acquire))
                break;

            ProcessDescriptor d;
            d.pid = ++next_pid_;
            d.name = "proc_" + std::to_string(d.pid);
            d.arrival = SimClock::instance().now();

            double burst_raw = burst_dist(engine);
            Tick burst = static_cast<Tick>(std::round(burst_raw));
            burst = std::max(profile_.burst_min, std::min(profile_.burst_max, burst));
            d.burst = burst;
            d.priority = pri_dist(engine);
            d.interactive = interactive_dist(engine);

            auto proc = WorkloadFactory::make(d);
            std::cout << "[GEN] Dynamic PID=" << proc->pid
                      << " at tick=" << proc->arrival_time
                      << " burst=" << proc->burst_time
                      << " pri=" << proc->priority
                      << (proc->is_interactive ? " [I]" : "")
                      << "\n";

            inject_cb_(proc);
            generated_.fetch_add(1, std::memory_order_relaxed);
            ++generated_count;
        }

        std::cout << "[GEN] Dynamic generator done. Total injected: "
                  << generated_.load() << "\n";
    }

    // ============================================================================
    // wait_until — spin-sleep until simulation clock reaches target_tick
    // Uses short sleeps to avoid burning CPU while yielding frequently enough
    // that the simulation doesn't stall.
    // ============================================================================
    void ProcessGenerator::wait_until(Tick target_tick)
    {
        while (running_.load(std::memory_order_acquire))
        {
            Tick current = SimClock::instance().now();
            if (current >= target_tick)
                return;

            // Sleep a short wall-clock interval and retry
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

} // namespace adaptive_sched
