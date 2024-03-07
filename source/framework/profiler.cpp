#include "profiler.hpp"

#include <algorithm>

namespace Profiler {

constexpr auto skip_profiler = true;

Duration DurationMeasure::TimePassedUntil(TimePoint now) const {
    return std::chrono::duration_cast<Duration>(now - begin);
}

Metric Accumulator::SnapshotFor(TimePoint now) const {
    // Careful here for race-conditions!

    // If paused, immediately return the metric. If paused gets toggled before returning, metric will be mildy out of date at worst.
    if (paused) {
        return metric;
    }

    // Get the current metric.
    // TODO: Race condition here - measure.begin might get updated after taking the metric snapshot :/
    auto snapshot_metric = metric; // TODO: Copying this should be ensured atomic!
    auto snapshot_measure = measure;

    auto time_passed = snapshot_measure.TimePassedUntil(now);

    // If measurement begin is later than "now" (e.g. due to cross-thread
    // access), assume the activity was interrupted before and hence no
    // metrics were gathered.
    snapshot_metric.total += std::max(time_passed, std::chrono::duration_values<Duration>::zero());

    return snapshot_metric;
}

Activity& Activity::GetSubActivity(std::string_view name) {
    auto it = std::find_if(sub_activities.begin(), sub_activities.end(), [=](auto& activity) { return activity.name == name; });
    if (it != sub_activities.end()) {
        return *it;
    }

    return sub_activities.emplace_back(*this, name);
}

void Activity::ResumeFrom(TimePoint time) {
    if constexpr (skip_profiler) {
        return;
    }

    if (!parent) {
        // The root activity always stays active
        return;
    }

    assert(!IsMeasuring());

    // Resume parent first, just to make sure they always get a slightly higher total metric
    if (!parent->IsMeasuring()) {
        parent->ResumeFrom(time);
    }

    if (!accumulator) {
        accumulator = std::make_unique<Accumulator>(metric, time);
        assert(IsMeasuring());
    } else {
        assert(!IsMeasuring());
        accumulator->Resume(time);
    }
}

void Activity::InterruptAt(TimePoint time) {
    if constexpr (skip_profiler) {
        return;
    }

    // Interrupt children first, to make sure the parent always has a slightly higher total metric
    for (auto& activity : sub_activities) {
        if (activity.IsMeasuring()) {
            activity.InterruptAt(time);
        }
    }

    if (!IsMeasuring()) {
        throw std::runtime_error("Called Interrupt on Activity " + name + " which wasn't active");
    }
    accumulator->Interrupt(time);
}

void Activity::Resume() {
    if constexpr (skip_profiler) {
        return;
    }

    auto time = std::chrono::steady_clock::now();
    ResumeFrom(time);
}

void Activity::Interrupt() {
    if constexpr (skip_profiler) {
        return;
    }

    auto time = std::chrono::steady_clock::now();
    InterruptAt(time);
}

void Activity::SwitchSubActivity(std::string_view from, std::string_view to) {
    if constexpr (skip_profiler) {
        return;
    }

    auto time = std::chrono::steady_clock::now();
    GetSubActivity(from).InterruptAt(time);
    GetSubActivity(to).ResumeFrom(time);
}

FrozenMetrics Activity::GetMetricsAt(TimePoint now) const {
    // TODO: Race condition here. What if IsMeasuring changes before we take the snapshot?
    auto metrics = IsMeasuring() ? accumulator->SnapshotFor(now) : metric;

    FrozenMetrics ret { *this, metrics, {} };
    // TODO: Verify iteration over std::list is thread-safe
    if constexpr (!skip_profiler) {
    for (auto& activity : sub_activities) {
        ret.sub_metrics.emplace_back(activity.GetMetricsAt(now));
    }
    }
    return std::move(ret);
}

FrozenMetrics Profiler::Freeze() const {
    // TODO: Achieve this without interrupting any activity, and don't assume no activity is running right now!
//    root_activity->Interrupt();
//    root_activity->accumulator->Resume();

    return root_activity->GetMetricsAt(std::chrono::steady_clock::now());
}

// TODO: MeasureScope should get the previously active activity and restore it!

} // namespace Profiler
