#include "pw_session_controller.h"

#include <editor/mcp/json.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace unravel
{
namespace
{
using json = nlohmann::json;
using steady_clock = std::chrono::steady_clock;

constexpr uint32_t kStatusPollMs = 250;   // starting/authenticating/role_list_loading
constexpr uint32_t kIdlePollMs = 500;     // character_select
constexpr uint32_t kEnterPollMs = 300;    // selecting_role/entering_world
constexpr uint32_t kInWorldPollMs = 1000; // in_world
constexpr uint32_t kBridgeCallTimeoutMs = 5000;
constexpr int kMaxConsecutiveFailures = 3;

void secure_zero(std::string& value)
{
    if(value.empty())
    {
        return;
    }
#if defined(_WIN32)
    SecureZeroMemory(value.data(), value.size());
#else
    volatile char* p = value.data();
    for(size_t i = 0; i < value.size(); ++i)
    {
        p[i] = 0;
    }
#endif
    value.clear();
}

void secure_zero(std::wstring& value)
{
    if(value.empty())
    {
        return;
    }
#if defined(_WIN32)
    SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
#else
    volatile wchar_t* p = value.data();
    for(size_t i = 0; i < value.size(); ++i)
    {
        p[i] = 0;
    }
#endif
    value.clear();
}

auto camera_for(pw_session_state state, int32_t selected_role_id) -> pw_session_camera
{
    switch(state)
    {
    case pw_session_state::idle:
    case pw_session_state::starting:
    case pw_session_state::authenticating:
    case pw_session_state::role_list_loading:
    case pw_session_state::error:
        return pw_session_camera::login;
    case pw_session_state::character_select:
        return selected_role_id != 0 ? pw_session_camera::choose : pw_session_camera::selchar;
    case pw_session_state::selecting_role:
    case pw_session_state::entering_world:
        return pw_session_camera::choose;
    case pw_session_state::in_world:
        // The world view (WP7) owns the camera while in the world: it follows
        // the replicated self marker instead of a login-scene preset.
        return pw_session_camera::none;
    }
    return pw_session_camera::login;
}

// Extracts result.structuredContent from a bridge JSON-RPC response.
auto parse_structured_content(const std::string& response_json, json& out) -> bool
{
    const json response = json::parse(response_json, nullptr, false);
    if(response.is_discarded() || !response.is_object())
    {
        return false;
    }
    const auto result = response.find("result");
    if(result == response.end() || !result->is_object())
    {
        return false;
    }
    const auto structured = result->find("structuredContent");
    if(structured == result->end() || !structured->is_object())
    {
        return false;
    }
    out = *structured;
    return true;
}

struct login_status
{
    bool valid = false;
    std::string phase;
    bool role_list_ready = false;
    uint32_t role_list_revision = 0;
    std::string last_error;
};

auto parse_login_status(const json& structured, login_status& out) -> bool
{
    const auto state = structured.find("state");
    if(state == structured.end() || !state->is_object())
    {
        return false;
    }
    const auto login = state->find("login");
    if(login == state->end() || !login->is_object())
    {
        return false;
    }
    out.valid = true;
    out.phase = login->value("phase", std::string());
    out.role_list_ready = login->value("roleListReady", false);
    out.role_list_revision = login->value("roleListRevision", 0u);
    out.last_error = login->value("lastError", std::string());
    return true;
}

struct session_command
{
    enum class kind
    {
        connect,
        disconnect,
        refresh_roles,
        fetch_preview,
        enter,
        cancel,
        world_action,
    };
    kind command_kind = kind::disconnect;
    pw_session_connect_params params;
    int32_t role_id = 0; // fetch_preview
    // world_action (WP7): action name plus optional move/select payload
    std::string action;
    float action_x = 0.0f;
    float action_y = 0.0f;
    float action_z = 0.0f;
    int32_t action_id = 0;
};
} // namespace

void pw_session_connect_params::clear_secrets()
{
    secure_zero(account_utf8);
    secure_zero(password_utf8);
}

auto pw_session_state_name(pw_session_state state) -> const char*
{
    switch(state)
    {
    case pw_session_state::idle: return "idle";
    case pw_session_state::starting: return "starting";
    case pw_session_state::authenticating: return "authenticating";
    case pw_session_state::role_list_loading: return "role_list_loading";
    case pw_session_state::character_select: return "character_select";
    case pw_session_state::selecting_role: return "selecting_role";
    case pw_session_state::entering_world: return "entering_world";
    case pw_session_state::in_world: return "in_world";
    case pw_session_state::error: return "error";
    }
    return "unknown";
}

auto pw_session_camera_name(pw_session_camera camera) -> const char*
{
    switch(camera)
    {
    case pw_session_camera::none: return "none";
    case pw_session_camera::login: return "login";
    case pw_session_camera::selchar: return "selchar";
    case pw_session_camera::choose: return "choose";
    }
    return "unknown";
}

struct pw_session_controller::implementation
{
    explicit implementation(pw_runtime_client& runtime_ref)
        : runtime(runtime_ref)
    {
    }

    pw_runtime_client& runtime;

    std::mutex mutex; // guards snapshot, commands, worker flags
    std::condition_variable cv;
    std::deque<session_command> commands;
    pw_session_snapshot snapshot;
    bool worker_running = false;
    bool stop_requested = false;
    std::thread worker;

    // Worker-owned state (only touched on the worker thread, or written under
    // mutex before the command that consumes them is posted).
    steady_clock::time_point connect_deadline{};
    steady_clock::time_point enter_deadline{};
    uint32_t connect_deadline_ms = 60000;
    uint32_t enter_deadline_ms = 60000;
    uint32_t next_operation_id = 1;
    uint32_t active_operation_id = 0;
    uint32_t active_revision = 0;
    int32_t active_role_id = 0;
    int32_t active_worldtag = 0;
    int consecutive_failures = 0;
    uint32_t status_revision = 0;  // last authoritative login.roleListRevision
    uint32_t fetched_revision = 0; // status revision the stored list content belongs to
    // Monotonic preview publish counter — never resets, so a cleared preview
    // (serial 0) followed by a fresh fetch always yields a new serial and
    // consumers comparing serials can never mistake it for the previous one.
    uint32_t preview_publish_counter = 0;
    // World stream: epoch bumps on every in_world entry (reconnect/resync),
    // the publish counter on every published world update. Neither resets.
    uint32_t world_epoch = 0;
    uint64_t world_publish_counter = 0;

    // --- snapshot helpers (call with mutex held) ---

    void refresh_camera_locked()
    {
        snapshot.desired_camera = camera_for(snapshot.state, snapshot.selected_role_id);
    }

    void set_state_locked(pw_session_state state)
    {
        snapshot.state = state;
        refresh_camera_locked();
    }

    void set_error_locked(const std::string& code, const std::string& message)
    {
        snapshot.state = pw_session_state::error;
        snapshot.error_code = code;
        snapshot.error_message = message;
        snapshot.mutation_capable = false;
        refresh_camera_locked();
    }

    void clear_session_locked()
    {
        snapshot.roles.clear();
        snapshot.role_list_revision = 0;
        snapshot.selected_role_id = 0;
        snapshot.mutation_capable = false;
        snapshot.child_pid = 0;
        snapshot.attested_role_id = 0;
        snapshot.attested_instance_id = 0;
        snapshot.preview = pw_session_preview{};
        snapshot.world = pw_session_world{};
        snapshot.error_code.clear();
        snapshot.error_message.clear();
        active_operation_id = 0;
        active_revision = 0;
        active_role_id = 0;
        active_worldtag = 0;
        consecutive_failures = 0;
        status_revision = 0;
        fetched_revision = 0;
    }

    // --- bridge failure accounting (worker thread) ---

    void note_bridge_failure(const std::string& message)
    {
        ++consecutive_failures;
        if(consecutive_failures >= kMaxConsecutiveFailures)
        {
            std::lock_guard<std::mutex> lock(mutex);
            set_error_locked("connection_lost", message);
        }
    }

    void note_bridge_success()
    {
        consecutive_failures = 0;
    }

    // --- status poll (worker thread) ---

    void poll_status()
    {
        const pw_runtime_operation operation = runtime.call_tool("pw.status", "{}", kBridgeCallTimeoutMs);
        if(!operation.ok)
        {
            note_bridge_failure(operation.error);
            return;
        }
        json structured;
        if(!parse_structured_content(operation.response_json, structured))
        {
            note_bridge_failure("pw.status returned a malformed response");
            return;
        }
        login_status status;
        if(!parse_login_status(structured, status) || !status.valid)
        {
            note_bridge_failure("pw.status has no login object");
            return;
        }
        note_bridge_success();
        status_revision = status.role_list_revision;
        const auto state_it = structured.find("state");
        const json* state_ptr =
            state_it != structured.end() && state_it->is_object() ? &(*state_it) : nullptr;

        if(!status.last_error.empty())
        {
            std::lock_guard<std::mutex> lock(mutex);
            set_error_locked(status.last_error, "login failed with terminal code " + status.last_error);
            return;
        }

        std::unique_lock<std::mutex> lock(mutex);
        const pw_session_state state = snapshot.state;
        if(state == pw_session_state::in_world)
        {
            if(status.phase != "in_world")
            {
                set_error_locked("connection_lost", "session left the world unexpectedly");
                return;
            }
            // Live capability revalidation (locks the client, not this mutex).
            lock.unlock();
            const bool capable = runtime.has_mutation_capability();
            lock.lock();
            snapshot.mutation_capable = capable;
            if(state_ptr != nullptr)
            {
                publish_world_self_locked(*state_ptr);
            }
            lock.unlock();
            fetch_world_entities();
            return;
        }
        if(state == pw_session_state::selecting_role || state == pw_session_state::entering_world)
        {
            // The enter poll is authoritative while an operation is in flight.
            return;
        }

        if(status.phase == "offline")
        {
            if(state == pw_session_state::starting)
            {
                return; // link not up yet; the connect deadline bounds this state
            }
            set_error_locked("connection_lost", "login link dropped");
            return;
        }
        if(status.phase == "authenticating")
        {
            if(state == pw_session_state::starting)
            {
                set_state_locked(pw_session_state::authenticating);
            }
            return;
        }
        if(status.phase == "character_select")
        {
            if(!status.role_list_ready)
            {
                if(state != pw_session_state::role_list_loading)
                {
                    set_state_locked(pw_session_state::role_list_loading);
                }
                return;
            }
            if(status.role_list_revision != fetched_revision)
            {
                // Revision moved (or first list): drop any stale selection and
                // adopt the fresh authoritative list.
                snapshot.selected_role_id = 0;
                snapshot.preview = pw_session_preview{};
                refresh_camera_locked();
                lock.unlock();
                const bool fetched = fetch_role_list();
                lock.lock();
                if(!fetched)
                {
                    return; // failure handling already applied
                }
            }
            if(snapshot.state != pw_session_state::character_select)
            {
                set_state_locked(pw_session_state::character_select);
            }
            return;
        }
        if(status.phase == "entering_world" || status.phase == "in_world")
        {
            // Reaching the world without an attested local operation must never
            // be presented as success.
            set_error_locked("unexpected_state",
                             "runtime entered the world without an attested pw.role_enter operation");
            return;
        }
    }

    // --- role list fetch (worker thread) ---

    // --- world stream (WP7; worker thread) ---

    // Self state rides the authoritative in_world pw.status poll.
    void publish_world_self_locked(const json& state)
    {
        pw_world_self self;
        self.role_id = state.value("roleId", 0);
        self.instance_id = state.value("instanceId", 0);
        const auto position = state.find("position");
        if(position != state.end() && position->is_object())
        {
            self.x = position->value("x", 0.0f);
            self.y = position->value("y", 0.0f);
            self.z = position->value("z", 0.0f);
        }
        const auto direction = state.find("direction");
        if(direction != state.end() && direction->is_object())
        {
            self.dir_x = direction->value("x", 0.0f);
            self.dir_y = direction->value("y", 0.0f);
            self.dir_z = direction->value("z", 1.0f);
        }
        self.hp = state.value("hp", 0);
        self.max_hp = state.value("maxHp", 0);
        self.mp = state.value("mp", 0);
        self.max_mp = state.value("maxMp", 0);
        self.level = state.value("level", 0);
        self.dead = state.value("dead", false);
        self.moving = state.value("moving", false);
        self.target_id = state.value("targetId", 0);
        snapshot.world.self = self;
        snapshot.world.seq = ++world_publish_counter;
    }

    // Read-only nearby fetch. A failure keeps the last published entity set and
    // only records world.error — the world stream can never break the session.
    void fetch_world_entities()
    {
        const pw_runtime_operation operation =
            runtime.call_tool("pw.nearby", "{\"range\":60}", kBridgeCallTimeoutMs);
        json structured;
        std::vector<pw_world_entity> entities;
        std::string error;
        if(!operation.ok || !parse_structured_content(operation.response_json, structured))
        {
            error = operation.ok ? "malformed pw.nearby response" : operation.error;
        }
        else if(!structured.value("ok", false))
        {
            error = structured.value("errorCode", std::string("nearby_failed"));
        }
        else
        {
            const auto payload = structured.find("payload");
            if(payload == structured.end() || !payload->is_object())
            {
                error = "pw.nearby has no payload";
            }
            else
            {
                const auto items = payload->find("entities");
                if(items == payload->end() || !items->is_array())
                {
                    error = "pw.nearby has no entities array";
                }
                else
                {
                    for(const json& item : *items)
                    {
                        pw_world_entity entity;
                        entity.id = item.value("id", 0);
                        entity.kind = item.value("type", std::string());
                        entity.template_id = item.value("templateId", 0u);
                        entity.name = item.value("name", std::string());
                        entity.x = item.value("x", 0.0f);
                        entity.y = item.value("y", 0.0f);
                        entity.z = item.value("z", 0.0f);
                        entity.dist = item.value("dist", 0.0f);
                        entity.dead = item.value("dead", false);
                        entity.gatherable = item.value("gatherable", false);
                        entities.push_back(std::move(entity));
                    }
                }
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        if(snapshot.state != pw_session_state::in_world)
        {
            return; // left the world mid-fetch; the clear path owns the snapshot
        }
        if(!error.empty())
        {
            snapshot.world.error = error;
            return; // keep the last good entity set; seq stays put
        }
        snapshot.world.entities = std::move(entities);
        snapshot.world.error.clear();
        snapshot.world.seq = ++world_publish_counter;
    }

    // Read-only preview fetch for one role of the current revision. Failures
    // only surface as preview.error in the snapshot — never as a session error,
    // so a preview problem can never block login/enter.
    void fetch_preview(int32_t role_id)
    {
        json args = {{"role_id", role_id}};
        {
            std::lock_guard<std::mutex> lock(mutex);
            args["role_list_revision"] = snapshot.role_list_revision;
        }
        const pw_runtime_operation operation =
            runtime.call_tool("pw.role_preview", args.dump(), kBridgeCallTimeoutMs);
        pw_session_preview preview;
        preview.role_id = role_id;
        json structured;
        if(!operation.ok || !parse_structured_content(operation.response_json, structured))
        {
            preview.error = operation.ok ? "malformed pw.role_preview response" : operation.error;
        }
        else if(!structured.value("ok", false))
        {
            preview.error = structured.value("errorCode", std::string("role_preview_failed"));
        }
        else
        {
            const auto payload = structured.find("payload");
            if(payload == structured.end() || !payload->is_object())
            {
                preview.error = "pw.role_preview has no payload";
            }
            else
            {
                preview.profession = payload->value("profession", 0);
                preview.gender = payload->value("gender", 0);
                preview.race = payload->value("race", 0);
                const auto custom = payload->find("custom");
                if(custom != payload->end() && custom->is_object())
                {
                    preview.custom_present = custom->value("present", false);
                    preview.color_body = custom->value("colorBody", 0u);
                    preview.color_hair = custom->value("colorHair", 0u);
                }
                const auto equipment = payload->find("equipment");
                if(equipment != payload->end() && equipment->is_array())
                {
                    for(const json& entry : *equipment)
                    {
                        preview.equipment.emplace_back(entry.value("slot", 0),
                                                       entry.value("itemId", 0u));
                    }
                }
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        // A selection/revision change during the fetch invalidates the result.
        if(snapshot.selected_role_id == role_id)
        {
            preview.serial = ++preview_publish_counter;
            snapshot.preview = std::move(preview);
        }
    }

    // --- role list fetch (worker thread) ---

    auto fetch_role_list() -> bool
    {
        const pw_runtime_operation operation = runtime.call_tool("pw.role_list", "{}", kBridgeCallTimeoutMs);
        if(!operation.ok)
        {
            note_bridge_failure(operation.error);
            return false;
        }
        json structured;
        if(!parse_structured_content(operation.response_json, structured))
        {
            note_bridge_failure("pw.role_list returned a malformed response");
            return false;
        }
        if(!structured.value("ok", false))
        {
            const std::string code = structured.value("errorCode", std::string("role_list_failed"));
            if(code == "role_list_not_ready")
            {
                // Retryable: stay in role_list_loading; the next status poll retries.
                return false;
            }
            std::lock_guard<std::mutex> lock(mutex);
            set_error_locked(code, structured.value("message", std::string("pw.role_list failed")));
            return false;
        }
        note_bridge_success();
        const auto payload = structured.find("payload");
        if(payload == structured.end() || !payload->is_object())
        {
            note_bridge_failure("pw.role_list has no payload");
            return false;
        }
        std::vector<pw_session_role> roles;
        const auto roles_json = payload->find("roles");
        if(roles_json != payload->end() && roles_json->is_array())
        {
            for(const json& entry : *roles_json)
            {
                pw_session_role role;
                role.role_id = entry.value("role_id", 0);
                role.name = entry.value("name", std::string());
                role.profession = entry.value("profession", 0);
                role.gender = entry.value("gender", 0);
                role.race = entry.value("race", 0);
                role.level = entry.value("level", 0);
                role.level2 = entry.value("level2", 0);
                role.status = entry.value("status", 0);
                role.deleting = entry.value("deleting", false);
                role.worldtag = entry.value("worldtag", 0);
                roles.push_back(std::move(role));
            }
        }
        const uint32_t payload_revision = payload->value("revision", 0u);
        std::lock_guard<std::mutex> lock(mutex);
        // The status revision is authoritative for gating; the payload content is
        // adopted with it. A payload revision above the status revision means the
        // status poll raced a fresh list, so take the newer of the two.
        snapshot.role_list_revision = status_revision >= payload_revision ? status_revision : payload_revision;
        fetched_revision = status_revision;
        snapshot.roles = std::move(roles);
        return true;
    }

    // --- world actions (WP7; worker thread) ---

    // Exact-role-gated world mutation. The outcome surfaces only in
    // snapshot.world.error (cleared on success) — never a session error.
    void execute_world_action(const session_command& command)
    {
        std::string tool;
        json args = json::object();
        if(command.action == "move_to")
        {
            tool = "pw.move_to";
            args = {{"x", command.action_x}, {"y", command.action_y}, {"z", command.action_z}};
        }
        else if(command.action == "stop_move")
        {
            tool = "pw.stop_move";
        }
        else if(command.action == "jump")
        {
            tool = "pw.jump";
        }
        else if(command.action == "select_target")
        {
            tool = "pw.select_target";
            args = {{"id", command.action_id}};
        }
        else if(command.action == "normal_attack")
        {
            tool = "pw.normal_attack";
        }
        else
        {
            return; // rejected at intent time already; never reach here
        }
        const pw_runtime_operation operation = runtime.call_mutation(tool, args.dump(), kBridgeCallTimeoutMs);
        std::string error;
        json structured;
        if(!operation.ok || !parse_structured_content(operation.response_json, structured))
        {
            error = operation.ok ? "malformed world action response" : operation.error;
        }
        else if(!structured.value("ok", false))
        {
            error = structured.value("errorCode", std::string("world_action_failed"));
        }
        std::lock_guard<std::mutex> lock(mutex);
        if(snapshot.state != pw_session_state::in_world)
        {
            return; // left the world mid-action; the clear path owns the snapshot
        }
        snapshot.world.error = error;
    }

    // --- enter operation (worker thread) ---

    void poll_enter()
    {
        const pw_role_enter_result result = runtime.enter_role(active_role_id,
                                                               active_revision,
                                                               active_worldtag,
                                                               active_operation_id,
                                                               kBridgeCallTimeoutMs);
        if(!result.ok)
        {
            note_bridge_failure(result.error);
            return;
        }
        if(result.pending)
        {
            note_bridge_success();
            std::lock_guard<std::mutex> lock(mutex);
            if(snapshot.state == pw_session_state::selecting_role)
            {
                set_state_locked(pw_session_state::entering_world);
            }
            return;
        }
        if(!result.error_code.empty())
        {
            std::lock_guard<std::mutex> lock(mutex);
            set_error_locked(result.error_code, result.error);
            return;
        }
        if(result.entered)
        {
            note_bridge_success();
            // Publish in_world together with its attestation and capability in a
            // single locked section: observers must never see in_world without
            // the proof fields already set.
            const uint32_t pid = runtime.get_status().process_id;
            const bool capable = runtime.has_mutation_capability();
            std::lock_guard<std::mutex> lock(mutex);
            snapshot.attested_role_id = result.attested_role_id;
            snapshot.attested_instance_id = result.attested_instance_id;
            snapshot.child_pid = pid;
            snapshot.mutation_capable = capable;
            // Fresh world stream epoch: consumers drop every replicated entity
            // of the previous epoch and wait for a full re-publish.
            snapshot.world = pw_session_world{};
            snapshot.world.epoch = ++world_epoch;
            set_state_locked(pw_session_state::in_world);
            return;
        }
        note_bridge_failure("pw.role_enter returned an unexpected result");
    }

    // --- command execution (worker thread, called with mutex released) ---

    void execute_command(const session_command& command)
    {
        switch(command.command_kind)
        {
        case session_command::kind::connect:
            execute_connect(command.params);
            break;
        case session_command::kind::disconnect:
        case session_command::kind::cancel:
            execute_stop_to_idle();
            break;
        case session_command::kind::refresh_roles:
            fetch_role_list();
            break;
        case session_command::kind::fetch_preview:
            fetch_preview(command.role_id);
            break;
        case session_command::kind::world_action:
            execute_world_action(command);
            break;
        case session_command::kind::enter:
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                const pw_session_role* selected = nullptr;
                for(const pw_session_role& role : snapshot.roles)
                {
                    if(role.role_id == snapshot.selected_role_id)
                    {
                        selected = &role;
                        break;
                    }
                }
                if(selected == nullptr)
                {
                    set_error_locked("role_not_found", "selected role is no longer in the role list");
                    return;
                }
                active_role_id = selected->role_id;
                active_worldtag = selected->worldtag;
                active_revision = snapshot.role_list_revision;
                active_operation_id = next_operation_id++;
                enter_deadline = steady_clock::now() + std::chrono::milliseconds(enter_deadline_ms);
                set_state_locked(pw_session_state::selecting_role);
            }
            poll_enter();
            break;
        }
        }
    }

    void execute_connect(pw_session_connect_params params)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            clear_session_locked();
            set_state_locked(pw_session_state::starting);
        }
        // A previous sidecar session (error path) must not survive into the new one.
        runtime.stop();
        pw_runtime_operation operation = runtime.start_role_admin(params.executable_path,
                                                                  params.working_directory,
                                                                  params.server,
                                                                  params.account_utf8,
                                                                  params.password_utf8);
        params.clear_secrets();
        if(!operation.ok)
        {
            std::lock_guard<std::mutex> lock(mutex);
            set_error_locked("start_failed", operation.error);
            return;
        }
        connect_deadline = steady_clock::now() + std::chrono::milliseconds(connect_deadline_ms);
        const pw_runtime_status status = runtime.get_status();
        std::lock_guard<std::mutex> lock(mutex);
        snapshot.child_pid = status.process_id;
    }

    void execute_stop_to_idle()
    {
        runtime.stop();
        std::lock_guard<std::mutex> lock(mutex);
        clear_session_locked();
        set_state_locked(pw_session_state::idle);
    }

    // --- worker loop ---

    auto poll_interval_for(pw_session_state state) -> uint32_t
    {
        switch(state)
        {
        case pw_session_state::starting:
        case pw_session_state::authenticating:
        case pw_session_state::role_list_loading:
            return kStatusPollMs;
        case pw_session_state::character_select:
            return kIdlePollMs;
        case pw_session_state::selecting_role:
        case pw_session_state::entering_world:
            return kEnterPollMs;
        case pw_session_state::in_world:
            return kInWorldPollMs;
        case pw_session_state::idle:
        case pw_session_state::error:
            return 0;
        }
        return 0;
    }

    void check_deadlines()
    {
        const auto now = steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex);
        switch(snapshot.state)
        {
        case pw_session_state::starting:
        case pw_session_state::authenticating:
        case pw_session_state::role_list_loading:
            if(now >= connect_deadline)
            {
                set_error_locked("timeout", "login did not reach character select in time");
            }
            break;
        case pw_session_state::selecting_role:
        case pw_session_state::entering_world:
            if(now >= enter_deadline)
            {
                set_error_locked("enter_timeout", "role enter did not complete in time");
            }
            break;
        default:
            break;
        }
    }

    void worker_main()
    {
        std::unique_lock<std::mutex> lock(mutex);
        while(!stop_requested)
        {
            if(!commands.empty())
            {
                session_command command = std::move(commands.front());
                commands.pop_front();
                lock.unlock();
                execute_command(command);
                lock.lock();
                continue;
            }
            const uint32_t poll_ms = poll_interval_for(snapshot.state);
            if(poll_ms == 0)
            {
                cv.wait(lock, [&] { return stop_requested || !commands.empty(); });
                continue;
            }
            cv.wait_for(lock, std::chrono::milliseconds(poll_ms), [&] { return stop_requested || !commands.empty(); });
            if(stop_requested || !commands.empty())
            {
                continue;
            }
            const pw_session_state state = snapshot.state;
            lock.unlock();
            check_deadlines();
            if(state == pw_session_state::selecting_role || state == pw_session_state::entering_world)
            {
                poll_enter();
            }
            else
            {
                poll_status();
            }
            lock.lock();
        }
    }
};

pw_session_controller::pw_session_controller(pw_runtime_client& runtime)
    : implementation_(std::make_unique<implementation>(runtime))
{
}

pw_session_controller::~pw_session_controller()
{
    shutdown();
}

void pw_session_controller::start()
{
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    if(implementation_->worker_running)
    {
        return;
    }
    implementation_->stop_requested = false;
    implementation_->worker = std::thread([impl = implementation_.get()] { impl->worker_main(); });
    implementation_->worker_running = true;
}

void pw_session_controller::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(implementation_->mutex);
        if(!implementation_->worker_running)
        {
            return;
        }
        implementation_->stop_requested = true;
    }
    implementation_->cv.notify_all();
    if(implementation_->worker.joinable())
    {
        implementation_->worker.join();
    }
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    implementation_->worker_running = false;
}

auto pw_session_controller::connect(pw_session_connect_params params) -> bool
{
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    if(!implementation_->worker_running)
    {
        return false;
    }
    if(implementation_->snapshot.state != pw_session_state::idle &&
       implementation_->snapshot.state != pw_session_state::error)
    {
        return false;
    }
    implementation_->connect_deadline_ms = params.connect_deadline_ms;
    implementation_->enter_deadline_ms = params.enter_deadline_ms;
    session_command command;
    command.command_kind = session_command::kind::connect;
    command.params = std::move(params);
    implementation_->commands.push_back(std::move(command));
    implementation_->cv.notify_all();
    return true;
}

auto pw_session_controller::disconnect() -> bool
{
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    if(!implementation_->worker_running || implementation_->snapshot.state == pw_session_state::idle)
    {
        return false;
    }
    session_command command;
    command.command_kind = session_command::kind::disconnect;
    implementation_->commands.push_back(std::move(command));
    implementation_->cv.notify_all();
    return true;
}

auto pw_session_controller::refresh_roles() -> bool
{
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    if(!implementation_->worker_running || implementation_->snapshot.state != pw_session_state::character_select)
    {
        return false;
    }
    session_command command;
    command.command_kind = session_command::kind::refresh_roles;
    implementation_->commands.push_back(std::move(command));
    implementation_->cv.notify_all();
    return true;
}

auto pw_session_controller::world_action(const std::string& action, float x, float y, float z, int32_t id) -> bool
{
    static const char* allowed[] = {"move_to", "stop_move", "jump", "select_target", "normal_attack"};
    bool known = false;
    for(const char* name : allowed)
    {
        if(action == name)
        {
            known = true;
            break;
        }
    }
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    if(!known || !implementation_->worker_running ||
       implementation_->snapshot.state != pw_session_state::in_world)
    {
        return false;
    }
    session_command command;
    command.command_kind = session_command::kind::world_action;
    command.action = action;
    command.action_x = x;
    command.action_y = y;
    command.action_z = z;
    command.action_id = id;
    implementation_->commands.push_back(std::move(command));
    implementation_->cv.notify_all();
    return true;
}

auto pw_session_controller::select_role(int32_t role_id) -> bool
{
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    if(!implementation_->worker_running || implementation_->snapshot.state != pw_session_state::character_select)
    {
        return false;
    }
    // Selection is a pure snapshot mutation (no bridge call), so apply it
    // synchronously: back-to-back select+enter intents must observe it.
    for(const pw_session_role& role : implementation_->snapshot.roles)
    {
        if(role.role_id == role_id && !role.deleting)
        {
            implementation_->snapshot.selected_role_id = role_id;
            implementation_->snapshot.preview = pw_session_preview{};
            implementation_->refresh_camera_locked();
            session_command command;
            command.command_kind = session_command::kind::fetch_preview;
            command.role_id = role_id;
            implementation_->commands.push_back(std::move(command));
            implementation_->cv.notify_all();
            return true;
        }
    }
    return false;
}

auto pw_session_controller::enter_selected_role() -> bool
{
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    if(!implementation_->worker_running || implementation_->snapshot.state != pw_session_state::character_select ||
       implementation_->snapshot.selected_role_id == 0)
    {
        return false;
    }
    session_command command;
    command.command_kind = session_command::kind::enter;
    implementation_->commands.push_back(std::move(command));
    implementation_->cv.notify_all();
    return true;
}

auto pw_session_controller::cancel() -> bool
{
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    if(!implementation_->worker_running)
    {
        return false;
    }
    switch(implementation_->snapshot.state)
    {
    case pw_session_state::starting:
    case pw_session_state::authenticating:
    case pw_session_state::role_list_loading:
    case pw_session_state::selecting_role:
    case pw_session_state::entering_world:
        break;
    default:
        return false;
    }
    session_command command;
    command.command_kind = session_command::kind::cancel;
    implementation_->commands.push_back(std::move(command));
    implementation_->cv.notify_all();
    return true;
}

auto pw_session_controller::get_snapshot() const -> pw_session_snapshot
{
    std::lock_guard<std::mutex> lock(implementation_->mutex);
    return implementation_->snapshot;
}
} // namespace unravel
