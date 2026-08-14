#pragma once

#include "pw_runtime_client.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace unravel
{
/**
 * Interactive PW login session states (design spec section 5). The UI only ever
 * observes these; every transition is derived from the authoritative sidecar
 * `pw.status` / tool results, never from local timers. Timers can only terminate
 * an operation with an error, they can never declare success.
 */
enum class pw_session_state
{
    idle,
    starting,
    authenticating,
    role_list_loading,
    character_select,
    selecting_role,
    entering_world,
    in_world,
    error,
};

/** Camera the UI should show for the current state (applied on the main thread). */
enum class pw_session_camera
{
    none,   // keep whatever camera is active
    login,  // login scene login preset
    selchar,// character-select overview preset
    choose, // per-character close-up preset
};

struct pw_session_role
{
    int32_t role_id = 0;
    std::string name;
    int32_t profession = 0;
    int32_t gender = 0;
    int32_t race = 0;
    int32_t level = 0;
    int32_t level2 = 0;
    int32_t status = 0;
    bool deleting = false;
    int32_t worldtag = 0;
};

/** Normalized preview data for the selected role (from read-only pw.role_preview). */
struct pw_session_preview
{
    uint32_t serial = 0;   // bumps on every published preview; 0 = none
    int32_t role_id = 0;
    int32_t profession = 0;
    int32_t gender = 0;
    int32_t race = 0;
    bool custom_present = false;
    uint32_t color_body = 0;
    uint32_t color_hair = 0;
    std::vector<std::pair<int32_t, uint32_t>> equipment; // slot, itemId
    std::string error; // preview fetch diagnostic; never blocks login/enter
};

/** One nearby world entity (from read-only pw.nearby). */
struct pw_world_entity
{
    int32_t id = 0;           // runtime object id (per world epoch)
    std::string kind;         // monster|npc|player|matter|mine
    uint32_t template_id = 0; // 0 for players
    std::string name;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float dist = 0.0f;
    bool dead = false;
    bool gatherable = false;
};

/** Self world state (from read-only pw.status), meaningful only in_world. */
struct pw_world_self
{
    int32_t role_id = 0;
    int32_t instance_id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float dir_x = 0.0f;
    float dir_y = 0.0f;
    float dir_z = 1.0f;
    int32_t hp = 0;
    int32_t max_hp = 0;
    int32_t mp = 0;
    int32_t max_mp = 0;
    int32_t level = 0;
    bool dead = false;
    bool moving = false;
    int32_t target_id = 0;
};

/**
 * World replication snapshot (WP7). `seq` is a monotonic publish counter
 * (0 = none); `epoch` increments on every in_world entry, so consumers key
 * replicated entities by (epoch, id) and a reconnect/resync is just an epoch
 * bump with a full re-publish (nearby sets are complete snapshots, not deltas).
 * Stream failures surface only in `error` — never as a session error.
 */
struct pw_session_world
{
    uint64_t seq = 0;
    uint32_t epoch = 0;
    pw_world_self self;
    std::vector<pw_world_entity> entities;
    std::string error;
};

struct pw_session_snapshot
{
    pw_session_state state = pw_session_state::idle;
    std::string error_code;     // machine-readable terminal code; empty unless state == error
    std::string error_message;  // human-readable detail; never contains credentials
    std::vector<pw_session_role> roles;
    uint32_t role_list_revision = 0;
    int32_t selected_role_id = 0;
    pw_session_camera desired_camera = pw_session_camera::login;
    bool mutation_capable = false;
    uint32_t child_pid = 0;
    int32_t attested_role_id = 0;
    int32_t attested_instance_id = 0;
    pw_session_preview preview;
    pw_session_world world;
};

struct pw_session_connect_params
{
    std::wstring executable_path;
    std::wstring working_directory;
    std::wstring server;
    std::string account_utf8;
    std::string password_utf8;
    uint32_t connect_deadline_ms = 60000; // overall start+auth+list budget
    uint32_t enter_deadline_ms = 60000;   // overall select+enter budget

    /** Zeroes credential material. Call once the values were consumed. */
    void clear_secrets();
};

auto pw_session_state_name(pw_session_state state) -> const char*;
auto pw_session_camera_name(pw_session_camera camera) -> const char*;

/**
 * Owns the interactive PW login flow for the Unravel editor: one worker thread
 * performs every bridge call (the UI thread never blocks), intents from the UI
 * are posted through a command slot, and the public snapshot is the only state
 * the UI reads. Revision rule: the `pw.status` login.roleListRevision is the
 * authoritative gating value; `pw.role_list` payload content is adopted with
 * the status revision (a native revision is monotonic per process, so a payload
 * revision below the status revision can only come from a replayed fixture
 * response, never from the real client).
 */
class pw_session_controller
{
public:
    explicit pw_session_controller(pw_runtime_client& runtime);
    ~pw_session_controller();
    pw_session_controller(const pw_session_controller&) = delete;
    auto operator=(const pw_session_controller&) -> pw_session_controller& = delete;

    /** Spawns the worker thread. Idempotent. */
    void start();
    /** Signals the worker to stop and joins it. Never blocks indefinitely:
     *  bridge calls carry finite timeouts, so the worker always surfaces. */
    void shutdown();

    // Intents — thread-safe, non-blocking. Return false when rejected in the
    // current state (re-checked by the worker at execution time).
    auto connect(pw_session_connect_params params) -> bool;  // from idle or error
    auto disconnect() -> bool;                               // any non-idle state -> idle
    auto refresh_roles() -> bool;                            // character_select
    auto select_role(int32_t role_id) -> bool;               // character_select, role present and not deleting
    auto enter_selected_role() -> bool;                      // character_select with a selection
    auto cancel() -> bool;                                   // aborts in-flight connect/enter (-> idle)
    // WP7: exact-role-gated world mutation, in_world only. action is one of
    // move_to (x/y/z), stop_move, jump, select_target (id), normal_attack.
    // The outcome surfaces in snapshot.world.error (empty on success).
    auto world_action(const std::string& action, float x, float y, float z, int32_t id) -> bool;
    auto get_snapshot() const -> pw_session_snapshot;

private:
    struct implementation;
    std::unique_ptr<implementation> implementation_;
};
} // namespace unravel
