#include "../tests.h"
#include <engine/assets/asset_task_queue.h>

#include <cstdio>
#include <deque>
#include <stdexcept>
#include <vector>

using namespace unravel;
namespace
{
int checks = 0;
int failures = 0;
void check(bool condition, const char* message)
{
    ++checks;
    if(!condition) { ++failures; std::printf("  FAIL: %s\n", message); }
}

struct manual_scheduler
{
    std::deque<asset_task_queue::task> jobs;
    std::shared_ptr<asset_task_queue> queue = std::make_shared<asset_task_queue>(
        [this](const std::string&, int, asset_task_queue::task work) { jobs.push_back(std::move(work)); });
    void step()
    {
        auto task = std::move(jobs.front());
        jobs.pop_front();
        task();
    }
};

auto run_asset_task_queue(rtti::context&) -> int
{
    checks = failures = 0;
    manual_scheduler scheduler;
    auto project = scheduler.queue->create_scope();
    std::vector<int> ran;
    scheduler.queue->enqueue(project, "first", "first mesh", 1, [&] { ran.push_back(1); });
    for(int i = 0; i < 3500; ++i)
        scheduler.queue->enqueue(project, "mesh" + std::to_string(i), "bulk mesh", 1, [] {});
    check(scheduler.jobs.size() == 1, "3501 compile requests submit only one scheduler job, leaving workers available for asset loads");
    check(scheduler.queue->snapshot(project).active == 1 && scheduler.queue->snapshot(project).pending == 3500,
          "pending compilation remains visible in readiness progress");
    scheduler.queue->enqueue(project, "mesh0", "newest mesh contents", 1, [&] { ran.push_back(3); });
    check(scheduler.queue->snapshot(project).pending == 3500, "duplicate queued output coalesces instead of compiling stale revisions");
    auto bootstrap = scheduler.queue->create_scope();
    scheduler.queue->enqueue(bootstrap, "alternate", "alternate renderer shader", 0, [&] { ran.push_back(4); });
    scheduler.queue->enqueue(bootstrap, "native", "current renderer shader", 2, [&] { ran.push_back(2); });
    scheduler.step();
    check(scheduler.jobs.size() == 1, "finishing work schedules one successor");
    scheduler.step();
    check(ran == std::vector<int>({1, 2}), "current renderer shader is prioritized ahead of bulk meshes");
    check(!scheduler.queue->snapshot(bootstrap, 1).busy() && scheduler.queue->snapshot(bootstrap).busy(),
          "bootstrap waits normal work while alternate renderer shaders can continue later");
    scheduler.step();
    check(ran.back() == 3, "coalesced output compiles its latest source revision");
    scheduler.queue->cancel(project);
    check(scheduler.queue->snapshot(project).pending == 0 && scheduler.queue->snapshot(project).active == 1,
          "cancel drops bulk queue but keeps scheduled old generation tracked until drained");
    scheduler.step(); // Scheduled, cancelled mesh does not execute; alternate shader follows.
    check(!scheduler.queue->snapshot(project).busy(), "old project generation drains before protocol rebinding");
    scheduler.step();
    check(ran.back() == 4 && !scheduler.queue->snapshot().busy(), "cancelling project does not cancel another protocol's shaders");

    auto old_scope = scheduler.queue->create_scope();
    auto replacement = scheduler.queue->create_scope();
    bool old_running_observed = false;
    int old_side_effects = 0;
    int new_side_effects = 0;
    scheduler.queue->enqueue(old_scope, "same-path", "active old compile", 1, [&]
    {
        ++old_side_effects;
        scheduler.queue->cancel(old_scope);
        old_running_observed = scheduler.queue->snapshot(old_scope).busy();
        scheduler.queue->enqueue(replacement, "same-path", "replacement compile", 1, [&] { ++new_side_effects; });
        check(scheduler.jobs.empty(), "replacement cannot start during in-flight old-protocol side effects");
    });
    scheduler.queue->enqueue(old_scope, "never", "cancelled stale compile", 1, [&] { old_side_effects += 100; });
    scheduler.step();
    check(old_running_observed && old_side_effects == 1 && !scheduler.queue->snapshot(old_scope).busy(),
          "running cancellation preserves old-scope lifetime until its real work returns");
    scheduler.step();
    check(new_side_effects == 1, "same output path in fresh generation executes independently");
    scheduler.queue->enqueue(old_scope, "late callback", "late callback", 1, [&] { old_side_effects += 100; });
    check(scheduler.jobs.empty(), "late file callback from retired generation cannot enqueue work");

    scheduler.queue->enqueue(replacement, "bad", "throwing importer", 1, [] { throw std::runtime_error("synthetic import failure"); });
    scheduler.queue->enqueue(replacement, "good", "next valid importer", 1, [&] { ++new_side_effects; });
    scheduler.step(); scheduler.step();
    check(new_side_effects == 2 && scheduler.queue->snapshot(replacement).failed == 1 && !scheduler.queue->snapshot().busy(),
          "import exception releases dispatcher and later valid assets still compile");
    bool reject_submission = false;
    std::deque<asset_task_queue::task> accepted;
    auto rejecting_queue = std::make_shared<asset_task_queue>(
        [&](const std::string&, int, asset_task_queue::task work)
        {
            if(reject_submission) throw std::runtime_error("scheduler stopped");
            accepted.push_back(std::move(work));
        });
    auto stopping_project = rejecting_queue->create_scope();
    rejecting_queue->enqueue(stopping_project, "active", "active", 1, [] {});
    for(int i = 0; i < 3500; ++i)
        rejecting_queue->enqueue(stopping_project, std::to_string(i), "queued", 1, [] {});
    reject_submission = true;
    auto last_accepted = std::move(accepted.front());
    accepted.pop_front();
    last_accepted();
    check(!rejecting_queue->snapshot().busy() && rejecting_queue->snapshot().failed == 3500,
          "scheduler rejection drains reservations iteratively so shutdown cannot wait forever");
    std::printf("Asset task queue: %d checks, %d failures\n", checks, failures);
    return failures;
}
} // namespace
REGISTER_TEST_SUITE("asset compilation / bounded queue / generation lifetime", run_asset_task_queue)
