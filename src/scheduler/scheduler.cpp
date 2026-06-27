#include "../../include/scheduler/scheduler.h"
#include <numeric>
#include <cmath>

namespace adaptive_sched
{

    void IScheduler::compute_fairness(const std::vector<ProcessPtr> &completed)
    {
        if (completed.empty())
        {
            stats_.fairness_index = 1.0;
            return;
        }

        // Jain's fairness index on waiting times:
        // J = (sum(x_i))^2 / (n * sum(x_i^2))
        double sum = 0.0;
        double sumSq = 0.0;
        double n = static_cast<double>(completed.size());

        for (const auto &p : completed)
        {
            double w = static_cast<double>(p->waiting_time);
            sum += w;
            sumSq += w * w;
        }

        if (sumSq < 1e-9)
        {
            stats_.fairness_index = 1.0; // all equal → perfectly fair
        }
        else
        {
            stats_.fairness_index = (sum * sum) / (n * sumSq);
        }
    }

} // namespace adaptive_sched