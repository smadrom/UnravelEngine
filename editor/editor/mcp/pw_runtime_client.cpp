#include "pw_runtime_client.h"

#include <editor/mcp/json.hpp>
#include <pw_runtime/pw_runtime_api.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace unravel
{
namespace
{
using json = nlohmann::json;
constexpr std::array<std::string_view, 15> READ_ONLY_GAME_TOOLS = {
    "pw.status",
    "pw.chat_recent",
    "pw.nearby",
    "pw.get_target",
    "pw.auto_combat_status",
    "pw.inventory",
    "pw.skills",
    "pw.team_status",
    "pw.equipment",
    "pw.equipment_advice",
    "pw.service_directory",
    "pw.travel_status",
    "pw.quest_status",
    "pw.role_list",
    "pw.role_preview",
};

// WP7 world actions: the only mutation tools the editor may forward, and only
// with a live exact-role mutation capability (see call_mutation).
constexpr std::array<std::string_view, 5> WORLD_MUTATION_TOOLS = {
    "pw.move_to",
    "pw.stop_move",
    "pw.jump",
    "pw.select_target",
    "pw.normal_attack",
};

auto is_world_mutation_tool(std::string_view tool_name) -> bool
{
    return std::find(WORLD_MUTATION_TOOLS.begin(), WORLD_MUTATION_TOOLS.end(), tool_name) !=
        WORLD_MUTATION_TOOLS.end();
}

auto is_read_only_game_tool(std::string_view tool_name, const json& arguments) -> bool
{
    if(std::find(READ_ONLY_GAME_TOOLS.begin(), READ_ONLY_GAME_TOOLS.end(), tool_name) != READ_ONLY_GAME_TOOLS.end())
    {
        return true;
    }
    if(tool_name == "pw.npc_service" && arguments.empty())
    {
        return true;
    }
    if(tool_name != "pw.set_mode" || arguments.size() != 2 || !arguments.contains("mode") || !arguments.contains("role_id"))
    {
        return false;
    }
    return arguments["mode"].is_number_integer() && arguments["mode"].get<int64_t>() == 0 &&
           arguments["role_id"].is_number_integer() && arguments["role_id"].get<int64_t>() == 0;
}

#if defined(_WIN32)
auto get_environment_value(const wchar_t* name) -> std::wstring
{
    const DWORD required_size = GetEnvironmentVariableW(name, nullptr, 0);
    if(required_size == 0)
    {
        return {};
    }
    std::wstring value(required_size, L'\0');
    const DWORD copied_size = GetEnvironmentVariableW(name, value.data(), required_size);
    if(copied_size == 0 || copied_size >= required_size)
    {
        return {};
    }
    value.resize(copied_size);
    return value;
}

auto get_editor_directory() -> std::filesystem::path
{
    std::wstring module_path(32768, L'\0');
    const DWORD path_size = GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if(path_size == 0 || path_size >= module_path.size())
    {
        return {};
    }
    module_path.resize(path_size);
    return std::filesystem::path(module_path).parent_path();
}

void PW_RUNTIME_CALL capture_response(const char* response_json, uint32_t response_size, void* user_data)
{
    auto* response = static_cast<std::string*>(user_data);
    response->assign(response_json, response_size);
}
#endif
}

struct pw_runtime_client::implementation
{
    // Serializes every public entry point: the bridge DLL serializes requests
    // internally, but client-side members (next_request_id, last_error, the
    // capability tuple) race when the session-controller worker thread and the
    // MCP server thread call in concurrently. Recursive because
    // has_mutation_capability() re-enters call_tool().
    std::recursive_mutex mutex;
#if defined(_WIN32)
    HMODULE library = nullptr;
    pw_runtime_api_v2 api{};
    bool has_v2 = false;
    pw_runtime_handle handle = nullptr;
    uint64_t next_request_id = 2;
    // Mutation capability: granted only by an attested pw.role_enter completion
    // and bound to the exact child PID, role and world instance. Any restart,
    // stop, or live-state mismatch revokes it.
    bool mutation_capability = false;
    uint32_t capability_pid = 0;
    int32_t capability_role_id = 0;
    int32_t capability_instance_id = 0;
#endif
    std::string last_error;

#if defined(_WIN32)
    auto copy_runtime_error() -> std::string
    {
        if(handle == nullptr || api.copy_last_error == nullptr)
        {
            return last_error;
        }
        uint32_t required_size = 0;
        const pw_runtime_result size_result = api.copy_last_error(handle, nullptr, 0, &required_size);
        if(size_result != PW_RUNTIME_RESULT_BUFFER_TOO_SMALL || required_size == 0)
        {
            return last_error;
        }
        std::vector<char> buffer(required_size, '\0');
        if(api.copy_last_error(handle, buffer.data(), static_cast<uint32_t>(buffer.size()), &required_size) != PW_RUNTIME_RESULT_OK)
        {
            return last_error;
        }
        return std::string(buffer.data());
    }

    void revoke_capability()
    {
        mutation_capability = false;
        capability_pid = 0;
        capability_role_id = 0;
        capability_instance_id = 0;
    }
#endif
};

pw_runtime_client::pw_runtime_client()
    : implementation_(std::make_unique<implementation>())
{
}

pw_runtime_client::~pw_runtime_client()
{
    deinit();
}

auto pw_runtime_client::init() -> bool
{
    std::lock_guard<std::recursive_mutex> lock(implementation_->mutex);
#if defined(_WIN32)
    if(implementation_->library != nullptr)
    {
        return true;
    }
    std::filesystem::path library_path = get_environment_value(L"PW_RUNTIME_LIBRARY");
    if(library_path.empty())
    {
        library_path = get_editor_directory() / L"PWRuntimeBridge.dll";
    }
    implementation_->library = LoadLibraryW(library_path.c_str());
    if(implementation_->library == nullptr)
    {
        implementation_->last_error = "PWRuntimeBridge.dll is not available";
        return false;
    }
    const auto get_api = reinterpret_cast<pw_runtime_get_api_fn>(GetProcAddress(implementation_->library, "pw_runtime_get_api"));
    if(get_api == nullptr)
    {
        implementation_->last_error = "PW runtime plugin has no pw_runtime_get_api entry point";
        deinit();
        return false;
    }
    // Prefer the v2 table (credentials-carrying start); fall back to v1 so older
    // bridge DLLs keep working without the role-admin start path.
    pw_runtime_result api_result = get_api(PW_RUNTIME_ABI_VERSION_V2,
                                           sizeof(implementation_->api),
                                           reinterpret_cast<pw_runtime_api_v1*>(&implementation_->api));
    if(api_result == PW_RUNTIME_RESULT_OK && implementation_->api.abi_version == PW_RUNTIME_ABI_VERSION_V2)
    {
        implementation_->has_v2 = true;
    }
    else
    {
        implementation_->api = {};
        implementation_->has_v2 = false;
        api_result = get_api(PW_RUNTIME_ABI_VERSION_V1,
                             sizeof(pw_runtime_api_v1),
                             reinterpret_cast<pw_runtime_api_v1*>(&implementation_->api));
        if(api_result != PW_RUNTIME_RESULT_OK || implementation_->api.abi_version != PW_RUNTIME_ABI_VERSION_V1)
        {
            implementation_->last_error = "PW runtime plugin supports neither ABI v2 nor v1";
            deinit();
            return false;
        }
    }
    const pw_runtime_result create_result = implementation_->api.create(&implementation_->handle);
    if(create_result != PW_RUNTIME_RESULT_OK || implementation_->handle == nullptr)
    {
        implementation_->last_error = "PW runtime plugin could not create a runtime instance";
        deinit();
        return false;
    }
    implementation_->last_error.clear();
    return true;
#else
    implementation_->last_error = "PW runtime plugins are currently supported only on Windows";
    return false;
#endif
}

void pw_runtime_client::deinit()
{
    std::lock_guard<std::recursive_mutex> lock(implementation_->mutex);
#if defined(_WIN32)
    implementation_->revoke_capability();
    if(implementation_->handle != nullptr && implementation_->api.destroy != nullptr)
    {
        implementation_->api.destroy(implementation_->handle);
        implementation_->handle = nullptr;
    }
    if(implementation_->library != nullptr)
    {
        FreeLibrary(implementation_->library);
        implementation_->library = nullptr;
    }
    implementation_->api = {};
    implementation_->has_v2 = false;
    implementation_->next_request_id = 2;
#endif
}

auto pw_runtime_client::start_from_environment() -> pw_runtime_operation
{
    std::lock_guard<std::recursive_mutex> lock(implementation_->mutex);
    pw_runtime_operation operation;
#if defined(_WIN32)
    if(!init())
    {
        operation.error = implementation_->last_error;
        return operation;
    }
    const std::filesystem::path executable_path = get_environment_value(L"PW_RUNTIME_CLIENT_EXE");
    if(executable_path.empty())
    {
        operation.error = "PW_RUNTIME_CLIENT_EXE is not configured";
        return operation;
    }
    std::filesystem::path working_directory = get_environment_value(L"PW_RUNTIME_WORKING_DIRECTORY");
    if(working_directory.empty())
    {
        working_directory = executable_path.parent_path();
    }
    const std::wstring executable_value = executable_path.wstring();
    const std::wstring working_directory_value = working_directory.wstring();
    pw_runtime_start_info_v1 start_info{};
    start_info.struct_size = sizeof(start_info);
    start_info.executable_path = executable_value.c_str();
    start_info.working_directory = working_directory_value.c_str();
    start_info.startup_timeout_ms = 10000;
    const pw_runtime_result result = implementation_->api.start(implementation_->handle, &start_info);
    if(result != PW_RUNTIME_RESULT_OK)
    {
        operation.error = implementation_->copy_runtime_error();
        implementation_->last_error = operation.error;
        return operation;
    }
    implementation_->next_request_id = 2;
    // A fresh child process means a fresh login session: any previously
    // attested mutation capability belongs to the old PID and is revoked.
    implementation_->revoke_capability();
    implementation_->last_error.clear();
    operation.ok = true;
    return operation;
#else
    operation.error = implementation_->last_error;
    return operation;
#endif
}

auto pw_runtime_client::start_role_admin(const std::wstring& executable_path,
                                         const std::wstring& working_directory,
                                         const std::wstring& server,
                                         const std::string& account_utf8,
                                         const std::string& password_utf8,
                                         uint32_t startup_timeout_ms) -> pw_runtime_operation
{
    std::lock_guard<std::recursive_mutex> lock(implementation_->mutex);
    pw_runtime_operation operation;
#if defined(_WIN32)
    if(!init())
    {
        operation.error = implementation_->last_error;
        return operation;
    }
    if(!implementation_->has_v2 || implementation_->api.start_v2 == nullptr)
    {
        operation.error = "PW runtime role-admin start requires bridge ABI v2";
        implementation_->last_error = operation.error;
        return operation;
    }
    if(executable_path.empty())
    {
        operation.error = "role-admin start requires an executable path";
        implementation_->last_error = operation.error;
        return operation;
    }
    pw_runtime_start_info_v2 start_info{};
    start_info.struct_size = sizeof(start_info);
    start_info.executable_path = executable_path.c_str();
    start_info.working_directory = working_directory.empty() ? nullptr : working_directory.c_str();
    start_info.startup_timeout_ms = startup_timeout_ms;
    start_info.role_admin = 1;
    start_info.account_utf8 = account_utf8.empty()
        ? nullptr
        : reinterpret_cast<const uint8_t*>(account_utf8.data());
    start_info.account_utf8_size = static_cast<uint32_t>(account_utf8.size());
    start_info.password_utf8 = password_utf8.empty()
        ? nullptr
        : reinterpret_cast<const uint8_t*>(password_utf8.data());
    start_info.password_utf8_size = static_cast<uint32_t>(password_utf8.size());
    start_info.server = server.empty() ? nullptr : server.c_str();
    start_info.exact_role_id = 0;
    const pw_runtime_result result = implementation_->api.start_v2(implementation_->handle, &start_info);
    if(result != PW_RUNTIME_RESULT_OK)
    {
        operation.error = implementation_->copy_runtime_error();
        implementation_->last_error = operation.error;
        return operation;
    }
    implementation_->next_request_id = 2;
    implementation_->revoke_capability();
    implementation_->last_error.clear();
    operation.ok = true;
    return operation;
#else
    (void)executable_path;
    (void)working_directory;
    (void)server;
    (void)account_utf8;
    (void)password_utf8;
    (void)startup_timeout_ms;
    operation.error = implementation_->last_error;
    return operation;
#endif
}

auto pw_runtime_client::stop() -> pw_runtime_operation
{
    std::lock_guard<std::recursive_mutex> lock(implementation_->mutex);
    pw_runtime_operation operation;
#if defined(_WIN32)
    if(implementation_->handle == nullptr)
    {
        operation.error = "PW runtime plugin is not loaded";
        return operation;
    }
    const pw_runtime_result result = implementation_->api.stop(implementation_->handle, 2000);
    if(result != PW_RUNTIME_RESULT_OK)
    {
        operation.error = implementation_->copy_runtime_error();
        implementation_->last_error = operation.error;
        return operation;
    }
    // The sidecar session is gone; the attested capability cannot survive it.
    implementation_->revoke_capability();
    implementation_->last_error.clear();
    operation.ok = true;
    return operation;
#else
    operation.error = implementation_->last_error;
    return operation;
#endif
}

auto pw_runtime_client::get_status() -> pw_runtime_status
{
    std::lock_guard<std::recursive_mutex> lock(implementation_->mutex);
    pw_runtime_status status;
#if defined(_WIN32)
    status.is_available = implementation_->handle != nullptr;
    if(implementation_->handle == nullptr)
    {
        status.error = implementation_->last_error;
        return status;
    }
    pw_runtime_status_v1 runtime_status{};
    runtime_status.struct_size = sizeof(runtime_status);
    const pw_runtime_result result = implementation_->api.get_status(implementation_->handle, &runtime_status);
    if(result != PW_RUNTIME_RESULT_OK)
    {
        status.error = implementation_->copy_runtime_error();
        implementation_->last_error = status.error;
        return status;
    }
    status.process_state = runtime_status.process_state;
    status.process_id = runtime_status.process_id;
    status.exit_code = runtime_status.exit_code;
    return status;
#else
    status.error = implementation_->last_error;
    return status;
#endif
}

auto pw_runtime_client::call_tool(const std::string& tool_name,
                                  const std::string& arguments_json,
                                  uint32_t timeout_ms) -> pw_runtime_operation
{
    std::lock_guard<std::recursive_mutex> lock(implementation_->mutex);
    pw_runtime_operation operation;
#if defined(_WIN32)
    if(implementation_->handle == nullptr)
    {
        operation.error = "PW runtime plugin is not loaded";
        return operation;
    }
    json arguments = json::parse(arguments_json, nullptr, false);
    if(arguments.is_discarded() || !arguments.is_object())
    {
        operation.error = "PW runtime tool arguments must be a JSON object";
        return operation;
    }
    if(!is_read_only_game_tool(tool_name, arguments))
    {
        operation.error = "PW runtime mutations require enhanced-login exact-role attestation; the current DLL backend is read-only";
        return operation;
    }
    const json request = {
        {"jsonrpc", "2.0"},
        {"id", implementation_->next_request_id++},
        {"method", "tools/call"},
        {"params", {{"name", tool_name}, {"arguments", std::move(arguments)}}},
    };
    const std::string request_json = request.dump();
    const pw_runtime_result result = implementation_->api.request_json(implementation_->handle,
                                                                       request_json.data(),
                                                                       static_cast<uint32_t>(request_json.size()),
                                                                       timeout_ms,
                                                                       &capture_response,
                                                                       &operation.response_json);
    if(result != PW_RUNTIME_RESULT_OK)
    {
        operation.error = implementation_->copy_runtime_error();
        implementation_->last_error = operation.error;
        return operation;
    }
    implementation_->last_error.clear();
    operation.ok = true;
    return operation;
#else
    (void)tool_name;
    (void)arguments_json;
    (void)timeout_ms;
    operation.error = implementation_->last_error;
    return operation;
#endif
}

auto pw_runtime_client::call_mutation(const std::string& tool_name,
                                      const std::string& arguments_json,
                                      uint32_t timeout_ms) -> pw_runtime_operation
{
    std::lock_guard<std::recursive_mutex> lock(implementation_->mutex);
    pw_runtime_operation operation;
#if defined(_WIN32)
    if(implementation_->handle == nullptr)
    {
        operation.error = "PW runtime plugin is not loaded";
        return operation;
    }
    json arguments = json::parse(arguments_json, nullptr, false);
    if(arguments.is_discarded() || !arguments.is_object())
    {
        operation.error = "PW runtime tool arguments must be a JSON object";
        return operation;
    }
    if(!is_world_mutation_tool(tool_name))
    {
        operation.error = "PW runtime mutation tool is not on the world-action allowlist";
        return operation;
    }
    // Live revalidation against child PID + exact role/world; revokes and
    // refuses on any mismatch. Recursive: re-enters this same mutex.
    if(!has_mutation_capability())
    {
        operation.error = "PW runtime world action requires an attested in-world session";
        return operation;
    }
    const json request = {
        {"jsonrpc", "2.0"},
        {"id", implementation_->next_request_id++},
        {"method", "tools/call"},
        {"params", {{"name", tool_name}, {"arguments", std::move(arguments)}}},
    };
    const std::string request_json = request.dump();
    const pw_runtime_result result = implementation_->api.request_json(implementation_->handle,
                                                                       request_json.data(),
                                                                       static_cast<uint32_t>(request_json.size()),
                                                                       timeout_ms,
                                                                       &capture_response,
                                                                       &operation.response_json);
    if(result != PW_RUNTIME_RESULT_OK)
    {
        operation.error = implementation_->copy_runtime_error();
        implementation_->last_error = operation.error;
        return operation;
    }
    implementation_->last_error.clear();
    operation.ok = true;
    return operation;
#else
    (void)tool_name;
    (void)arguments_json;
    (void)timeout_ms;
    operation.error = implementation_->last_error;
    return operation;
#endif
}

auto pw_runtime_client::enter_role(int32_t role_id,
                                   uint32_t role_list_revision,
                                   int32_t expected_worldtag,
                                   uint32_t operation_id,
                                   uint32_t timeout_ms) -> pw_role_enter_result
{
    std::lock_guard<std::recursive_mutex> lock(implementation_->mutex);
    pw_role_enter_result outcome;
#if defined(_WIN32)
    if(implementation_->handle == nullptr)
    {
        outcome.error = "PW runtime plugin is not loaded";
        return outcome;
    }
    if(role_id <= 0 || role_list_revision == 0 || operation_id == 0)
    {
        outcome.error = "enter_role requires positive role_id, role_list_revision and operation_id";
        return outcome;
    }
    // Typed mutation path: pw.role_enter is the only gameplay-adjacent mutation
    // and never flows through the generic read-only call_tool gate.
    const json arguments = {
        {"role_id", role_id},
        {"role_list_revision", role_list_revision},
        {"operation_id", operation_id},
    };
    const json request = {
        {"jsonrpc", "2.0"},
        {"id", implementation_->next_request_id++},
        {"method", "tools/call"},
        {"params", {{"name", "pw.role_enter"}, {"arguments", arguments}}},
    };
    const std::string request_json = request.dump();
    const pw_runtime_result result = implementation_->api.request_json(implementation_->handle,
                                                                       request_json.data(),
                                                                       static_cast<uint32_t>(request_json.size()),
                                                                       timeout_ms,
                                                                       &capture_response,
                                                                       &outcome.response_json);
    if(result != PW_RUNTIME_RESULT_OK)
    {
        outcome.error = implementation_->copy_runtime_error();
        implementation_->last_error = outcome.error;
        return outcome;
    }
    const json response = json::parse(outcome.response_json, nullptr, false);
    const json* structured = nullptr;
    if(!response.is_discarded() && response.is_object() && response.contains("result") &&
       response["result"].is_object() && response["result"].contains("structuredContent") &&
       response["result"]["structuredContent"].is_object())
    {
        structured = &response["result"]["structuredContent"];
    }
    if(structured == nullptr)
    {
        outcome.error = "PW runtime returned a malformed pw.role_enter response";
        implementation_->last_error = outcome.error;
        return outcome;
    }
    const bool native_ok = structured->value("ok", false);
    if(!native_ok)
    {
        outcome.ok = true;
        outcome.error_code = structured->value("errorCode", std::string());
        outcome.error = structured->value("message", std::string("pw.role_enter failed"));
        // A proven role/world mismatch is also the native revocation signal;
        // mirror it here so the editor never retains a stale capability.
        if(outcome.error_code == "role_mismatch" || outcome.error_code == "world_mismatch" ||
           outcome.error_code == "role_management_connection_reset" ||
           outcome.error_code == "connection_lost")
        {
            implementation_->revoke_capability();
        }
        return outcome;
    }
    const json& payload = structured->contains("payload") && (*structured)["payload"].is_object()
        ? (*structured)["payload"]
        : json::object();
    outcome.ok = true;
    if(payload.value("result", std::string()) == "pending")
    {
        outcome.pending = true;
        return outcome;
    }
    if(payload.value("status", std::string()) != "entered")
    {
        outcome.error = "pw.role_enter returned an unexpected payload";
        implementation_->last_error = outcome.error;
        return outcome;
    }
    const int32_t attested_role = payload.value("attestedRoleId", 0);
    const int32_t attested_instance = payload.value("instanceId", 0);
    outcome.attested_role_id = attested_role;
    outcome.attested_instance_id = attested_instance;
    if(attested_role != role_id || attested_instance != expected_worldtag)
    {
        // Defense in depth: the native side already fails closed, and the
        // editor mirrors the revocation instead of trusting the payload.
        implementation_->revoke_capability();
        outcome.error_code = "attestation_mismatch";
        outcome.error = "pw.role_enter attestation does not match the requested role/world";
        implementation_->last_error = outcome.error;
        return outcome;
    }
    uint32_t current_pid = 0;
    {
        pw_runtime_status_v1 runtime_status{};
        runtime_status.struct_size = sizeof(runtime_status);
        if(implementation_->api.get_status(implementation_->handle, &runtime_status) == PW_RUNTIME_RESULT_OK &&
           runtime_status.process_state == PW_RUNTIME_PROCESS_RUNNING)
        {
            current_pid = runtime_status.process_id;
        }
    }
    if(current_pid == 0)
    {
        implementation_->revoke_capability();
        outcome.error_code = "attestation_mismatch";
        outcome.error = "pw.role_enter succeeded but the child process state is unavailable";
        implementation_->last_error = outcome.error;
        return outcome;
    }
    implementation_->mutation_capability = true;
    implementation_->capability_pid = current_pid;
    implementation_->capability_role_id = attested_role;
    implementation_->capability_instance_id = attested_instance;
    outcome.entered = true;
    return outcome;
#else
    (void)role_id;
    (void)role_list_revision;
    (void)expected_worldtag;
    (void)operation_id;
    (void)timeout_ms;
    outcome.error = implementation_->last_error;
    return outcome;
#endif
}

auto pw_runtime_client::has_mutation_capability() -> bool
{
    std::lock_guard<std::recursive_mutex> lock(implementation_->mutex);
#if defined(_WIN32)
    if(!implementation_->mutation_capability)
    {
        return false;
    }
    pw_runtime_status_v1 runtime_status{};
    runtime_status.struct_size = sizeof(runtime_status);
    if(implementation_->handle == nullptr ||
       implementation_->api.get_status(implementation_->handle, &runtime_status) != PW_RUNTIME_RESULT_OK ||
       runtime_status.process_state != PW_RUNTIME_PROCESS_RUNNING ||
       runtime_status.process_id != implementation_->capability_pid)
    {
        implementation_->revoke_capability();
        return false;
    }
    const pw_runtime_operation status = call_tool("pw.status", "{}", 5000);
    bool live_match = false;
    if(status.ok)
    {
        const json response = json::parse(status.response_json, nullptr, false);
        if(!response.is_discarded() && response.is_object() && response.contains("result") &&
           response["result"].is_object() && response["result"].contains("structuredContent") &&
           response["result"]["structuredContent"].is_object())
        {
            const json& state = response["result"]["structuredContent"].contains("state") &&
                                response["result"]["structuredContent"]["state"].is_object()
                ? response["result"]["structuredContent"]["state"]
                : json::object();
            live_match = state.value("inWorld", false) &&
                state.value("roleId", 0) == implementation_->capability_role_id &&
                state.value("instanceId", 0) == implementation_->capability_instance_id;
        }
    }
    if(!live_match)
    {
        implementation_->revoke_capability();
        return false;
    }
    return true;
#else
    return false;
#endif
}

void pw_runtime_client::revoke_mutation_capability()
{
    std::lock_guard<std::recursive_mutex> lock(implementation_->mutex);
#if defined(_WIN32)
    implementation_->revoke_capability();
#endif
}

auto pw_runtime_client::get_last_error() const -> const std::string&
{
    std::lock_guard<std::recursive_mutex> lock(implementation_->mutex);
    return implementation_->last_error;
}
}
