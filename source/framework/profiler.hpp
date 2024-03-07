#pragma once

#include <cassert>
#include <chrono>
#include <exception>
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/mp11/algorithm.hpp>
#include <boost/mp11/list.hpp>

namespace Profiler {

using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;
using Duration = std::chrono::microseconds;

/**
 * A DurationMeasure measures the time from a fixed starting point in time to
 * later moments in time.
 */
class DurationMeasure {
    TimePoint begin;

public:
    DurationMeasure(TimePoint begin) : begin(begin) {
    }

    Duration TimePassedUntil(TimePoint) const;
};

struct Metric {
    /// Total time spend on the given task
    /// TODO: Should use an atomic for this!
    Duration total;
};

class Accumulator {
    friend class Activity;

    Accumulator(const Accumulator&) = delete;
    Accumulator& operator=(const Accumulator&) = delete;

    Metric& metric;

    DurationMeasure measure;

    bool paused = false;

public:
    Accumulator(Metric& metric, TimePoint begin) : metric(metric), measure(begin) {
    }

    ~Accumulator() {
    }

    void Interrupt(TimePoint end) {
        metric.total += measure.TimePassedUntil(end);
        paused = true;
    }

    /// Resume after prior interruption
    void Resume(TimePoint begin) {
        measure = DurationMeasure(begin);
        paused = false;
    }

    bool IsPaused() const {
        return paused;
    }

    /**
     * Returns metrics for the given TimePoint.
     * @note If the given time point is earlier than begin, it is assumed that
     *       measuring has been interrupted throughout the entire time between
     *       the two TimePoints.
     * Thread-safe.
     */
    Metric SnapshotFor(TimePoint) const;
};

class Activity;

struct FrozenMetrics {
    const Activity& activity;

    Metric metric;

    std::vector<FrozenMetrics> sub_metrics;
};

/**
 * The life time of these should last until the end of the program so that references are guaranteed to be stable.
 */
class Activity {
public: // TODO: Iteration interface!
    std::string name;
    Metric metric = {};

    // Parent activity, or nullptr for the root activity
    Activity* parent = nullptr;

    // Using std::list here because we need stable references when appending new activities.
    // TODO: Use something that has more efficient iteration, and random-access for compile-time-specified activities!
    // We also need thread-safe iteration while adding new elements at the end of the list.
    // Invariant: Not more than one of these IsMeasuring at any point in time.
    // Invariant: !IsMeasuring implies no sub activity IsMeasuring.
    // NOTE: The first entries are occupied by compile-time activities
//    std::list<Activity> sub_activities;
    std::list<Activity> sub_activities;

    // @pre: Only active if parent->accumulator is active
    // TODO: Why is this a unique_ptr again?
    std::unique_ptr<Accumulator> accumulator;

    /// Creates the root activity in the measuring state
    Activity() : name("root") {
        accumulator = std::make_unique<Accumulator>(metric, std::chrono::steady_clock::now());
    }

    friend class Profiler;

    bool IsMeasuring() const {
        return accumulator && !accumulator->IsPaused();
    }

    void InterruptAt(TimePoint);

    void ResumeFrom(TimePoint);
public:
    /// Creates the activity in the stopped state
    Activity(Activity& parent, std::string_view name) : name(name), parent(&parent) {
    }

    /**
     * Look up or create new sub task with the given name
     */
    Activity& GetSubActivity(std::string_view name);

    /**
     * Interrupt all measurements in this activity, including any subactivities
     */
    void Interrupt();

    /**
     * Starts measurement after construction or resumes measurements on this activity after a prior call to Interrupt().
     * Resumption propagates to all parent activities up to the root.
     */
    void Resume();

    void SwitchSubActivity(std::string_view from, std::string_view to);

    // Get a snapshot of the metrics for this and all child activities
    // Thread-safe.
    FrozenMetrics GetMetricsAt(TimePoint) const;

    std::string_view GetName() const {
        return name;
    }
};

/// Compile-time tag used for zero-cost lookup of activities with tags in the Activities namespace
template<typename ActivityTag>
struct TaggedActivity {
    using SubTags = typename ActivityTag::tags;

    Activity activity;

    TaggedActivity() {
        auto populate_activity = [](Activity& activity, auto tag, auto&& self) {
            boost::mp11::mp_for_each<typename decltype(tag)::tags>([&activity, self](auto subtag) {
//                activity.sub_activities.reserve(100);
                auto& sub_activity = activity.GetSubActivity(subtag.name);
                self(sub_activity, subtag, self);
            });
            ////            // Trigger creation by activity lookup
            ////            static_cast<void>(activity.GetSubActivity(tag.name));
        };
        populate_activity(activity, ActivityTag{}, populate_activity);

//        boost::mp11::mp_for_each<SubTags>([activity=&this->activity](auto tag) {
////            // Trigger creation by activity lookup
////            static_cast<void>(activity.GetSubActivity(tag.name));
//            TaggedActivity<decltype(tag)> sub_activity;
//            activity.sub_activities.emplace_back(std::move(sub_activity.activity));
//        });
    }

    Activity& operator*() {
        return activity;
    }

    Activity* operator->() {
        return &activity;
    }

    const Activity& operator*() const {
        return activity;
    }

    const Activity* operator->() const {
        return &activity;
    }

    template<typename SubActivityTag>
    TaggedActivity<SubActivityTag>& GetSubActivity() {
        constexpr size_t index = boost::mp11::mp_find<SubTags, SubActivityTag>::value;
        static_assert(index != boost::mp11::mp_size<SubTags>::value, "Given tag is not a child of this activity");

        // reinterpret_cast here is (hopefully) fine since Activity is the only member in TaggedActivity
        auto& result = *std::next(activity.sub_activities.begin(), index);
        if (result.name != SubActivityTag::name) {
            throw std::runtime_error("WTF? " + result.name + " <-> " + std::string{SubActivityTag::name});
        }
        return reinterpret_cast<TaggedActivity<SubActivityTag>&>(result);
//        return reinterpret_cast<TaggedActivity<SubActivityTag>&>(*std::next(activity.sub_activities.begin(), index));
    }

//    template<typename FirstTag, typename SecondTag, typename... SubActivityTags>
//    auto GetSubActivity() {
//        auto& sub_activity = GetSubActivity<FirstTag>();
//        return sub_activity.template GetSubActivity<SecondTag, SubActivityTags...>();
//    }

    Activity& GetSubActivity(std::string_view name) {
        return activity.GetSubActivity(name);
    }
};

namespace Activities {

using std::string_view_literals::operator""sv;

template<typename... SubActivities>
struct ActivityTag {
    // subset of child activities that can be looked up at compile-time using their type rather than a runtime name
    using tags = boost::mp11::mp_list<SubActivities...>;
};

struct OS : ActivityTag<> { static constexpr auto name = "OS"sv; };

struct Loader : ActivityTag<> { static constexpr auto name = "Loader"sv; };
struct Shader : ActivityTag<> { static constexpr auto name = "Shader"sv; };
struct InputAssembly : ActivityTag<> { static constexpr auto name = "InputAssembly"sv; };

struct VertexLoader : ActivityTag<Loader, Shader, InputAssembly> { static constexpr auto name = "VertexLoader"sv; };

struct SubmitBatch : ActivityTag<VertexLoader> { static constexpr auto name = "SubmitBatch"sv; };

struct GPU : ActivityTag<SubmitBatch> { static constexpr auto name = "GPU"sv; };

struct Root : ActivityTag<OS, GPU> { static constexpr auto name = "root"sv; };

} // namespace Activities

class Profiler {
    /*
        struct Timeline {
            // TODO: Keep list of events/tasks: std::vector<START, std::variant<Event, Task*>>
            Task* active_task;
        };
    */

    // TODO: Keep separate root Activity per thread!
//    Activity root_activity;
    TaggedActivity<Activities::Root> root_activity;

public:
    Profiler() {
        root_activity->Resume();
    }

    Activity& GetActivity(std::string_view name) {
        return root_activity->GetSubActivity(name);
    }

    // Get a snapshot of the metrics for all activities
    // Thread-safe.
    FrozenMetrics Freeze() const;

    // TODO: Activity GetActive() => Should be able to dynamically add a sub activity for the current one!
};

// Resumes the subactivity for the current scope; at scope exit, the entire activity is interrupted
// This function may be called with multiple sub category strings, e.g. MeasureScope(activity, "SubTask", "SubSubTask")
template<typename... SubCategories>
inline auto MeasureScope(Activity& activity, SubCategories&&... subs) {
    struct ScopeGuard {
        Activity& activity;

        ~ScopeGuard() noexcept(false) {
            // If there are uncaught exceptions, we probably end up in an unexpected category.
            // Just abort profiling in this case hence
            if (!std::uncaught_exceptions()) {
                activity.Interrupt();
            }
        }
    };
    // Successively iterate into sub categories based on the given strings
    Activity* sub_activity = &activity;
    ((sub_activity = &sub_activity->GetSubActivity(std::forward<SubCategories>(subs))), ...);
    sub_activity->Resume();

    // On scope exit, interrupt the parent activity
    return ScopeGuard { activity };
}

} // namespace Profiler
