#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace unravel
{
/** Serial compilation dispatcher. Waiting work never occupies a scheduler worker. */
class asset_task_queue : public std::enable_shared_from_this<asset_task_queue>
{
public:
    struct scope
    {
        uint64_t generation = 0;
        std::atomic<bool> cancelled{false};
    };
    using scope_ptr = std::shared_ptr<scope>;
    using task = std::function<void()>;
    using submit_fn = std::function<void(const std::string&, int, task)>;
    struct progress
    {
        size_t pending = 0;
        size_t active = 0;
        size_t completed = 0;
        size_t failed = 0;
        std::string name;
        auto busy() const -> bool { return pending != 0 || active != 0; }
    };

    explicit asset_task_queue(submit_fn submit) : submit_(std::move(submit)) {}

    auto create_scope() -> scope_ptr
    {
        auto result = std::make_shared<scope>();
        std::lock_guard lock(mutex_);
        result->generation = ++generation_;
        counters_.emplace(result->generation, progress{});
        return result;
    }

    void enqueue(const scope_ptr& owner, std::string key, std::string name, int priority, task work)
    {
        if(!owner || !work) return;
        {
            std::lock_guard lock(mutex_);
            if(owner->cancelled) return;
            const auto identity = std::make_pair(owner->generation, key);
            const auto existing = pending_by_key_.find(identity);
            if(existing != pending_by_key_.end())
            {
                existing->second->work = std::move(work);
                existing->second->name = std::move(name);
                existing->second->priority = priority;
            }
            else
            {
                pending_.push_back({owner, std::move(key), std::move(name), priority, std::move(work)});
                pending_by_key_.emplace(identity, std::prev(pending_.end()));
            }
        }
        dispatch();
    }

    /** Drop pending work. The one in-flight task must finish before protocol rebinding. */
    void cancel(const scope_ptr& owner)
    {
        if(!owner) return;
        std::lock_guard lock(mutex_);
        owner->cancelled = true;
        for(auto it = pending_.begin(); it != pending_.end();)
        {
            if(it->owner == owner)
            {
                pending_by_key_.erase({owner->generation, it->key});
                it = pending_.erase(it);
            }
            else ++it;
        }
    }

    /** A null scope reports all work; priority 1 waits normal/bootstrap work but not low-priority alternate shaders. */
    auto snapshot(const scope_ptr& owner = {}, int minimum_priority = 0) const -> progress
    {
        std::lock_guard lock(mutex_);
        progress result;
        for(const auto& [generation, counters] : counters_)
            if(!owner || owner->generation == generation)
            { result.completed += counters.completed; result.failed += counters.failed; }
        const auto matches = [&](const job& value)
        { return (!owner || value.owner == owner) && value.priority >= minimum_priority; };
        for(const auto& value : pending_) if(matches(value)) ++result.pending;
        if(active_ && matches(*active_)) { result.active = 1; result.name = active_->name; }
        return result;
    }

private:
    struct job
    {
        scope_ptr owner;
        std::string key;
        std::string name;
        int priority = 1;
        task work;
    };

    void dispatch()
    {
        // A stopped/rejecting scheduler must not leave an active reservation
        // behind. Drain rejected submissions iteratively, without recursion.
        for(;;)
        {
            job next;
            {
                std::lock_guard lock(mutex_);
                if(active_ || pending_.empty()) return;
                // Highest priority first, preserving FIFO within each priority.
                const auto selected = std::max_element(pending_.begin(), pending_.end(),
                    [](const job& a, const job& b) { return a.priority < b.priority; });
                next = std::move(*selected);
                pending_by_key_.erase({next.owner->generation, next.key});
                pending_.erase(selected);
                active_ = next;
                active_->work = {};
            }
            auto self = shared_from_this();
            const auto name = next.name;
            const auto priority = next.priority;
            const auto generation = next.owner->generation;
            try
            {
                submit_(name, priority, [self, next = std::move(next)]() mutable
                {
                    bool failed = false;
                    try { if(!next.owner->cancelled) next.work(); }
                    catch(...) { failed = true; }
                    {
                        std::lock_guard lock(self->mutex_);
                        auto& counters = self->counters_[next.owner->generation];
                        ++counters.completed;
                        if(failed) ++counters.failed;
                        self->active_.reset();
                    }
                    self->dispatch();
                });
                return;
            }
            catch(...)
            {
                std::lock_guard lock(mutex_);
                auto& counters = counters_[generation];
                ++counters.completed;
                ++counters.failed;
                active_.reset();
            }
        }
    }

    submit_fn submit_;
    mutable std::mutex mutex_;
    uint64_t generation_ = 0;
    std::list<job> pending_;
    std::map<std::pair<uint64_t, std::string>, std::list<job>::iterator> pending_by_key_;
    std::optional<job> active_;
    std::map<uint64_t, progress> counters_;
};
} // namespace unravel
