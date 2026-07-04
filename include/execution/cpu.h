#pragma once

#include "../core/process.h"
#include "../core/clock.h"
#include "../scheduler/adaptive.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <optional>

namespace adaptive_sched
{

    // ---------------------------------------------------------------------------
    // CPUConfig — runtime-tunable parameters for the execution engine
    // ---------------------------------------------------------------------------
    struct CPUConfig
    {
        Tick context_switch_overhead = 1; // ticks charged per context switch
        Tick tick_sleep_us = 0;           // wall-clock microseconds per tick (0=max speed)
        bool preemption_enabled = true;
        bool log_every_tick = false;
    };

    // ---------------------------------------------------------------------------
    // CPUEngine — simulates a single-core CPU executing processes
    //
    // Runs on its own thread.  Each iteration of the main loop:
    //   1. Ask the active scheduler for the next process.
    //   2. If process is new, record response time.
    //   3. Execute one tick of the process (decrement remaining_time).
    //   4. Check for quantum expiry (preemptive schedulers).
    //   5. Check for preemption by newly arrived process (SRTF / Priority).
    //   6. If process completes, record metrics and notify.
    //   7. Advance simulation clock.
    //
    // The engine publishes per-tick telemetry to AdaptiveScheduler::record_cpu_sample().
    // ---------------------------------------------------------------------------
    class CPUEngine
    {
    public:
        using CompletionCallback = std::function<void(ProcessPtr)>;
        using TickCallback = std::function<void(Tick)>;

        explicit CPUEngine(AdaptiveScheduler &scheduler,
                           CPUConfig cfg = {});

        ~CPUEngine();

        // Start the CPU thread
        void start();

        // Signal graceful shutdown (drains queue before stopping)
        void stop();

        // Block until CPU thread exits
        void join();

        // Register a callback invoked each time a process completes
        void on_complete(CompletionCallback cb);

        // Register a callback invoked every tick (used by monitor thread)
        void on_tick(TickCallback cb);

        // Pause / resume execution (for controlled experiments)
        void pause();
        void resume();

        // -----------------------------------------------------------------------
        // Telemetry accessors (thread-safe)
        // -----------------------------------------------------------------------
        Tick total_ticks() const { return total_ticks_.load(); }
        Tick idle_ticks() const { return idle_ticks_.load(); }
        Tick busy_ticks() const { return busy_ticks_.load(); }
        uint64_t ctx_switches() const { return ctx_switches_.load(); }
        double utilization() const;

        ProcessPtr current_process() const;

    private:
        AdaptiveScheduler &scheduler_;
        CPUConfig cfg_;

        // -----------------------------------------------------------------------
        // Threading
        // -----------------------------------------------------------------------
        std::thread cpu_thread_;
        std::atomic<bool> running_{false};
        std::atomic<bool> paused_{false};
        std::mutex pause_mtx_;
        std::condition_variable pause_cv_;

        // -----------------------------------------------------------------------
        // Current execution state
        // -----------------------------------------------------------------------
        mutable std::mutex current_proc_mtx_;
        ProcessPtr current_proc_;

        // -----------------------------------------------------------------------
        // Telemetry
        // -----------------------------------------------------------------------
        std::atomic<Tick> total_ticks_{0};
        std::atomic<Tick> idle_ticks_{0};
        std::atomic<Tick> busy_ticks_{0};
        std::atomic<uint64_t> ctx_switches_{0};

        // -----------------------------------------------------------------------
        // Callbacks
        // -----------------------------------------------------------------------
        mutable std::mutex cb_mtx_;
        CompletionCallback completion_cb_;
        TickCallback tick_cb_;

        // -----------------------------------------------------------------------
        // Main loop
        // -----------------------------------------------------------------------
        void run();

        // Execute a single tick of work for current_proc_
        // Returns: true if the process completed during this tick
        bool execute_tick();

        // Perform a context switch from old to new (charges overhead ticks)
        void do_context_switch(ProcessPtr from, ProcessPtr to);

        // Log a formatted line to stdout
        void log(const std::string &msg) const;
    };

} // namespace adaptive_sched