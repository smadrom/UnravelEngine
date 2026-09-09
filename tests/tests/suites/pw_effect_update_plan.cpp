#include "../tests.h"
#include <engine/pw/pw_map_effects.h>
#include <algorithm>
#include <cstdio>
#include <set>

using namespace unravel;

namespace
{
int failures = 0;
int checks = 0;

void check(bool value, const char* text)
{
    ++checks;
    if(!value)
    {
        ++failures;
        std::printf("  FAIL: %s\n", text);
    }
}

auto make_instances(std::initializer_list<float> x_positions) -> std::vector<pw_effect_instance>
{
    std::vector<pw_effect_instance> result;
    for(const float x : x_positions)
    {
        pw_effect_instance instance;
        instance.position = {x, 0.0f, 0.0f};
        result.push_back(std::move(instance));
    }
    return result;
}

auto run_pw_effect_update_plan_suite(rtti::context&) -> int
{
    failures = 0;
    checks = 0;

    // No observers: everything simulates, nothing freezes, regardless of radius.
    {
        const auto instances = make_instances({0.0f, 500.0f, 5000.0f});
        pw_effect_update_policy policy;
        policy.update_radius = 100.0f;
        policy.max_updates_per_frame = 0;
        size_t cursor = 0;
        const auto plan = plan_pw_effect_updates(instances, {}, policy, cursor);
        check(plan.update.size() == 3, "no observers: all instances update");
        check(plan.freeze.empty(), "no observers: nothing frozen");
        check(plan.deferred == 0, "no observers: nothing deferred");
    }

    // Radius splits near and far against the closest observer.
    {
        const auto instances = make_instances({10.0f, 150.0f, 400.0f, 990.0f});
        pw_effect_update_policy policy;
        policy.update_radius = 200.0f;
        policy.max_updates_per_frame = 0;
        size_t cursor = 0;
        const std::vector<std::array<float, 3>> observers{{0.0f, 0.0f, 0.0f}, {1000.0f, 0.0f, 0.0f}};
        const auto plan = plan_pw_effect_updates(instances, observers, policy, cursor);
        check(plan.update.size() == 3, "radius: instances near either observer update");
        check(plan.freeze.size() == 1 && plan.freeze[0] == 2, "radius: only the far instance freezes");
        check(std::find(plan.update.begin(), plan.update.end(), 3) != plan.update.end(),
              "radius: second observer keeps its neighbour live");
    }

    // A non-positive radius disables culling even with observers present.
    {
        const auto instances = make_instances({0.0f, 100000.0f});
        pw_effect_update_policy policy;
        policy.update_radius = 0.0f;
        policy.max_updates_per_frame = 0;
        size_t cursor = 0;
        const auto plan = plan_pw_effect_updates(instances, {{0.0f, 0.0f, 0.0f}}, policy, cursor);
        check(plan.update.size() == 2 && plan.freeze.empty(), "zero radius disables culling");
    }

    // Budget rotates through the near set so every near instance updates within ceil(n / budget) frames.
    {
        const auto instances = make_instances({1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 900.0f});
        pw_effect_update_policy policy;
        policy.update_radius = 100.0f;
        policy.max_updates_per_frame = 2;
        size_t cursor = 0;
        std::set<size_t> seen;
        size_t total_deferred = 0;
        for(int frame = 0; frame < 3; ++frame)
        {
            const auto plan = plan_pw_effect_updates(instances, {{0.0f, 0.0f, 0.0f}}, policy, cursor);
            check(plan.update.size() == 2, "budget: exactly two updates per frame");
            check(plan.freeze.size() == 1 && plan.freeze[0] == 5, "budget: far instance frozen each frame");
            total_deferred += plan.deferred;
            seen.insert(plan.update.begin(), plan.update.end());
        }
        check(seen.size() == 5, "budget: all five near instances updated within three frames");
        check(total_deferred == 9, "budget: deferred count reports the postponed near instances");
    }

    // Empty input resets the cursor and yields an empty plan.
    {
        pw_effect_update_policy policy;
        size_t cursor = 7;
        const auto plan = plan_pw_effect_updates({}, {{0.0f, 0.0f, 0.0f}}, policy, cursor);
        check(plan.update.empty() && plan.freeze.empty() && plan.deferred == 0, "empty input: empty plan");
        check(cursor == 0, "empty input: cursor reset");
    }

    std::printf("pw_effect_update_plan: %d checks, %d failures\n", checks, failures);
    return failures;
}
} // namespace

REGISTER_TEST_SUITE("PW effect update plan", run_pw_effect_update_plan_suite)
