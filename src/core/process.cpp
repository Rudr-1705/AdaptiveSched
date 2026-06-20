#include "../../include/core/process.h"
#include <sstream>
#include <iomanip>

namespace adaptive_sched
{

    Process::Process(PID pid, std::string name, Tick arrival, Tick burst,
                     int priority, bool interactive)
        : pid(pid), name(std::move(name)), arrival_time(arrival), burst_time(burst), remaining_time(burst), priority(priority), base_priority(priority), is_interactive(interactive)
    {
        atomic_state.store(ProcessState::NEW, std::memory_order_release);
    }

    void Process::set_state(ProcessState s)
    {
        atomic_state.store(s, std::memory_order_release);
        state = s;
    }

    ProcessState Process::get_state() const
    {
        return atomic_state.load(std::memory_order_acquire);
    }

    void Process::record_completion(Tick now)
    {
        completion_time = now;
        turnaround_time = completion_time - arrival_time;
        // waiting_time = turnaround - actual execution time
        waiting_time = (turnaround_time > burst_time) ? (turnaround_time - burst_time) : 0;
        if (start_time != UINT64_MAX)
        {
            response_time = start_time - arrival_time;
        }
        else
        {
            response_time = 0;
        }
        set_state(ProcessState::TERMINATED);
    }

    void Process::apply_aging(Tick now, int age_threshold, int boost_amount)
    {
        aging.starvation_ticks++;
        if (aging.starvation_ticks >= age_threshold && priority > 0)
        {
            priority = std::max(0, priority - boost_amount);
            aging.starvation_ticks = 0;
            aging.last_promoted_at = now;
            aging.boost_count++;
        }
    }

    std::string Process::summary() const
    {
        std::ostringstream oss;
        oss << "PID=" << std::setw(4) << pid
            << " [" << std::setw(12) << name << "] "
            << "arr=" << std::setw(4) << arrival_time
            << " burst=" << std::setw(4) << burst_time
            << " rem=" << std::setw(4) << remaining_time
            << " pri=" << std::setw(3) << priority
            << " state=" << to_string(get_state())
            << " WT=" << std::setw(4) << waiting_time
            << " TAT=" << std::setw(4) << turnaround_time
            << " RT=" << std::setw(4) << response_time;
        return oss.str();
    }

} // namespace adaptive_sched