#pragma once

#include "state.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <mutex>

namespace adaptive_sched
{

    using PID = uint32_t;
    using Tick = uint64_t;

    // Per-process aging metadata used by schedulers that implement anti-starvation
    struct AgingInfo
    {
        Tick last_promoted_at = 0; // simulation tick of last priority boost
        int starvation_ticks = 0;  // accumulated ticks waiting at low priority
        int boost_count = 0;       // how many times aging raised this process
    };

    // Context-switch counter and cost accounting
    struct ContextSwitchInfo
    {
        uint32_t switch_count = 0;
        Tick total_overhead = 0; // in ticks
    };

    // Complete process descriptor — equivalent to a real OS PCB
    class Process
    {
    public:
        // --- Identity ---
        PID pid;
        std::string name;

        // --- Timing (all in simulation ticks) ---
        Tick arrival_time = 0;
        Tick burst_time = 0;          // original CPU burst
        Tick remaining_time = 0;      // decremented during execution
        Tick start_time = UINT64_MAX; // first time scheduled
        Tick completion_time = 0;

        // --- Derived metrics (computed at completion) ---
        Tick waiting_time = 0;
        Tick turnaround_time = 0;
        Tick response_time = 0;

        // --- Scheduling attributes ---
        int priority = 0;           // lower number = higher priority
        int base_priority = 0;      // original, before dynamic changes
        int queue_level = 0;        // MLFQ queue index (0 = highest)
        Tick quantum_remaining = 0; // ticks left in current time quantum

        // --- State ---
        ProcessState state = ProcessState::NEW;

        // --- Concurrency ---
        mutable std::mutex mtx;

        // --- Supplemental metadata ---
        AgingInfo aging;
        ContextSwitchInfo ctx_switch;
        bool is_interactive = false; // hint for MLFQ promotion

        // --- Constructors ---
        Process() = default;
        Process(PID pid, std::string name, Tick arrival, Tick burst, int priority = 0, bool interactive = false);

        // --- Accessors (all thread-safe via caller's lock or atomic reads) ---
        void set_state(ProcessState s);
        ProcessState get_state() const;

        // --- Helpers ---
        bool is_complete() const { return remaining_time == 0; }
        void record_completion(Tick now);
        void apply_aging(Tick now, int age_threshold, int boost_amount);

        // --- Formatting ---
        std::string summary() const;

    private:
        // mutable because get_state may lock
        mutable std::atomic<ProcessState> atomic_state{ProcessState::NEW};
    };

    using ProcessPtr = std::shared_ptr<Process>;

} // namespace adaptive_sched