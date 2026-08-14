#include "mcp_tool_registry.h"

#include <editor/mcp/json.hpp>
#include <editor/mcp/mcp_commands.h>
#include <editor/mcp/mcp_system.h>

#include <pw_runtime/pw_runtime_api.h>

#include <exception>
#include <cstdlib>
#include <string>
#include <utility>

namespace unravel::mcp
{
namespace
{
using json = nlohmann::json;

auto call_pw_bridge(rtti::context& ctx,
                    const simdjson::dom::object& args,
                    const std::string& method) -> tool_result
{
    try
    {
        const std::string params_json = std::string(simdjson::minify(args));
        json params = json::parse(params_json, nullptr, false);
        if(params.is_discarded() || !params.is_object())
        {
            return {.text = "Invalid PW tool arguments", .is_error = true};
        }

        const json request = {
            {"seq", 0},
            {"method", method},
            {"params", std::move(params)},
        };
        auto& system = ctx.get_cached<mcp_system>();
        const std::string response_json = mcp_commands::dispatch(ctx, request.dump(), system);
        const json response = json::parse(response_json, nullptr, false);
        if(response.is_discarded() || !response.is_object())
        {
            return {.text = "PW bridge returned malformed JSON", .is_error = true};
        }
        if(!response.value("ok", false))
        {
            return {.text = response.value("error", "PW bridge tool failed"), .is_error = true};
        }
        if(!response.contains("result"))
        {
            return {.text = "PW bridge response has no result", .is_error = true};
        }
        if(method == "camera_preset")
        {
            system.sync_camera_to_scene_viewport(ctx);
        }
        return {.text = response["result"].dump(), .is_error = false};
    }
    catch(const std::exception& error)
    {
        return {.text = error.what(), .is_error = true};
    }
}

auto make_pw_tool(std::string name,
                  std::string description,
                  std::string schema,
                  std::string bridge_method,
                  bool mutates_scene = false) -> mcp_tool
{
    return {
        .name = std::move(name),
        .description = std::move(description),
        .input_schema_json = std::move(schema),
        .handler = [method = std::move(bridge_method)](rtti::context& ctx, const simdjson::dom::object& args)
        {
            return call_pw_bridge(ctx, args, method);
        },
        .mutates_scene = mutates_scene,
    };
}

auto unwrap_runtime_response(const pw_runtime_operation& operation) -> tool_result
{
    if(!operation.ok)
    {
        return {.text = operation.error, .is_error = true};
    }
    const json response = json::parse(operation.response_json, nullptr, false);
    if(response.is_discarded() || !response.is_object())
    {
        return {.text = "PW runtime returned malformed JSON-RPC", .is_error = true};
    }
    if(response.contains("error"))
    {
        return {.text = response["error"].value("message", "PW runtime request failed"), .is_error = true};
    }
    if(!response.contains("result"))
    {
        return {.text = "PW runtime response has no result", .is_error = true};
    }
    const json& result = response["result"];
    if(result.value("isError", false))
    {
        const std::string message = result.contains("content") && result["content"].is_array() && !result["content"].empty()
            ? result["content"][0].value("text", "PW runtime tool failed")
            : "PW runtime tool failed";
        return {.text = message, .is_error = true};
    }
    if(result.contains("structuredContent"))
    {
        return {.text = result["structuredContent"].dump(), .is_error = false};
    }
    return {.text = result.dump(), .is_error = false};
}

auto make_runtime_status(rtti::context& ctx) -> tool_result
{
    auto& runtime = ctx.get_cached<mcp_system>().get_pw_runtime();
    const pw_runtime_status status = runtime.get_status();
    json payload = {
        {"available", status.is_available},
        {"processState", status.process_state},
        {"processId", status.process_id},
        {"exitCode", status.exit_code},
    };
    if(!status.error.empty())
    {
        payload["error"] = status.error;
    }
    if(status.process_state == PW_RUNTIME_PROCESS_RUNNING)
    {
        const pw_runtime_operation client_status = runtime.call_tool("pw.status", "{}", 5000);
        if(client_status.ok)
        {
            const json response = json::parse(client_status.response_json, nullptr, false);
            if(!response.is_discarded() && response.is_object() && response.contains("result"))
            {
                const json& result = response["result"];
                payload["client"] = result.contains("structuredContent") ? result["structuredContent"] : result;
            }
        }
        else
        {
            payload["clientError"] = client_status.error;
        }
    }
    return {.text = payload.dump(), .is_error = false};
}
} // namespace

void register_pw_tools(mcp_tool_registry& registry)
{
    registry.add(make_pw_tool(
        "pw_login_load",
        "Load the Perfect World login scene incrementally from converted project content.",
        R"({"type":"object","properties":{"content_root":{"type":"string"},"chunk_size":{"type":"integer","minimum":1},"restart":{"type":"boolean"}}})",
        "load_login",
        true));

    registry.add(make_pw_tool(
        "pw_login_status",
        "Return progress and diagnostics for the Perfect World login-scene loader.",
        R"({"type":"object","properties":{}})",
        "login_status"));

    registry.add(make_pw_tool(
        "pw_terrain_probe",
        "Sample the loaded Perfect World login terrain heightfield at one or more world positions.",
        R"({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"pos":{"type":"array","items":{"type":"number"},"minItems":3,"maxItems":3},"points":{"type":"array"}}})",
        "terrain_probe"));

    registry.add(make_pw_tool(
        "pw_camera_preset",
        "Apply an authored Perfect World login or character-selection camera preset.",
        R"({"type":"object","properties":{"preset":{"type":"string","enum":["login","selchar","create","choose"]},"index":{"type":"integer","minimum":0,"maximum":38}}})",
        "camera_preset"));

    registry.add(make_pw_tool(
        "pw_map_structure",
        "Return the active Perfect World scene hierarchy in the legacy map diagnostic format.",
        R"({"type":"object","properties":{}})",
        "get_map_structure"));

    registry.add(make_pw_tool(
        "pw_scene_detail",
        "Inspect Perfect World scene entities with mesh, submesh, material, and bounds diagnostics.",
        R"({"type":"object","properties":{"id":{"type":"integer","minimum":0},"uuid":{"type":"string"},"name":{"type":"string"},"limit":{"type":"integer","minimum":1,"maximum":1000},"submesh_limit":{"type":"integer","minimum":0,"maximum":128},"include_materials":{"type":"boolean"},"include_bounds":{"type":"boolean"},"model_only":{"type":"boolean"}}})",
        "scene_detail"));

    registry.add({
        .name = "pw_runtime_start",
        .description = "Start the configured Perfect World headless runtime through the versioned DLL bridge.",
        .input_schema_json = R"({"type":"object","additionalProperties":false,"properties":{}})",
        .handler = [](rtti::context& ctx, const simdjson::dom::object&)
        {
            auto& runtime = ctx.get_cached<mcp_system>().get_pw_runtime();
            const pw_runtime_operation operation = runtime.start_from_environment();
            if(!operation.ok)
            {
                return tool_result{.text = operation.error, .is_error = true};
            }
            return make_runtime_status(ctx);
        },
        .mutates_scene = false,
    });

    registry.add({
        .name = "pw_runtime_stop",
        .description = "Stop the Perfect World headless runtime owned by the editor.",
        .input_schema_json = R"({"type":"object","additionalProperties":false,"properties":{}})",
        .handler = [](rtti::context& ctx, const simdjson::dom::object&)
        {
            auto& runtime = ctx.get_cached<mcp_system>().get_pw_runtime();
            const pw_runtime_operation operation = runtime.stop();
            if(!operation.ok)
            {
                return tool_result{.text = operation.error, .is_error = true};
            }
            return make_runtime_status(ctx);
        },
        .mutates_scene = false,
    });

    registry.add({
        .name = "pw_runtime_status",
        .description = "Return DLL, process, login and current-character state for the Perfect World headless runtime.",
        .input_schema_json = R"({"type":"object","additionalProperties":false,"properties":{}})",
        .handler = [](rtti::context& ctx, const simdjson::dom::object&)
        {
            return make_runtime_status(ctx);
        },
        .mutates_scene = false,
    });

    registry.add({
        .name = "pw_runtime_call",
        .description = "Call an allowlisted read-only pw.* MCP tool on the Perfect World headless runtime.",
        .input_schema_json = R"({"type":"object","additionalProperties":false,"properties":{"tool":{"type":"string","pattern":"^pw\\."},"arguments":{"type":"object"}},"required":["tool"]})",
        .handler = [](rtti::context& ctx, const simdjson::dom::object& args)
        {
            const std::string arguments_json = std::string(simdjson::minify(args));
            const json parsed_args = json::parse(arguments_json, nullptr, false);
            if(parsed_args.is_discarded() || !parsed_args.is_object() || !parsed_args.contains("tool") || !parsed_args["tool"].is_string())
            {
                return tool_result{.text = "pw_runtime_call requires a tool name", .is_error = true};
            }
            const std::string tool_name = parsed_args["tool"].get<std::string>();
            if(tool_name.rfind("pw.", 0) != 0)
            {
                return tool_result{.text = "pw_runtime_call accepts only pw.* tools", .is_error = true};
            }
            const json tool_arguments = parsed_args.contains("arguments") ? parsed_args["arguments"] : json::object();
            if(!tool_arguments.is_object())
            {
                return tool_result{.text = "pw_runtime_call arguments must be an object", .is_error = true};
            }
            auto& runtime = ctx.get_cached<mcp_system>().get_pw_runtime();
            return unwrap_runtime_response(runtime.call_tool(tool_name, tool_arguments.dump()));
        },
        .mutates_scene = false,
    });

    registry.add({
        .name = "pw_runtime_role_enter",
        .description = "Typed mutation: select one exact role of the current role-list revision and enter the world (deferred; re-send the same ids to poll). Only completes after exact role/worldtag attestation.",
        .input_schema_json = R"({"type":"object","additionalProperties":false,"properties":{"role_id":{"type":"integer","minimum":1},"role_list_revision":{"type":"integer","minimum":1},"worldtag":{"type":"integer","minimum":1},"operation_id":{"type":"integer","minimum":1}},"required":["role_id","role_list_revision","worldtag","operation_id"]})",
        .handler = [](rtti::context& ctx, const simdjson::dom::object& args)
        {
            const std::string arguments_json = std::string(simdjson::minify(args));
            const json parsed_args = json::parse(arguments_json, nullptr, false);
            if(parsed_args.is_discarded() || !parsed_args.is_object() ||
               !parsed_args.contains("role_id") || !parsed_args["role_id"].is_number_integer() ||
               !parsed_args.contains("role_list_revision") || !parsed_args["role_list_revision"].is_number_unsigned() ||
               !parsed_args.contains("worldtag") || !parsed_args["worldtag"].is_number_integer() ||
               !parsed_args.contains("operation_id") || !parsed_args["operation_id"].is_number_unsigned())
            {
                return tool_result{.text = "pw_runtime_role_enter requires role_id, role_list_revision, worldtag and operation_id", .is_error = true};
            }
            auto& runtime = ctx.get_cached<mcp_system>().get_pw_runtime();
            const pw_role_enter_result outcome = runtime.enter_role(
                parsed_args["role_id"].get<int32_t>(),
                parsed_args["role_list_revision"].get<uint32_t>(),
                parsed_args["worldtag"].get<int32_t>(),
                parsed_args["operation_id"].get<uint32_t>());
            if(!outcome.ok)
            {
                return tool_result{.text = outcome.error, .is_error = true};
            }
            json payload = {
                {"pending", outcome.pending},
                {"entered", outcome.entered},
                {"attestedRoleId", outcome.attested_role_id},
                {"attestedInstanceId", outcome.attested_instance_id},
                {"mutationCapable", outcome.entered ? runtime.has_mutation_capability() : false},
            };
            if(!outcome.error_code.empty())
            {
                payload["errorCode"] = outcome.error_code;
            }
            if(!outcome.error.empty() && !outcome.entered)
            {
                payload["error"] = outcome.error;
            }
            const bool is_error = !outcome.pending && !outcome.entered && !outcome.error_code.empty();
            return tool_result{.text = payload.dump(), .is_error = is_error};
        },
        .mutates_scene = false,
    });

    registry.add(make_pw_tool(
        "pw_session_ui",
        "Show or hide the Perfect World login / character-select overlay (RmlUi document over the login scene).",
        R"({"type":"object","properties":{"active":{"type":"boolean"}},"required":["active"]})",
        "pw_session_ui",
        false));

    registry.add(make_pw_tool(
        "pw_session_status",
        "Return the PW login session snapshot: state, role list with revision, selection, attestation and error.",
        R"({"type":"object","properties":{}})",
        "pw_session_status"));

    registry.add(make_pw_tool(
        "pw_session_world",
        "Return the PW world replication snapshot (WP7): seq/epoch, self state and the nearby entity set. Read-only.",
        R"({"type":"object","properties":{}})",
        "pw_session_world"));

    registry.add(make_pw_tool(
        "pw_session_world_action",
        "Issue an exact-role-gated world action (WP7): action = move_to (with x/y/z) | stop_move | jump | select_target (with id) | normal_attack. Requires an attested in-world session.",
        R"({"type":"object","properties":{"action":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"id":{"type":"integer"}},"required":["action"]})",
        "pw_session_world_action"));

    registry.add(make_pw_tool(
        "pw_session_connect",
        "Start the interactive PW login (role-admin sidecar). Credentials come ONLY from the editor process environment (PW_SERVER, PW_ACCOUNT, PW_PASSWORD, PW_RUNTIME_CLIENT_EXE, PW_RUNTIME_WORKING_DIRECTORY) - never from MCP arguments.",
        R"({"type":"object","properties":{}})",
        "pw_session_connect"));

    registry.add(make_pw_tool(
        "pw_session_select",
        "Select a role from the current character list (character_select state only).",
        R"({"type":"object","properties":{"role_id":{"type":"integer","minimum":1}},"required":["role_id"]})",
        "pw_session_select"));

    registry.add(make_pw_tool(
        "pw_session_enter",
        "Enter the world with the currently selected role (typed, attested path).",
        R"({"type":"object","properties":{}})",
        "pw_session_enter"));

    registry.add(make_pw_tool(
        "pw_session_disconnect",
        "Disconnect the PW session (stops the sidecar) and return to the login screen.",
        R"({"type":"object","properties":{}})",
        "pw_session_disconnect"));
}

} // namespace unravel::mcp
