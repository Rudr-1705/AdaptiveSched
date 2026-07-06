#pragma once

#include "../core/process.h"
#include "../core/clock.h"
#include "workload.h"
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>

namespace adaptive_sched
{

    // ---------------------------------------------------------------------------
    // ProcessGenerator — produces processes and injects them into the scheduler
    //
    // Runs on its own thread, sleeping between arrivals.
    //
    // Two modes:
    //   STATIC  — all descriptors loaded up front; injected at specified ticks
    //   DYNAMIC — generates processes on-the-fly according to a WorkloadProfile
    // ---------------------------------------------------------------------------
    class ProcessGenerator
    {
    public:
        using InjectionCallback = std::function<void(ProcessPtr)>;

        enum class Mode
        {
            STATIC,
            DYNAMIC
        };

        // STATIC mode: use pre-loaded descriptors
        ProcessGenerator(std::vector<ProcessDescriptor> descriptors,
                         InjectionCallback inject_cb);

        // DYNAMIC mode: generate according to profile
        ProcessGenerator(WorkloadProfile profile,
                         InjectionCallback inject_cb,
                         uint32_t seed = 42);

        ~ProcessGenerator();

        void start();
        void stop();
        void join();

        std::size_t total_generated() const { return generated_.load(); }
        std::size_t pending_count() const;

    private:
        Mode mode_;
        std::vector<ProcessDescriptor> descriptors_;
        WorkloadProfile profile_;
        uint32_t seed_;
        InjectionCallback inject_cb_;

        std::thread gen_thread_;
        std::atomic<bool> running_{false};
        mutable std::mutex mtx_;
        std::condition_variable cv_;

        std::atomic<std::size_t> generated_{0};
        PID next_pid_{1};

        void run_static();
        void run_dynamic();

        // Wait until simulation clock reaches target tick
        void wait_until(Tick target_tick);
    };

} // namespace adaptive_sched