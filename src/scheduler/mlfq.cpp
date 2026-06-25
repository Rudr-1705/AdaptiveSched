#include "../../include/scheduler/mlfq.h"
#include "../../include/core/clock.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace adaptive_sched
{

    // Static constexpr definition (required in C++17 for ODR-used)
    constexpr std::array<Tick, MLFQScheduler::NUM_LEVELS> MLFQScheduler::LEVEL_QUANTA;

    MLFQScheduler::MLFQScheduler()
    {
        for (std::size_t i = 0; i < NUM_LEVELS; ++i)
        {
            QueueOrder ord = (i < NUM_LEVELS - 1) ? QueueOrder::FIFO : QueueOrder::FIFO;
            levels_[i] = std::make_unique<ReadyQueue>(ord);
        }
    }

    // ---------------------------------------------------------------------------
    // add_process — new process always enters at level 0
    // ---------------------------------------------------------------------------
    void MLFQScheduler::add_process(ProcessPtr proc)
    {
        proc->set_state(ProcessState::READY);
        proc->queue_level = 0;
        proc->quantum_remaining = LEVEL_QUANTA[0];
        levels_[0]->push(proc);
        stats_.processes_handled.fetch_add(1, std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------------------
    // get_next_process — always serve highest occupied level
    // ---------------------------------------------------------------------------
    ProcessPtr MLFQScheduler::get_next_process()
    {
        for (std::size_t lvl = 0; lvl < NUM_LEVELS; ++lvl)
        {
            auto proc = levels_[lvl]->try_pop();
            if (proc)
            {
                proc->set_state(ProcessState::RUNNING);
                if (proc->start_time == UINT64_MAX)
                {
                    proc->start_time = now();
                }
                // Arm quantum for this level
                Tick q = LEVEL_QUANTA[proc->queue_level];
                proc->quantum_remaining = (q == 0) ? proc->remaining_time : q;
                return proc;
            }
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------------
    // handle_preemption
    //   • Quantum expired (quantum_remaining == 0): demote and switch
    //   • Level 0 always preempts lower levels (interactive preference)
    // ---------------------------------------------------------------------------
    bool MLFQScheduler::handle_preemption(ProcessPtr running, ProcessPtr incoming)
    {
        if (!running)
            return false;

        // If a newly arrived process is at a higher level, preempt
        if (incoming && static_cast<std::size_t>(incoming->queue_level) <
                            static_cast<std::size_t>(running->queue_level))
        {
            running->set_state(ProcessState::READY);
            enqueue_at_level(running);
            stats_.preemptions.fetch_add(1, std::memory_order_relaxed);
            stats_.context_switches.fetch_add(1, std::memory_order_relaxed);
            running->ctx_switch.switch_count++;
            return true;
        }

        // Quantum exhausted → demote
        if (running->quantum_remaining == 0 && running->remaining_time > 0)
        {
            demote(running);
            stats_.preemptions.fetch_add(1, std::memory_order_relaxed);
            stats_.context_switches.fetch_add(1, std::memory_order_relaxed);
            running->ctx_switch.switch_count++;
            return true;
        }

        return false;
    }

    void MLFQScheduler::on_process_complete(ProcessPtr proc)
    {
        proc->record_completion(now());
        std::lock_guard<std::mutex> lk(completed_mtx_);
        completed_.push_back(proc);
    }

    // ---------------------------------------------------------------------------
    // on_tick — periodic global priority boost (anti-starvation)
    // ---------------------------------------------------------------------------
    void MLFQScheduler::on_tick(Tick current_time)
    {
        if (current_time - last_boost_tick_ >= BOOST_INTERVAL)
        {
            priority_boost(current_time);
            last_boost_tick_ = current_time;
        }
    }

    // ---------------------------------------------------------------------------
    // Internal helpers
    // ---------------------------------------------------------------------------
    void MLFQScheduler::enqueue_at_level(ProcessPtr proc)
    {
        std::size_t lvl = static_cast<std::size_t>(
            std::min(static_cast<int>(NUM_LEVELS - 1), proc->queue_level));
        proc->queue_level = static_cast<int>(lvl);
        Tick q = LEVEL_QUANTA[lvl];
        proc->quantum_remaining = (q == 0) ? proc->remaining_time : q;
        levels_[lvl]->push(proc);
    }

    void MLFQScheduler::demote(ProcessPtr proc)
    {
        proc->set_state(ProcessState::READY);
        std::size_t new_lvl = std::min(
            static_cast<std::size_t>(proc->queue_level) + 1, NUM_LEVELS - 1);
        proc->queue_level = static_cast<int>(new_lvl);
        enqueue_at_level(proc);
    }

    void MLFQScheduler::promote(ProcessPtr proc)
    {
        proc->set_state(ProcessState::READY);
        int new_lvl = std::max(0, proc->queue_level - 1);
        proc->queue_level = new_lvl;
        enqueue_at_level(proc);
    }

    void MLFQScheduler::priority_boost(Tick now_t)
    {
        // Collect all processes from levels 1..N and re-insert at level 0
        std::vector<ProcessPtr> to_boost;
        for (std::size_t lvl = 1; lvl < NUM_LEVELS; ++lvl)
        {
            ProcessPtr p;
            while ((p = levels_[lvl]->try_pop()) != nullptr)
            {
                to_boost.push_back(p);
            }
        }
        for (auto &p : to_boost)
        {
            p->queue_level = 0;
            p->aging.starvation_ticks = 0;
            p->aging.last_promoted_at = now_t;
            p->aging.boost_count++;
            p->quantum_remaining = LEVEL_QUANTA[0];
            levels_[0]->push(p);
            stats_.starvation_events.fetch_add(1, std::memory_order_relaxed);
        }
        if (!to_boost.empty())
        {
            std::cout << "[MLFQ] Priority boost: moved " << to_boost.size()
                      << " process(es) to level 0 at tick " << now_t << "\n";
        }
    }

    std::size_t MLFQScheduler::highest_occupied_level() const
    {
        for (std::size_t i = 0; i < NUM_LEVELS; ++i)
        {
            if (!levels_[i]->empty())
                return i;
        }
        return NUM_LEVELS;
    }

    // ---------------------------------------------------------------------------
    // IScheduler interface
    // ---------------------------------------------------------------------------
    std::size_t MLFQScheduler::ready_count() const
    {
        std::size_t total = 0;
        for (const auto &lv : levels_)
            total += lv->size();
        return total;
    }

    bool MLFQScheduler::has_ready_work() const
    {
        for (const auto &lv : levels_)
        {
            if (!lv->empty())
                return true;
        }
        return false;
    }

    std::vector<ProcessPtr> MLFQScheduler::snapshot() const
    {
        std::vector<ProcessPtr> all;
        for (const auto &lv : levels_)
        {
            auto s = lv->snapshot();
            all.insert(all.end(), s.begin(), s.end());
        }
        return all;
    }

    std::array<std::size_t, MLFQScheduler::NUM_LEVELS> MLFQScheduler::level_sizes() const
    {
        std::array<std::size_t, NUM_LEVELS> sz{};
        for (std::size_t i = 0; i < NUM_LEVELS; ++i)
            sz[i] = levels_[i]->size();
        return sz;
    }

    std::vector<ProcessPtr> MLFQScheduler::drain_queue()
    {
        std::vector<ProcessPtr> out;
        for (auto &lv : levels_)
        {
            ProcessPtr p;
            while ((p = lv->try_pop()) != nullptr)
                out.push_back(p);
        }
        return out;
    }

    void MLFQScheduler::import_queue(std::vector<ProcessPtr> procs)
    {
        for (auto &p : procs)
        {
            p->set_state(ProcessState::READY);
            // Preserve queue_level from previous scheduler if sensible;
            // otherwise cap to valid range
            p->queue_level = std::min(p->queue_level, (int)NUM_LEVELS - 1);
            enqueue_at_level(p);
        }
    }

    void MLFQScheduler::finalize_stats(Tick total_ticks)
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