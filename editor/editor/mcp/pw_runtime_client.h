#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace unravel
{
struct pw_runtime_operation
{
    bool ok = false;
    std::string response_json;
    std::string error;
};

struct pw_runtime_status
{
    bool is_available = false;
    uint32_t process_state = 0;
    uint32_t process_id = 0;
    int32_t exit_code = 0;
    std::string error;
};

/** Outcome of one typed pw.role_enter call against the headless runtime. */
struct pw_role_enter_result
{
    bool ok = false;               // request reached the runtime and was well-formed
    bool pending = false;          // operation accepted/in flight; poll again with the same ids
    bool entered = false;          // exact role+world attestation succeeded
    int32_t attested_role_id = 0;
    int32_t attested_instance_id = 0;
    std::string error_code;        // native terminal code (role_mismatch, world_mismatch, ...)
    std::string error;
    std::string response_json;
};

/** Loads and controls the optional versioned Perfect World runtime plugin. */
class pw_runtime_client
{
public:
    pw_runtime_client();
    ~pw_runtime_client();
    pw_runtime_client(const pw_runtime_client&) = delete;
    auto operator=(const pw_runtime_client&) -> pw_runtime_client& = delete;

    /** Loads the runtime DLL without starting a game client. */
    auto init() -> bool;
    /** Stops the child runtime and unloads the DLL. */
    void deinit();
    /** Starts the configured headless client from PW_RUNTIME_CLIENT_EXE. */
    auto start_from_environment() -> pw_runtime_operation;
    /**
     * Starts the headless client in role-admin character-select mode. Credentials
     * travel only through the v2 ABI as byte spans; they never touch MCP JSON,
     * the command line, environment inheritance of the editor, or logs.
     */
    auto start_role_admin(const std::wstring& executable_path,
                          const std::wstring& working_directory,
                          const std::wstring& server,
                          const std::string& account_utf8,
                          const std::string& password_utf8,
                          uint32_t startup_timeout_ms = 10000) -> pw_runtime_operation;
    /** Stops the configured headless client. */
    auto stop() -> pw_runtime_operation;
    /** Returns plugin and child-process lifecycle state. */
    auto get_status() -> pw_runtime_status;
    /** Calls one MCP tool on the headless client. */
    auto call_tool(const std::string& tool_name,
                   const std::string& arguments_json,
                   uint32_t timeout_ms = 10000) -> pw_runtime_operation;
    /**
     * Typed world-mutation path (WP7): a small allowlist of in-world action
     * tools (move/stop/jump/target/attack), forwarded only while the mutation
     * capability (attested enter, bound to child PID/role/instance) still
     * revalidates live. Everything else stays read-only through call_tool.
     */
    auto call_mutation(const std::string& tool_name,
                       const std::string& arguments_json,
                       uint32_t timeout_ms = 10000) -> pw_runtime_operation;
    /**
     * Typed mutation path: select one exact role of the current list revision and
     * enter the world. Pending operations are polled by repeating the same ids.
     * Success additionally binds the mutation capability to this child PID, role
     * and worldtag/instance; any mismatch revokes it.
     */
    auto enter_role(int32_t role_id,
                    uint32_t role_list_revision,
                    int32_t expected_worldtag,
                    uint32_t operation_id,
                    uint32_t timeout_ms = 10000) -> pw_role_enter_result;
    /** Revalidates the mutation capability against live state (PID + exact role/world). */
    auto has_mutation_capability() -> bool;
    /** Drops the mutation capability without contacting the runtime. */
    void revoke_mutation_capability();
    /** Returns the last loader or runtime error. */
    auto get_last_error() const -> const std::string&;

private:
    struct implementation;
    std::unique_ptr<implementation> implementation_;
};
}
