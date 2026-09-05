#include "../tests.h"
#include <filesystem/watchers/watcher_fallback.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <thread>

namespace
{
using namespace std::chrono_literals;
int checks = 0;
int failures = 0;

void check(bool value, const char* message)
{
    ++checks;
    if(!value) { ++failures; std::printf("  FAIL: %s\n", message); }
}

auto run_filesystem_watcher_lifetime(rtti::context&) -> int
{
    checks = failures = 0;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("unravel-watcher-lifetime-" + std::to_string(unique));
    fs::create_directories(root);
    fs::watcher_fallback watcher;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false, unwatch_started = false, unwatch_returned = false;
    std::atomic<unsigned> calls{0};
    const auto id = watcher.watch_impl(root, fs::pattern_filter("*.txt"), true, false, 10ms,
        [&](const auto&, bool)
        {
            std::unique_lock lock(mutex);
            ++calls;
            entered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        }, "Lifetime barrier test");
    watcher.start();
    { std::ofstream file(root / "barrier.txt"); file << "first"; }
    {
        std::unique_lock lock(mutex);
        check(changed.wait_for(lock, 5s, [&] { return entered; }), "real filesystem event enters callback");
    }
    std::thread removal([&]
    {
        { std::lock_guard lock(mutex); unwatch_started = true; changed.notify_all(); }
        watcher.unwatch_impl(id);
        { std::lock_guard lock(mutex); unwatch_returned = true; changed.notify_all(); }
    });
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return unwatch_started; });
        if(entered)
            check(!changed.wait_for(lock, 100ms, [&] { return unwatch_returned; }),
                  "unwatch cannot return while an existing callback still uses its owner");
        release = true;
        changed.notify_all();
    }
    removal.join();
    check(unwatch_returned, "unwatch returns after the callback is released");

    // Keep the directory listener live through another subscription, then prove
    // that a removed slot cannot receive the next notification snapshot.
    std::atomic<unsigned> live_calls{0};
    const auto live = watcher.watch_impl(root, fs::pattern_filter("*.txt"), true, false, 10ms,
        [&](const auto&, bool)
        {
            std::lock_guard lock(mutex);
            ++live_calls;
            changed.notify_all();
        }, "Live sibling test");
    const auto before = calls.load();
    { std::ofstream file(root / "after.txt"); file << "second"; }
    {
        std::unique_lock lock(mutex);
        check(changed.wait_for(lock, 5s, [&] { return live_calls.load() != 0; }), "sibling watcher observes next real event");
    }
    check(calls.load() == before, "retired callback cannot run after unwatch returns");
    watcher.unwatch_impl(live);

    std::atomic<uint64_t> self_id{0};
    bool self_done = false;
    self_id = watcher.watch_impl(root, fs::pattern_filter("*.txt"), true, false, 10ms,
        [&](const auto&, bool)
        {
            watcher.unwatch_impl(self_id.load());
            std::lock_guard lock(mutex);
            self_done = true;
            changed.notify_all();
        }, "Self-unwatch test");
    { std::ofstream file(root / "self.txt"); file << "third"; }
    {
        std::unique_lock lock(mutex);
        const bool completed = changed.wait_for(lock, 5s, [&] { return self_done; });
        check(completed, "callback can unwatch itself without deadlock or premature destruction");
        if(!completed)
        {
            // A deadlocked watcher cannot be joined safely during unwinding.
            // Fail the test process within its deadline instead of hanging CI.
            std::fflush(stdout);
            std::_Exit(EXIT_FAILURE);
        }
    }
    watcher.close();
    fs::remove_all(root);
    std::printf("Filesystem watcher lifetime: %d checks, %d failures\n", checks, failures);
    return failures;
}
} // namespace

REGISTER_TEST_SUITE("filesystem watcher / unwatch barrier / self-unwatch", run_filesystem_watcher_lifetime)
