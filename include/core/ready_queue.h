#pragma once

#include "process.h"
#include <deque>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>

namespace adaptive_sched
{

    // ---------------------------------------------------------------------------
    // QueueOrder — determines how processes are ordered within a queue level
    // ---------------------------------------------------------------------------
    enum class QueueOrder
    {
        FIFO,     // arrival / enqueue order
        SJF,      // shortest remaining burst first
        PRIORITY, // lowest priority number first (= highest urgency)
        CUSTOM    // caller-supplied comparator
    };

    // ---------------------------------------------------------------------------
    // ReadyQueue
    //
    // A thread-safe, blocking producer-consumer queue for Process objects.
    //
    // Supports:
    //   • Multiple ordering strategies (FIFO / SJF / Priority / Custom)
    //   • Blocking pop with timeout
    //   • Non-blocking try_pop
    //   • Bulk snapshot for inspection
    //   • Graceful shutdown via poison-pill
    // ---------------------------------------------------------------------------
    class ReadyQueue
    {
    public:
        using Comparator = std::function<bool(const ProcessPtr &, const ProcessPtr &)>;

        explicit ReadyQueue(QueueOrder order = QueueOrder::FIFO,
                            Comparator custom_cmp = nullptr);

        // -----------------------------------------------------------------------
        // Producer API
        // -----------------------------------------------------------------------

        // Enqueue a process; wakes blocked consumers
        void push(ProcessPtr proc);

        // Drain another queue into this one (used during scheduler migration)
        void push_all(std::vector<ProcessPtr> procs);

        // -----------------------------------------------------------------------
        // Consumer API
        // -----------------------------------------------------------------------

        // Blocking pop; returns nullptr if shut down and queue is empty
        ProcessPtr pop();

        // Blocking pop with timeout; returns nullptr on timeout/shutdown
        ProcessPtr pop_for(std::chrono::milliseconds timeout);

        // Non-blocking; returns nullptr if empty
        ProcessPtr try_pop();

        // -----------------------------------------------------------------------
        // Inspection (snapshot — caller sees a copy)
        // -----------------------------------------------------------------------
        std::vector<ProcessPtr> snapshot() const;
        std::size_t size() const;
        bool empty() const;

        // Remove a specific process by PID (e.g. for preemption)
        bool remove(PID pid);

        // Find process with the shortest remaining time (for SRTF preemption check)
        ProcessPtr peek_shortest() const;

        // Find highest-urgency process (lowest priority number)
        ProcessPtr peek_highest_priority() const;

        // -----------------------------------------------------------------------
        // Lifecycle
        // -----------------------------------------------------------------------
        void shutdown();
        bool is_shutdown() const { return shutdown_; }

        // Re-sort the internal deque (called when order changes)
        void resort();

        QueueOrder order() const { return order_; }

    private:
        QueueOrder order_;
        Comparator custom_cmp_;

        mutable std::mutex mtx_;
        std::condition_variable cv_;
        std::deque<ProcessPtr> queue_;
        bool shutdown_ = false;

        // Insert according to current ordering strategy
        void insert_ordered(ProcessPtr proc);

        // Comparators
        static bool cmp_sjf(const ProcessPtr &a, const ProcessPtr &b);
        static bool cmp_priority(const ProcessPtr &a, const ProcessPtr &b);
    };

} // namespace adaptive_sched