#include "mcp_tool_registry.h"

#include <editor/mcp/json.hpp>
#include <editor/mcp/mcp_commands.h>
#include <editor/mcp/mcp_system.h>
#include <editor/hub/hub.h>
#include <editor/hub/panels/scene_panel/scene_panel.h>

#include <engine/ecs/components/transform_component.h>
#include <engine/rendering/ecs/components/camera_component.h>

#include <exception>
#include <string>
#include <utility>

namespace unravel::mcp
{
namespace
{
using json = nlohmann::json;

void sync_pw_camera_to_scene_viewport(rtti::context& ctx, mcp_system& system)
{
    auto source = system.ensure_camera(ctx);
    auto target = ctx.get_cached<hub>().get_panels().get_scene_panel().get_camera();
    if(!source || !target ||
       !source.all_of<transform_component, camera_component>() ||
       !target.all_of<transform_component, camera_component>())
    {
        return;
    }

    const auto& source_transform = source.get<transform_component>();
    const auto& source_camera = source.get<camera_component>();
    auto& target_transform = target.get<transform_component>();
    auto& target_camera = target.get<camera_component>();

    target_transform.set_position_global(source_transform.get_position_global());
    target_transform.set_rotation_global(source_transform.get_rotation_global());
    target_camera.set_fov(source_camera.get_fov());
    target_camera.set_near_clip(source_camera.get_near_clip());
    target_camera.set_far_clip(source_camera.get_far_clip());
}

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
            sync_pw_camera_to_scene_viewport(ctx, system);
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
}

} // namespace unravel::mcp
