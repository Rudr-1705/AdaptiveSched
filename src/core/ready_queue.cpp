#include "../../include/core/ready_queue.h"
#include <algorithm>
#include <stdexcept>

namespace adaptive_sched
{

    // ---------------------------------------------------------------------------
    // Comparators
    // ---------------------------------------------------------------------------
    bool ReadyQueue::cmp_sjf(const ProcessPtr &a, const ProcessPtr &b)
    {
        // Shorter remaining time has priority (min-heap behaviour in deque)
        if (a->remaining_time != b->remaining_time)
            return a->remaining_time < b->remaining_time;
        return a->arrival_time < b->arrival_time; // tie-break: FCFS
    }

    bool ReadyQueue::cmp_priority(const ProcessPtr &a, const ProcessPtr &b)
    {
        // Lower priority number = higher urgency
        if (a->priority != b->priority)
            return a->priority < b->priority;
        return a->arrival_time < b->arrival_time;
    }

    // ---------------------------------------------------------------------------
    // Constructor
    // ---------------------------------------------------------------------------
    ReadyQueue::ReadyQueue(QueueOrder order, Comparator custom_cmp)
        : order_(order), custom_cmp_(std::move(custom_cmp))
    {
    }

    // ---------------------------------------------------------------------------
    // insert_ordered — place a new process into the sorted deque
    // ---------------------------------------------------------------------------
    void ReadyQueue::insert_ordered(ProcessPtr proc)
    {
        switch (order_)
        {
        case QueueOrder::FIFO:
            queue_.push_back(std::move(proc));
            break;

        case QueueOrder::SJF:
        {
            auto it = std::lower_bound(queue_.begin(), queue_.end(), proc,
                                       cmp_sjf);
            queue_.insert(it, std::move(proc));
        }
        break;

        case QueueOrder::PRIORITY:
        {
            auto it = std::lower_bound(queue_.begin(), queue_.end(), proc,
                                       cmp_priority);
            queue_.insert(it, std::move(proc));
        }
        break;

        case QueueOrder::CUSTOM:
            if (custom_cmp_)
            {
                auto it = std::lower_bound(queue_.begin(), queue_.end(), proc,
                                           custom_cmp_);
                queue_.insert(it, std::move(proc));
            }
            else
            {
                queue_.push_back(std::move(proc));
            }
            break;
        }
    }

    // ---------------------------------------------------------------------------
    // push
    // ---------------------------------------------------------------------------
    void ReadyQueue::push(ProcessPtr proc)
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (shutdown_)
                return;
            insert_ordered(proc);
        }
        cv_.notify_one();
    }

    void ReadyQueue::push_all(std::vector<ProcessPtr> procs)
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (shutdown_)
                return;
            for (auto &p : procs)
            {
                insert_ordered(p);
            }
        }
        cv_.notify_all();
    }

    // ---------------------------------------------------------------------------
    // pop (blocking)
    // ---------------------------------------------------------------------------
    ProcessPtr ReadyQueue::pop()
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this]
                 { return !queue_.empty() || shutdown_; });
        if (queue_.empty())
            return nullptr;
        auto proc = queue_.front();
        queue_.pop_front();
        return proc;
    }

    ProcessPtr ReadyQueue::pop_for(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait_for(lk, timeout, [this]
                     { return !queue_.empty() || shutdown_; });
        if (queue_.empty())
            return nullptr;
        auto proc = queue_.front();
        queue_.pop_front();
        return proc;
    }

    ProcessPtr ReadyQueue::try_pop()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (queue_.empty())
            return nullptr;
        auto proc = queue_.front();
        queue_.pop_front();
        return proc;
    }

    // ---------------------------------------------------------------------------
    // Inspection
    // ---------------------------------------------------------------------------
    std::vector<ProcessPtr> ReadyQueue::snapshot() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return {queue_.begin(), queue_.end()};
    }

    std::size_t ReadyQueue::size() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return queue_.size();
    }

    bool ReadyQueue::empty() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return queue_.empty();
    }

    bool ReadyQueue::remove(PID pid)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = std::find_if(queue_.begin(), queue_.end(),
                               [pid](const ProcessPtr &p)
                               { return p->pid == pid; });
        if (it == queue_.end())
            return false;
        queue_.erase(it);
        return true;
    }

    ProcessPtr ReadyQueue::peek_shortest() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (queue_.empty())
            return nullptr;
        // For SJF queues the front is already shortest; for others we scan
        return *std::min_element(queue_.begin(), queue_.end(), cmp_sjf);
    }

    ProcessPtr ReadyQueue::peek_highest_priority() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (queue_.empty())
            return nullptr;
        return *std::min_element(queue_.begin(), queue_.end(), cmp_priority);
    }

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------
    void ReadyQueue::shutdown()
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

    void ReadyQueue::resort()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<ProcessPtr> tmp(queue_.begin(), queue_.end());
        queue_.clear();
        for (auto &p : tmp)
            insert_ordered(p);
    }

} // namespace adaptive_sched