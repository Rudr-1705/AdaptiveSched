#include "../../include/scheduler/sjf.h"
#include "../../include/core/clock.h"
#include <iostream>
#include <algorithm>

namespace adaptive_sched
{

    SJFScheduler::SJFScheduler(Mode mode)
        : mode_(mode), queue_(QueueOrder::SJF) // always sorted by remaining_time
    {
    }

    std::string SJFScheduler::name() const
    {
        return (mode_ == Mode::SRTF) ? "SRTF" : "SJF";
    }

    SchedulerPolicy SJFScheduler::policy() const
    {
        return (mode_ == Mode::SRTF) ? SchedulerPolicy::SRTF : SchedulerPolicy::SJF;
    }

    void SJFScheduler::add_process(ProcessPtr proc)
    {
        proc->set_state(ProcessState::READY);
        queue_.push(proc);
        stats_.processes_handled.fetch_add(1, std::memory_order_relaxed);
    }

    ProcessPtr SJFScheduler::get_next_process()
    {
        // Queue is already sorted shortest-first
        auto proc = queue_.try_pop();
        if (proc)
        {
            proc->set_state(ProcessState::RUNNING);
            if (proc->start_time == UINT64_MAX)
            {
                proc->start_time = now();
            }
            // Non-preemptive: run to completion; SRTF will preempt via handle_preemption
            proc->quantum_remaining = proc->remaining_time;
        }
        return proc;
    }

    // In SRTF mode: preempt if incoming has shorter remaining time
    bool SJFScheduler::handle_preemption(ProcessPtr running, ProcessPtr incoming)
    {
        if (mode_ == Mode::NON_PREEMPTIVE)
            return false;
        if (!running || !incoming)
            return false;

        if (incoming->remaining_time < running->remaining_time)
        {
            stats_.preemptions.fetch_add(1, std::memory_order_relaxed);
            stats_.context_switches.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    void SJFScheduler::on_process_complete(ProcessPtr proc)
    {
        proc->record_completion(now());
        std::lock_guard<std::mutex> lk(completed_mtx_);
        completed_.push_back(proc);
    }

    void SJFScheduler::on_tick(Tick current_time)
    {
        // Periodic starvation detection for long-burst processes
        if (current_time % 10 == 0)
        {
            check_starvation(current_time);
        }
    }

    void SJFScheduler::check_starvation(Tick now_t)
    {
        auto snap = queue_.snapshot();
        for (auto &p : snap)
        {
            p->aging.starvation_ticks++;
            if (p->aging.starvation_ticks >= STARVATION_THRESHOLD)
            {
                stats_.starvation_events.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    std::size_t SJFScheduler::ready_count() const { return queue_.size(); }
    bool SJFScheduler::has_ready_work() const { return !queue_.empty(); }
    std::vector<ProcessPtr> SJFScheduler::snapshot() const { return queue_.snapshot(); }

    std::vector<ProcessPtr> SJFScheduler::drain_queue()
    {
        std::vector<ProcessPtr> out;
        ProcessPtr p;
        while ((p = queue_.try_pop()) != nullptr)
            out.push_back(p);
        return out;
    }

    void SJFScheduler::import_queue(std::vector<ProcessPtr> procs)
    {
        for (auto &p : procs)
        {
            p->set_state(ProcessState::READY);
            queue_.push(p);
        }
    }

    void SJFScheduler::finalize_stats(Tick total_ticks)
    {
        std::lock_guard<std::mutex> lk(completed_mtx_);
        if (completed_.empty())
            return;

        double ws = 0, ts = 0, rs = 0;
        for (auto &p : completed_)
        {
            ws += p->waiting_time;
            ts += p->turnaround_time;
            rs += p->response_time;
        }
        double n = completed_.size();
        stats_.avg_waiting_time = ws / n;
        stats_.avg_turnaround_time = ts / n;
        stats_.avg_response_time = rs / n;
        if (total_ticks > 0)
        {
            stats_.throughput = n / static_cast<double>(total_ticks);
            stats_.cpu_utilization = 1.0 - static_cast<double>(stats_.idle_ticks.load()) / static_cast<double>(total_ticks);
        }
        compute_fairness(completed_);
    }

} // namespace adaptive_sched