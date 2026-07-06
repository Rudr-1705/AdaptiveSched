#include "../../include/execution/cpu.h"
#include "../../include/core/clock.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <chrono>

namespace adaptive_sched
{

    CPUEngine::CPUEngine(AdaptiveScheduler &scheduler, CPUConfig cfg)
        : scheduler_(scheduler), cfg_(cfg)
    {
    }

    CPUEngine::~CPUEngine()
    {
        stop();
        join();
    }

    void CPUEngine::start()
    {
        running_.store(true, std::memory_order_release);
        cpu_thread_ = std::thread(&CPUEngine::run, this);
    }

    void CPUEngine::stop()
    {
        running_.store(false, std::memory_order_release);
        // Wake up any waiting thread
        {
            std::lock_guard<std::mutex> lk(pause_mtx_);
            paused_.store(false, std::memory_order_release);
        }
        pause_cv_.notify_all();
    }

    void CPUEngine::join()
    {
        if (cpu_thread_.joinable())
            cpu_thread_.join();
    }

    void CPUEngine::on_complete(CompletionCallback cb)
    {
        std::lock_guard<std::mutex> lk(cb_mtx_);
        completion_cb_ = std::move(cb);
    }

    void CPUEngine::on_tick(TickCallback cb)
    {
        std::lock_guard<std::mutex> lk(cb_mtx_);
        tick_cb_ = std::move(cb);
    }

    void CPUEngine::pause()
    {
        paused_.store(true, std::memory_order_release);
    }

    void CPUEngine::resume()
    {
        {
            std::lock_guard<std::mutex> lk(pause_mtx_);
            paused_.store(false, std::memory_order_release);
        }
        pause_cv_.notify_all();
    }

    ProcessPtr CPUEngine::current_process() const
    {
        std::lock_guard<std::mutex> lk(current_proc_mtx_);
        return current_proc_;
    }

    double CPUEngine::utilization() const
    {
        Tick total = total_ticks_.load(std::memory_order_acquire);
        if (total == 0)
            return 0.0;
        return static_cast<double>(busy_ticks_.load(std::memory_order_acquire)) /
               static_cast<double>(total);
    }

    // ============================================================================
    // Main execution loop
    // ============================================================================
    void CPUEngine::run()
    {
        while (running_.load(std::memory_order_acquire))
        {
            // --- Pause check ---
            {
                std::unique_lock<std::mutex> lk(pause_mtx_);
                pause_cv_.wait(lk, [this]
                               { return !paused_.load(std::memory_order_relaxed) ||
                                        !running_.load(std::memory_order_relaxed); });
            }
            if (!running_.load(std::memory_order_acquire))
                break;

            Tick t = now();
            bool was_busy = false;

            // --- Dispatch if idle ---
            bool need_dispatch = false;
            {
                std::lock_guard<std::mutex> lk(current_proc_mtx_);
                need_dispatch = (current_proc_ == nullptr);
            }

            if (need_dispatch)
            {
                ProcessPtr next = scheduler_.get_next_process();
                if (next)
                {
                    {
                        std::lock_guard<std::mutex> lk(current_proc_mtx_);
                        if (current_proc_ && current_proc_->pid != next->pid)
                        {
                            do_context_switch(current_proc_, next);
                        }
                        current_proc_ = next;
                    }
                    // Adaptive: check for preemption by higher-priority newcomer
                    // (SRTF / Priority modes compare current vs front-of-queue)
                    auto snap = scheduler_.snapshot();
                    if (!snap.empty())
                    {
                        ProcessPtr front = snap.front();
                        if (front && front->pid != next->pid)
                        {
                            bool preempted = scheduler_.handle_preemption(next, front);
                            if (preempted)
                            {
                                std::lock_guard<std::mutex> lk(current_proc_mtx_);
                                current_proc_ = nullptr;
                                // Re-fetch on next iteration
                            }
                        }
                    }
                }
                else
                {
                    // CPU is idle
                    idle_ticks_.fetch_add(1, std::memory_order_relaxed);
                    total_ticks_.fetch_add(1, std::memory_order_relaxed);
                    scheduler_.record_cpu_sample(false);

                    // Fire tick callback
                    {
                        std::lock_guard<std::mutex> lk(cb_mtx_);
                        if (tick_cb_)
                            tick_cb_(t);
                    }
                    scheduler_.on_tick(t);
                    SimClock::instance().tick();

                    if (cfg_.tick_sleep_us > 0)
                        std::this_thread::sleep_for(std::chrono::microseconds(cfg_.tick_sleep_us));
                    continue;
                }
            }

            // --- Execute one tick ---
            {
                std::lock_guard<std::mutex> lk(current_proc_mtx_);
                if (current_proc_)
                {
                    was_busy = execute_tick();
                }
            }

            // --- Telemetry ---
            if (was_busy)
                busy_ticks_.fetch_add(1, std::memory_order_relaxed);
            else
                idle_ticks_.fetch_add(1, std::memory_order_relaxed);
            total_ticks_.fetch_add(1, std::memory_order_relaxed);

            scheduler_.record_cpu_sample(was_busy);

            // --- Tick callbacks ---
            {
                std::lock_guard<std::mutex> lk(cb_mtx_);
                if (tick_cb_)
                    tick_cb_(t);
            }
            scheduler_.on_tick(t);
            SimClock::instance().tick();

            // Configurable wall-clock pacing (0 = max speed)
            if (cfg_.tick_sleep_us > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(cfg_.tick_sleep_us));
        }
    }

    // ============================================================================
    // execute_tick — run current process for one logical tick
    // Returns true if process is still running, false if it completed
    // ============================================================================
    bool CPUEngine::execute_tick()
    {
        if (!current_proc_)
            return false;

        auto &proc = current_proc_;

        // Record first scheduling (response time)
        if (proc->start_time == UINT64_MAX)
        {
            proc->start_time = now();
        }
        proc->set_state(ProcessState::RUNNING);

        // Burn one tick
        if (proc->remaining_time > 0)
            proc->remaining_time--;
        if (proc->quantum_remaining > 0)
            proc->quantum_remaining--;

        if (cfg_.log_every_tick)
        {
            std::cout << "[CPU  t=" << std::setw(5) << now()
                      << "] PID=" << std::setw(3) << proc->pid
                      << " rem=" << std::setw(4) << proc->remaining_time
                      << " q=" << std::setw(3) << proc->quantum_remaining
                      << " [" << proc->name << "]\n";
        }

        // Completed?
        if (proc->remaining_time == 0)
        {
            scheduler_.on_process_complete(proc);

            // Notify completion callback
            ProcessPtr completed = proc;
            current_proc_ = nullptr;

            scheduler_.record_completion(completed,
                                         completed->waiting_time,
                                         completed->turnaround_time,
                                         completed->response_time);

            std::cout << "[CPU  t=" << std::setw(5) << now()
                      << "] COMPLETE PID=" << completed->pid
                      << " [" << completed->name << "]"
                      << " WT=" << completed->waiting_time
                      << " TAT=" << completed->turnaround_time
                      << " RT=" << completed->response_time << "\n";

            {
                std::lock_guard<std::mutex> lk(cb_mtx_);
                if (completion_cb_)
                    completion_cb_(completed);
            }
            return false;
        }

        // Check quantum expiry (preemptive schedulers)
        if (cfg_.preemption_enabled && proc->quantum_remaining == 0)
        {
            bool preempted = scheduler_.handle_preemption(proc, nullptr);
            if (preempted)
            {
                current_proc_ = nullptr;
                ctx_switches_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        return true;
    }

    // ============================================================================
    // do_context_switch — charge overhead ticks and log
    // ============================================================================
    void CPUEngine::do_context_switch(ProcessPtr from, ProcessPtr to)
    {
        if (!from || !to)
            return;

        ctx_switches_.fetch_add(1, std::memory_order_relaxed);
        from->ctx_switch.switch_count++;
        from->ctx_switch.total_overhead += cfg_.context_switch_overhead;

        if (cfg_.context_switch_overhead > 0)
        {
            // Charge overhead ticks (counted as idle / system time)
            for (Tick i = 0; i < cfg_.context_switch_overhead; ++i)
            {
                idle_ticks_.fetch_add(1, std::memory_order_relaxed);
                total_ticks_.fetch_add(1, std::memory_order_relaxed);
                scheduler_.record_cpu_sample(false);
                SimClock::instance().tick();
            }
        }

        std::cout << "[CTX  t=" << std::setw(5) << now()
                  << "] Switch PID " << from->pid << " -> PID " << to->pid << "\n";
    }

    void CPUEngine::log(const std::string &msg) const
    {
        std::cout << "[CPU] " << msg << "\n";
    }

} // namespace adaptive_sched