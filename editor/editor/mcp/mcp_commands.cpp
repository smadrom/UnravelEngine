#include "mcp_commands.h"

#include "json.hpp"
#include "mcp_system.h"

#include <engine/assets/asset_manager.h>
#include <engine/ecs/components/id_component.h>
#include <engine/ecs/components/tag_component.h>
#include <engine/ecs/components/transform_component.h>
#include <engine/ecs/ecs.h>
#include <engine/ecs/prefab.h>
#include <engine/rendering/ecs/components/camera_component.h>
#include <engine/rendering/ecs/components/light_component.h>
#include <engine/rendering/ecs/components/model_component.h>
#include <graphics/graphics.h>
#include <uuid/uuid.h>
#include <version/version.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace unravel
{
namespace
{
using json = nlohmann::json;

auto make_ping_result() -> json
{
    json result;
    result["pong"] = true;
    result["engine"] = "unravel";
    result["backend"] = gfx::get_renderer_name(gfx::get_renderer_type());
    result["version"] = version::get_full();
    return result;
}

auto make_scene_list(rtti::context& ctx) -> json
{
    auto& scn = ctx.get_cached<ecs>().get_scene();
    auto& reg = *scn.registry;

    json entities = json::array();
    auto view = reg.view<tag_component>();
    for(auto e : view)
    {
        const auto& tag = view.get<tag_component>(e);
        if(tag.name == "MCP Camera")
        {
            continue;
        }

        json components = json::array();
        const bool has_transform = reg.all_of<transform_component>(e);
        const bool has_model = reg.all_of<model_component>(e);
        const bool has_light = reg.all_of<light_component>(e);
        const bool has_camera = reg.all_of<camera_component>(e);

        if(has_transform)
        {
            components.push_back("transform");
        }
        if(has_model)
        {
            components.push_back("model");
        }
        if(has_light)
        {
            components.push_back("light");
        }
        if(has_camera)
        {
            components.push_back("camera");
        }

        json entity;
        entity["id"] = entt::to_integral(e);
        entity["uuid"] = reg.all_of<id_component>(e) ? hpp::to_string(reg.get<id_component>(e).id) : std::string{};
        entity["name"] = tag.name;
        entity["components"] = components;
        entity["flags"] = {
            {"transform", has_transform},
            {"model", has_model},
            {"light", has_light},
            {"camera", has_camera},
        };
        entities.push_back(entity);
    }

    return entities;
}

auto make_map_structure(rtti::context& ctx) -> json
{
    auto& scn = ctx.get_cached<ecs>().get_scene();
    auto entities = make_scene_list(ctx);

    json result;
    result["name"] = scn.tag;
    result["count"] = entities.size();
    result["entities"] = std::move(entities);
    return result;
}

auto vec3_to_json(const math::vec3& v) -> json
{
    return json::array({v.x, v.y, v.z});
}

auto read_vec3(const json& params, const char* key, math::vec3& out) -> bool
{
    if(!params.contains(key))
    {
        return false;
    }
    if(!params[key].is_array() || params[key].size() < 3)
    {
        throw std::runtime_error(std::string("camera_set: '") + key + "' must be [x,y,z]");
    }

    out.x = params[key][0].get<float>();
    out.y = params[key][1].get<float>();
    out.z = params[key][2].get<float>();
    return true;
}

auto make_camera_result(entt::handle camera) -> json
{
    auto& transform_comp = camera.get<transform_component>();
    auto& camera_comp = camera.get<camera_component>();

    json result;
    result["present"] = true;
    result["pos"] = vec3_to_json(transform_comp.get_position_local());
    result["rot"] = vec3_to_json(transform_comp.get_rotation_euler_local());
    result["fov"] = camera_comp.get_fov();
    result["near_clip"] = camera_comp.get_near_clip();
    result["far_clip"] = camera_comp.get_far_clip();

    const auto& viewport = camera_comp.get_viewport_size();
    result["viewport"] = json::array({viewport.width, viewport.height});
    return result;
}

auto make_camera_get_result(rtti::context& ctx, mcp_system& sys) -> json
{
    return make_camera_result(sys.ensure_camera(ctx));
}

auto make_camera_set_result(rtti::context& ctx, mcp_system& sys, const json& params) -> json
{
    auto camera = sys.ensure_camera(ctx);
    auto& transform_comp = camera.get<transform_component>();
    auto& camera_comp = camera.get<camera_component>();

    math::vec3 pos{};
    if(read_vec3(params, "pos", pos))
    {
        transform_comp.set_position_local(pos);
    }

    math::vec3 rot{};
    if(read_vec3(params, "rot", rot))
    {
        transform_comp.set_rotation_euler_local(rot);
    }

    if(params.contains("fov"))
    {
        camera_comp.set_fov(params["fov"].get<float>());
    }
    if(params.contains("near_clip"))
    {
        camera_comp.set_near_clip(params["near_clip"].get<float>());
    }
    if(params.contains("far_clip"))
    {
        camera_comp.set_far_clip(params["far_clip"].get<float>());
    }

    return make_camera_result(camera);
}

auto make_screenshot_result(mcp_system& sys, const json& params) -> json
{
    if(!params.contains("path") || !params["path"].is_string())
    {
        throw std::runtime_error("screenshot: 'path' (string) required");
    }

    const auto path = params["path"].get<std::string>();
    const int w = params.value("w", 1280);
    const int h = params.value("h", 720);
    if(path.empty())
    {
        throw std::runtime_error("screenshot: 'path' must not be empty");
    }
    if(w <= 0 || h <= 0)
    {
        throw std::runtime_error("screenshot: 'w' and 'h' must be positive");
    }

    sys.request_screenshot(path, static_cast<uint32_t>(w), static_cast<uint32_t>(h));

    json result;
    result["status"] = "pending";
    result["path"] = path;
    return result;
}

auto make_load_map_result(rtti::context& ctx, mcp_system& sys, const json& params) -> json
{
    if(!params.contains("path") || !params["path"].is_string())
    {
        throw std::runtime_error("load_map: 'path' (string) required");
    }

    const auto path = params["path"].get<std::string>();
    if(path.empty())
    {
        throw std::runtime_error("load_map: 'path' must not be empty");
    }

    auto& am = ctx.get_cached<asset_manager>();
    auto& scn = ctx.get_cached<ecs>().get_scene();
    auto asset = am.get_asset<scene_prefab>(path);
    auto prefab = asset.get();
    if(!prefab || prefab->buffer.data.empty())
    {
        throw std::runtime_error("load_map: failed to load '" + path + "'");
    }

    const bool loaded = scn.load_from(asset);
    if(!loaded)
    {
        throw std::runtime_error("load_map: failed to load '" + path + "'");
    }

    sys.invalidate_camera();

    json result;
    result["ok"] = true;
    result["path"] = path;
    return result;
}
} // namespace

namespace mcp_commands
{
auto dispatch(rtti::context& ctx, const std::string& req_json, mcp_system& sys) -> std::string
{
    json out;
    int seq = 0;
    try
    {
        json req = json::parse(req_json, nullptr, false);
        if(req.is_discarded() || !req.is_object())
        {
            throw std::runtime_error("malformed request JSON");
        }

        seq = req.value("seq", 0);
        const auto method = req.value("method", std::string{});
        json params = req.contains("params") && req["params"].is_object() ? req["params"] : json::object();
        (void)params;

        json result;
        if(method == "ping")
        {
            result = make_ping_result();
        }
        else if(method == "scene_list")
        {
            result = make_scene_list(ctx);
        }
        else if(method == "get_map_structure")
        {
            result = make_map_structure(ctx);
        }
        else if(method == "camera_get")
        {
            result = make_camera_get_result(ctx, sys);
        }
        else if(method == "camera_set")
        {
            result = make_camera_set_result(ctx, sys, params);
        }
        else if(method == "screenshot")
        {
            result = make_screenshot_result(sys, params);
        }
        else if(method == "load_map")
        {
            result = make_load_map_result(ctx, sys, params);
        }
        else
        {
            throw std::runtime_error("unknown method '" + method + "'");
        }

        out["seq"] = seq;
        out["ok"] = true;
        out["result"] = result;
    }
    catch(const std::exception& e)
    {
        out = json::object();
        out["seq"] = seq;
        out["ok"] = false;
        out["error"] = e.what();
    }

    return out.dump();
}
} // namespace mcp_commands

} // namespace unravel
