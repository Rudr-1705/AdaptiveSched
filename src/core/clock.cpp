#include "../../include/core/clock.h"
#include <stdexcept>

namespace adaptive_sched
{

    SimClock &SimClock::instance()
    {
        static SimClock inst;
        return inst;
    }

    void SimClock::tick()
    {
        Tick t = current_tick_.fetch_add(1, std::memory_order_acq_rel) + 1;
        fire_events(t);
    }

    void SimClock::advance(Tick n)
    {
        for (Tick i = 0; i < n; ++i)
        {
            tick();
        }
    }

    void SimClock::reset()
    {
        current_tick_.store(0, std::memory_order_release);
        std::lock_guard<std::mutex> lk(events_mtx_);
        one_shot_events_.clear();
        periodic_events_.clear();
    }

    void SimClock::schedule_event(Tick at, Callback cb)
    {
        std::lock_guard<std::mutex> lk(events_mtx_);
        one_shot_events_.emplace(at, std::move(cb));
    }

    void SimClock::add_periodic(Tick interval, Callback cb)
    {
        std::lock_guard<std::mutex> lk(events_mtx_);
        Tick now_val = current_tick_.load(std::memory_order_acquire);
        periodic_events_.push_back({interval, now_val + interval, std::move(cb)});
    }

    void SimClock::fire_events(Tick t)
    {
        std::lock_guard<std::mutex> lk(events_mtx_);

        // One-shot events
        auto range = one_shot_events_.equal_range(t);
        for (auto it = range.first; it != range.second; ++it)
        {
            it->second(t);
        }
        one_shot_events_.erase(range.first, range.second);

        // Periodic events
        for (auto &pe : periodic_events_)
        {
            if (t >= pe.next_fire)
            {
                pe.cb(t);
                pe.next_fire = t + pe.interval;
            }
        }
    }

} // namespace adaptive_sched