#include "../../include/scheduler/rr.h"
#include "../../include/core/clock.h"
#include <iostream>

namespace adaptive_sched
{

    RRScheduler::RRScheduler(Tick quantum, Tick ctx_switch_cost)
        : quantum_(quantum), ctx_switch_cost_(ctx_switch_cost), queue_(QueueOrder::FIFO)
    {
    }

    void RRScheduler::add_process(ProcessPtr proc)
    {
        proc->set_state(ProcessState::READY);
        proc->quantum_remaining = quantum_;
        queue_.push(proc);
        stats_.processes_handled.fetch_add(1, std::memory_order_relaxed);
    }

    ProcessPtr RRScheduler::get_next_process()
    {
        auto proc = queue_.try_pop();
        if (proc)
        {
            proc->set_state(ProcessState::RUNNING);
            if (proc->start_time == UINT64_MAX)
            {
                proc->start_time = now();
            }
            // (Re)assign quantum
            proc->quantum_remaining = quantum_;
        }
        return proc;
    }

    // handle_preemption for RR:
    //   - Called when the running process's quantum expires.
    //   - If remaining_time > 0, re-enqueue at tail and switch.
    bool RRScheduler::handle_preemption(ProcessPtr running, ProcessPtr /*incoming*/)
    {
        if (!running)
            return false;

        // Quantum exhausted?
        if (running->quantum_remaining == 0 && running->remaining_time > 0)
        {
            running->set_state(ProcessState::READY);
            running->quantum_remaining = quantum_;
            queue_.push(running); // back to tail
            stats_.preemptions.fetch_add(1, std::memory_order_relaxed);
            stats_.context_switches.fetch_add(1, std::memory_order_relaxed);
            running->ctx_switch.switch_count++;
            return true;
        }
        return false;
    }

    void RRScheduler::on_process_complete(ProcessPtr proc)
    {
        proc->record_completion(now());
        std::lock_guard<std::mutex> lk(completed_mtx_);
        completed_.push_back(proc);
    }

    std::size_t RRScheduler::ready_count() const { return queue_.size(); }
    bool RRScheduler::has_ready_work() const { return !queue_.empty(); }
    std::vector<ProcessPtr> RRScheduler::snapshot() const { return queue_.snapshot(); }

    std::vector<ProcessPtr> RRScheduler::drain_queue()
    {
        std::vector<ProcessPtr> out;
        ProcessPtr p;
        while ((p = queue_.try_pop()) != nullptr)
            out.push_back(p);
        return out;
    }

    void RRScheduler::import_queue(std::vector<ProcessPtr> procs)
    {
        for (auto &p : procs)
        {
            p->set_state(ProcessState::READY);
            p->quantum_remaining = quantum_; // re-arm quantum on arrival
            queue_.push(p);
        }
    }

    void RRScheduler::finalize_stats(Tick total_ticks)
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