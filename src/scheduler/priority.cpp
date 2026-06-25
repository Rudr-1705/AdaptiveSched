#include "../../include/scheduler/priority.h"
#include "../../include/core/clock.h"
#include <iostream>
#include <algorithm>

namespace adaptive_sched
{

    PriorityScheduler::PriorityScheduler()
        : queue_(QueueOrder::PRIORITY)
    {
    }

    void PriorityScheduler::add_process(ProcessPtr proc)
    {
        proc->set_state(ProcessState::READY);
        proc->aging.starvation_ticks = 0;
        queue_.push(proc);
        stats_.processes_handled.fetch_add(1, std::memory_order_relaxed);
    }

    ProcessPtr PriorityScheduler::get_next_process()
    {
        auto proc = queue_.try_pop();
        if (proc)
        {
            proc->set_state(ProcessState::RUNNING);
            if (proc->start_time == UINT64_MAX)
                proc->start_time = now();
            proc->quantum_remaining = proc->remaining_time;
        }
        return proc;
    }

    bool PriorityScheduler::handle_preemption(ProcessPtr running, ProcessPtr incoming)
    {
        if (!running || !incoming)
            return false;

        if (incoming->priority < running->priority)
        {
            running->set_state(ProcessState::READY);
            queue_.push(running);
            stats_.preemptions.fetch_add(1, std::memory_order_relaxed);
            stats_.context_switches.fetch_add(1, std::memory_order_relaxed);
            running->ctx_switch.switch_count++;
            return true;
        }
        return false;
    }

    void PriorityScheduler::on_process_complete(ProcessPtr proc)
    {
        proc->record_completion(now());
        std::lock_guard<std::mutex> lk(completed_mtx_);
        completed_.push_back(proc);
    }

    void PriorityScheduler::on_tick(Tick current_time)
    {
        if (current_time % AGING_THRESHOLD == 0)
            apply_aging(current_time);
    }

    void PriorityScheduler::apply_aging(Tick now_t)
    {
        auto snap = queue_.snapshot();
        bool any_boosted = false;
        for (auto &proc : snap)
        {
            proc->aging.starvation_ticks++;
            if (proc->aging.starvation_ticks >= AGING_THRESHOLD &&
                proc->aging.boost_count < MAX_PRIORITY_BOOSTS &&
                proc->priority > 0)
            {
                proc->priority = std::max(0, proc->priority - AGING_BOOST);
                proc->aging.starvation_ticks = 0;
                proc->aging.last_promoted_at = now_t;
                proc->aging.boost_count++;
                any_boosted = true;
                stats_.starvation_events.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (any_boosted)
            rebalance();
    }

    void PriorityScheduler::rebalance()
    {
        queue_.resort();
    }

    std::size_t PriorityScheduler::ready_count() const { return queue_.size(); }
    bool PriorityScheduler::has_ready_work() const { return !queue_.empty(); }
    std::vector<ProcessPtr> PriorityScheduler::snapshot() const { return queue_.snapshot(); }

    std::vector<ProcessPtr> PriorityScheduler::drain_queue()
    {
        std::vector<ProcessPtr> out;
        ProcessPtr p;
        while ((p = queue_.try_pop()) != nullptr)
            out.push_back(p);
        return out;
    }

    void PriorityScheduler::import_queue(std::vector<ProcessPtr> procs)
    {
        for (auto &p : procs)
        {
            p->set_state(ProcessState::READY);
            queue_.push(p);
        }
    }

    void PriorityScheduler::finalize_stats(Tick total_ticks)
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
        double n = static_cast<double>(completed_.size());
        stats_.avg_waiting_time = ws / n;
        stats_.avg_turnaround_time = ts / n;
        stats_.avg_response_time = rs / n;
        if (total_ticks > 0)
        {
            stats_.throughput = n / static_cast<double>(total_ticks);
            stats_.cpu_utilization =
                1.0 - static_cast<double>(stats_.idle_ticks.load()) / static_cast<double>(total_ticks);
        }
        compute_fairness(completed_);
    }

} // namespace adaptive_sched