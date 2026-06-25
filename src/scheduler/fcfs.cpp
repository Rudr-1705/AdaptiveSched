#include "../../include/scheduler/fcfs.h"
#include "../../include/core/clock.h"
#include <iostream>
#include <numeric>
#include <cmath>

namespace adaptive_sched
{

    FCFSScheduler::FCFSScheduler()
        : queue_(QueueOrder::FIFO)
    {
    }

    void FCFSScheduler::add_process(ProcessPtr proc)
    {
        proc->set_state(ProcessState::READY);
        queue_.push(proc);
        stats_.processes_handled.fetch_add(1, std::memory_order_relaxed);
    }

    ProcessPtr FCFSScheduler::get_next_process()
    {
        auto proc = queue_.try_pop();
        if (proc)
        {
            proc->set_state(ProcessState::RUNNING);
            if (proc->start_time == UINT64_MAX)
            {
                proc->start_time = now();
            }
            // Assign full remaining burst as quantum (non-preemptive)
            proc->quantum_remaining = proc->remaining_time;
        }
        return proc;
    }

    // FCFS is non-preemptive: always returns false (no preemption)
    bool FCFSScheduler::handle_preemption(ProcessPtr running, ProcessPtr /*incoming*/)
    {
        (void)running;
        // No preemption in FCFS — convoy effect is intentional
        return false;
    }

    void FCFSScheduler::on_process_complete(ProcessPtr proc)
    {
        Tick t = now();
        proc->record_completion(t);
        std::lock_guard<std::mutex> lk(completed_mtx_);
        completed_.push_back(proc);
    }

    std::size_t FCFSScheduler::ready_count() const
    {
        return queue_.size();
    }

    bool FCFSScheduler::has_ready_work() const
    {
        return !queue_.empty();
    }

    std::vector<ProcessPtr> FCFSScheduler::snapshot() const
    {
        return queue_.snapshot();
    }

    std::vector<ProcessPtr> FCFSScheduler::drain_queue()
    {
        std::vector<ProcessPtr> drained;
        ProcessPtr p;
        while ((p = queue_.try_pop()) != nullptr)
        {
            drained.push_back(p);
        }
        return drained;
    }

    void FCFSScheduler::import_queue(std::vector<ProcessPtr> procs)
    {
        for (auto &p : procs)
        {
            p->set_state(ProcessState::READY);
            queue_.push(p);
        }
    }

    void FCFSScheduler::finalize_stats(Tick total_ticks)
    {
        std::lock_guard<std::mutex> lk(completed_mtx_);
        if (completed_.empty())
            return;

        double wait_sum = 0.0;
        double tat_sum = 0.0;
        double resp_sum = 0.0;
        for (const auto &p : completed_)
        {
            wait_sum += static_cast<double>(p->waiting_time);
            tat_sum += static_cast<double>(p->turnaround_time);
            resp_sum += static_cast<double>(p->response_time);
        }
        double n = static_cast<double>(completed_.size());
        stats_.avg_waiting_time = wait_sum / n;
        stats_.avg_turnaround_time = tat_sum / n;
        stats_.avg_response_time = resp_sum / n;

        if (total_ticks > 0)
        {
            stats_.throughput = n / static_cast<double>(total_ticks);
            stats_.cpu_utilization = 1.0 - (static_cast<double>(stats_.idle_ticks.load()) / static_cast<double>(total_ticks));
        }
        compute_fairness(completed_);
    }

} // namespace adaptive_sched