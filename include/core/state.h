#pragma once

#include <string>

namespace adaptive_sched
{

    enum class ProcessState
    {
        NEW,
        READY,
        RUNNING,
        BLOCKED,
        TERMINATED
    };

    inline std::string to_string(ProcessState s)
    {
        switch (s)
        {
        case ProcessState::NEW:
            return "NEW";
        case ProcessState::READY:
            return "READY";
        case ProcessState::RUNNING:
            return "RUNNING";
        case ProcessState::BLOCKED:
            return "BLOCKED";
        case ProcessState::TERMINATED:
            return "TERMINATED";
        default:
            return "UNKNOWN";
        }
    }

    enum class WorkloadType
    {
        CPU_BOUND,
        INTERACTIVE,
        MIXED,
        BATCH,
        STRESS
    };

    inline std::string to_string(WorkloadType w)
    {
        switch (w)
        {
        case WorkloadType::CPU_BOUND:
            return "CPU_BOUND";
        case WorkloadType::INTERACTIVE:
            return "INTERACTIVE";
        case WorkloadType::MIXED:
            return "MIXED";
        case WorkloadType::BATCH:
            return "BATCH";
        case WorkloadType::STRESS:
            return "STRESS";
        default:
            return "UNKNOWN";
        }
    }

    enum class SchedulerPolicy
    {
        FCFS,
        SJF,
        SRTF,
        ROUND_ROBIN,
        PRIORITY,
        MLFQ,
        ADAPTIVE
    };

    inline std::string to_string(SchedulerPolicy p)
    {
        switch (p)
        {
        case SchedulerPolicy::FCFS:
            return "FCFS";
        case SchedulerPolicy::SJF:
            return "SJF";
        case SchedulerPolicy::SRTF:
            return "SRTF";
        case SchedulerPolicy::ROUND_ROBIN:
            return "ROUND_ROBIN";
        case SchedulerPolicy::PRIORITY:
            return "PRIORITY";
        case SchedulerPolicy::MLFQ:
            return "MLFQ";
        case SchedulerPolicy::ADAPTIVE:
            return "ADAPTIVE";
        default:
            return "UNKNOWN";
        }
    }

} // namespace adaptive_sched