#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace adaptive_sched
{

    using Tick = uint64_t;

    // ---------------------------------------------------------------------------
    // SimClock — deterministic, tick-based logical simulation clock
    //
    // Design rationale:
    //   Real wall-clock time is non-deterministic and machine-dependent.
    //   A logical tick allows repeatable, inspectable simulation runs independent
    //   of hardware speed.  All timing throughout AdaptiveSched++ is expressed
    //   in ticks.
    // ---------------------------------------------------------------------------
    class SimClock
    {
    public:
        // Singleton accessor
        static SimClock &instance();

        // Advance time by one tick; fires registered callbacks
        void tick();

        // Advance time by n ticks (useful for fast-forwarding idle intervals)
        void advance(Tick n);

        // Current simulation time
        Tick now() const { return current_tick_.load(std::memory_order_acquire); }

        // Reset to zero (used between benchmark runs)
        void reset();

        // Event callback: called once when the clock reaches 'at'
        using Callback = std::function<void(Tick)>;
        void schedule_event(Tick at, Callback cb);

        // Recurring callback: fired every 'interval' ticks
        void add_periodic(Tick interval, Callback cb);

    private:
        SimClock() = default;
        SimClock(const SimClock &) = delete;
        SimClock &operator=(const SimClock &) = delete;

        std::atomic<Tick> current_tick_{0};

        mutable std::mutex events_mtx_;
        std::multimap<Tick, Callback> one_shot_events_;

        struct PeriodicEntry
        {
            Tick interval;
            Tick next_fire;
            Callback cb;
        };
        std::vector<PeriodicEntry> periodic_events_;

        void fire_events(Tick t);
    };

    // ---------------------------------------------------------------------------
    // Convenience free-functions that forward to the singleton
    // ---------------------------------------------------------------------------
    inline Tick now() { return SimClock::instance().now(); }
    inline void tick() { SimClock::instance().tick(); }
    inline void advance(Tick n) { SimClock::instance().advance(n); }

} // namespace adaptive_sched