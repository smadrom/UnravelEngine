#include "mcp_commands.h"

#include "json.hpp"
#include "mcp_system.h"

#include <engine/defaults/defaults.h>
#include <engine/assets/asset_manager.h>
#include <engine/ecs/components/id_component.h>
#include <engine/ecs/components/tag_component.h>
#include <engine/ecs/components/transform_component.h>
#include <engine/ecs/ecs.h>
#include <engine/ecs/prefab.h>
#include <engine/rendering/ecs/components/camera_component.h>
#include <engine/rendering/ecs/components/light_component.h>
#include <engine/rendering/ecs/components/model_component.h>
#include <engine/rendering/ecs/components/reflection_probe_component.h>
#include <engine/rendering/ecs/components/volume_component.h>
#include <engine/rendering/material.h>
#include <engine/rendering/mesh.h>
#include <engine/rendering/model.h>
#include <graphics/graphics.h>
#include <uuid/uuid.h>
#include <version/version.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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
    const auto vec3_json = [](const math::vec3& v)
    {
        return json::array({v.x, v.y, v.z});
    };

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
        const bool has_skylight = reg.all_of<skylight_component>(e);
        const bool has_reflection_probe = reg.all_of<reflection_probe_component>(e);
        const bool has_volume = reg.all_of<volume_component>(e);
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
        if(has_skylight)
        {
            components.push_back("skylight");
        }
        if(has_reflection_probe)
        {
            components.push_back("reflection_probe");
        }
        if(has_volume)
        {
            components.push_back("volume");
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
            {"skylight", has_skylight},
            {"reflection_probe", has_reflection_probe},
            {"volume", has_volume},
            {"camera", has_camera},
        };
        if(has_transform)
        {
            const auto& transform = reg.get<transform_component>(e);
            entity["transform"] = {
                {"position_local", vec3_json(transform.get_position_local())},
                {"position_global", vec3_json(transform.get_position_global())},
                {"rotation_local", vec3_json(transform.get_rotation_euler_local())},
                {"scale_local", vec3_json(transform.get_scale_local())},
            };
        }
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

auto color_to_json(const math::color& color) -> json
{
    const math::vec4 value = color;
    return json::array({value.x, value.y, value.z, value.w});
}

auto bbox_to_json(const math::bbox& bounds) -> json
{
    json result;
    result["valid"] = bounds.is_populated();
    if(bounds.is_populated())
    {
        result["min"] = vec3_to_json(bounds.min);
        result["max"] = vec3_to_json(bounds.max);
        result["center"] = vec3_to_json(bounds.get_center());
        result["extents"] = vec3_to_json(bounds.get_extents());
    }
    return result;
}

auto cull_type_to_string(cull_type type) -> std::string
{
    switch(type)
    {
        case cull_type::none:
            return "none";
        case cull_type::clockwise:
            return "clockwise";
        case cull_type::counter_clockwise:
            return "counter_clockwise";
        default:
            return "unknown";
    }
}

auto mesh_status_to_string(mesh_status status) -> std::string
{
    switch(status)
    {
        case mesh_status::not_prepared:
            return "not_prepared";
        case mesh_status::preparing:
            return "preparing";
        case mesh_status::prepared:
            return "prepared";
        default:
            return "unknown";
    }
}

template<typename T>
auto asset_handle_to_json(const asset_handle<T>& asset) -> json
{
    json result;
    result["valid"] = asset.is_valid();
    result["ready"] = asset.is_ready();
    result["loading"] = asset.is_loading();
    result["deferred"] = asset.is_deferred();
    result["id"] = asset.id();
    result["uid"] = hpp::to_string(asset.uid());
    result["name"] = asset.name();
    result["extension"] = asset.extension();
    return result;
}

auto material_to_json(const material::sptr& material_instance, uint32_t index) -> json
{
    json result;
    result["index"] = index;
    result["present"] = static_cast<bool>(material_instance);
    if(!material_instance)
    {
        return result;
    }

    result["type"] = "material";
    result["cull_type"] = cull_type_to_string(material_instance->get_cull_type());

    auto pbr = std::dynamic_pointer_cast<pbr_material>(material_instance);
    if(pbr)
    {
        result["type"] = "pbr_material";
        result["base_color"] = color_to_json(pbr->get_base_color());
        result["roughness"] = pbr->get_roughness();
        result["metalness"] = pbr->get_metalness();
        result["textures"] = {
            {"color", asset_handle_to_json(pbr->get_color_map())},
            {"normal", asset_handle_to_json(pbr->get_normal_map())},
            {"roughness", asset_handle_to_json(pbr->get_roughness_map())},
            {"metalness", asset_handle_to_json(pbr->get_metalness_map())},
        };
    }

    return result;
}

auto mesh_asset_to_json(const asset_handle<mesh>& mesh_asset, uint32_t lod_index, size_t submesh_limit) -> json
{
    json result = asset_handle_to_json(mesh_asset);
    result["lod_index"] = lod_index;
    result["loaded"] = false;

    auto mesh_instance = mesh_asset.get(false);
    if(!mesh_instance)
    {
        return result;
    }

    const auto info = mesh_instance->get_info();
    result["loaded"] = true;
    result["status"] = mesh_status_to_string(mesh_instance->get_status());
    result["vertices"] = info.vertices;
    result["triangles"] = info.triangles;
    result["submeshes"] = info.submeshes;
    result["data_groups"] = info.data_groups;
    result["lod_count"] = mesh_instance->get_lod_count();
    result["bounds"] = bbox_to_json(mesh_instance->get_bounds());

    json internal_lods = json::array();
    for(size_t i = 0; i < info.lods.size(); ++i)
    {
        internal_lods.push_back({
            {"index", i + 1u},
            {"triangles", info.lods[i].triangles},
            {"percent", info.lods[i].percent},
        });
    }
    result["internal_lods"] = std::move(internal_lods);

    json submeshes = json::array();
    const size_t count = std::min(mesh_instance->get_submeshes_count(0), submesh_limit);
    for(size_t i = 0; i < count; ++i)
    {
        const auto* submesh = mesh_instance->get_submesh(static_cast<uint32_t>(i), 0);
        if(!submesh)
        {
            continue;
        }

        submeshes.push_back({
            {"index", i},
            {"data_group_id", submesh->data_group_id},
            {"vertex_start", submesh->vertex_start},
            {"vertex_count", submesh->vertex_count},
            {"face_start", submesh->face_start},
            {"face_count", submesh->face_count},
            {"skinned", submesh->skinned},
            {"bounds", bbox_to_json(submesh->bbox)},
        });
    }
    result["submesh_sample_count"] = submeshes.size();
    result["submesh_sample_limit"] = submesh_limit;
    result["submesh_sample"] = std::move(submeshes);
    return result;
}

auto transform_to_json(const transform_component& transform) -> json
{
    return {
        {"position_local", vec3_to_json(transform.get_position_local())},
        {"position_global", vec3_to_json(transform.get_position_global())},
        {"rotation_local", vec3_to_json(transform.get_rotation_euler_local())},
        {"rotation_global", vec3_to_json(transform.get_rotation_euler_global())},
        {"scale_local", vec3_to_json(transform.get_scale_local())},
        {"scale_global", vec3_to_json(transform.get_scale_global())},
    };
}

auto component_flags_json(entt::registry& reg, entt::entity e) -> json
{
    return {
        {"transform", reg.all_of<transform_component>(e)},
        {"model", reg.all_of<model_component>(e)},
        {"light", reg.all_of<light_component>(e)},
        {"skylight", reg.all_of<skylight_component>(e)},
        {"reflection_probe", reg.all_of<reflection_probe_component>(e)},
        {"volume", reg.all_of<volume_component>(e)},
        {"camera", reg.all_of<camera_component>(e)},
    };
}

auto component_names_json(const json& flags) -> json
{
    json components = json::array();
    for(const auto& item : flags.items())
    {
        if(item.value().get<bool>())
        {
            components.push_back(item.key());
        }
    }
    return components;
}

auto model_to_json(const model_component& model_comp, bool include_materials, size_t submesh_limit) -> json
{
    const auto& render_model = model_comp.get_model();

    json result;
    result["valid"] = render_model.is_valid();
    result["enabled"] = model_comp.is_enabled();
    result["casts_shadow"] = model_comp.casts_shadow();
    result["static"] = model_comp.is_static();
    result["skinned"] = model_comp.is_skinned();
    result["lod_count"] = render_model.get_lods_count();
    result["lod_asset_count"] = render_model.get_lods().size();
    result["material_instance_count"] = render_model.get_material_instances().size();

    json lods = json::array();
    const uint32_t lod_count = render_model.get_lods_count();
    for(uint32_t i = 0; i < lod_count && i < 16u; ++i)
    {
        lods.push_back(mesh_asset_to_json(render_model.get_lod(i), i, submesh_limit));
    }
    result["lods"] = std::move(lods);

    if(include_materials)
    {
        json materials = json::array();
        const size_t material_count = std::min(render_model.get_material_instances().size(), size_t{64});
        for(size_t i = 0; i < material_count; ++i)
        {
            materials.push_back(material_to_json(render_model.get_material_instance(static_cast<uint32_t>(i)),
                                                static_cast<uint32_t>(i)));
        }
        result["materials"] = std::move(materials);
    }

    return result;
}

auto entity_to_detail_json(rtti::context& ctx,
                           entt::registry& reg,
                           entt::entity e,
                           bool include_materials,
                           bool include_bounds,
                           size_t submesh_limit) -> json
{
    json entity;
    entity["id"] = entt::to_integral(e);
    entity["uuid"] = reg.all_of<id_component>(e) ? hpp::to_string(reg.get<id_component>(e).id) : std::string{};
    entity["name"] = reg.all_of<tag_component>(e) ? reg.get<tag_component>(e).name : std::string{};

    const auto flags = component_flags_json(reg, e);
    entity["flags"] = flags;
    entity["components"] = component_names_json(flags);

    if(reg.all_of<transform_component>(e))
    {
        entity["transform"] = transform_to_json(reg.get<transform_component>(e));
    }

    if(include_bounds)
    {
        entity["bounds_global"] = bbox_to_json(defaults::calc_bounds_global(entt::handle{reg, e}));
    }

    if(reg.all_of<model_component>(e))
    {
        entity["model"] = model_to_json(reg.get<model_component>(e), include_materials, submesh_limit);
    }

    (void)ctx;
    return entity;
}

auto resolve_entities(entt::registry& reg, const json& params) -> std::vector<entt::entity>
{
    std::vector<entt::entity> result;

    if(params.contains("id"))
    {
        const auto raw_id = params["id"].get<uint32_t>();
        const auto entity = static_cast<entt::entity>(raw_id);
        if(reg.valid(entity))
        {
            result.push_back(entity);
        }
        return result;
    }

    if(params.contains("uuid") && params["uuid"].is_string())
    {
        const auto wanted = params["uuid"].get<std::string>();
        auto view = reg.view<id_component>();
        for(auto e : view)
        {
            if(hpp::to_string(view.get<id_component>(e).id) == wanted)
            {
                result.push_back(e);
            }
        }
        return result;
    }

    if(params.contains("name") && params["name"].is_string())
    {
        const auto wanted = params["name"].get<std::string>();
        auto view = reg.view<tag_component>();
        for(auto e : view)
        {
            if(view.get<tag_component>(e).name == wanted)
            {
                result.push_back(e);
            }
        }
        return result;
    }

    auto view = reg.view<tag_component>();
    for(auto e : view)
    {
        result.push_back(e);
    }
    return result;
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

    math::vec3 target{};
    if(read_vec3(params, "target", target))
    {
        transform_comp.look_at(target, {0.0f, 1.0f, 0.0f});
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

auto make_screenshot_status_result(const mcp_system::screenshot_status& status) -> json
{
    json result;
    result["status"] = status.status;
    result["path"] = status.path;
    result["w"] = status.w;
    result["h"] = status.h;
    result["frames_left"] = status.frames_left;
    result["readback_frames_left"] = status.readback_frames_left;
    result["active"] = status.active;
    result["readback_started"] = status.readback_started;
    result["completed"] = status.completed;
    result["request_id"] = status.request_id;
    if(!status.error.empty())
    {
        result["error"] = status.error;
    }
    return result;
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
    return make_screenshot_status_result(sys.get_screenshot_status());
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

auto make_login_status_result(const mcp_system::login_load_status& status) -> json
{
    json result;
    result["status"] = status.status;
    result["content_root"] = status.content_root;
    result["done"] = status.done;
    result["total"] = status.total;
    result["created"] = status.created;
    result["skipped"] = status.skipped;
    result["terrain"] = status.terrain;
    result["current_index"] = status.current_index;
    if(status.has_current)
    {
        result["current"] = {
            {"index", status.current_index},
            {"attempts", status.current_attempts},
            {"name", status.current_name},
            {"model", status.current_model},
            {"texture", status.current_texture},
            {"position", vec3_to_json(status.current_position)},
        };
    }
    if(!status.error.empty())
    {
        result["error"] = status.error;
    }
    return result;
}

auto make_load_login_result(mcp_system& sys, const json& params) -> json
{
    std::string content_root = "app:/data/login";
    if(params.contains("content_root") && params["content_root"].is_string())
    {
        content_root = params["content_root"].get<std::string>();
    }
    else if(params.contains("content-root") && params["content-root"].is_string())
    {
        content_root = params["content-root"].get<std::string>();
    }

    const int chunk_size = params.value("chunk_size", 3);
    if(chunk_size <= 0)
    {
        throw std::runtime_error("load_login: 'chunk_size' must be positive");
    }

    const bool restart = params.value("restart", false);
    sys.start_login_load(content_root, static_cast<uint32_t>(chunk_size), restart);
    return make_login_status_result(sys.get_login_load_status());
}

auto make_terrain_probe_result(mcp_system& sys, const json& params) -> json
{
    struct sample_request
    {
        float x = 0.0f;
        float z = 0.0f;
        bool has_y = false;
        float y = 0.0f;
    };

    std::vector<sample_request> requests;
    if(params.contains("points") && params["points"].is_array())
    {
        for(const auto& point : params["points"])
        {
            if(point.is_object())
            {
                if(!point.contains("x") || !point.contains("z"))
                {
                    throw std::runtime_error("terrain_probe: each object point must include 'x' and 'z'");
                }

                sample_request request;
                request.x = point["x"].get<float>();
                request.z = point["z"].get<float>();
                if(point.contains("y"))
                {
                    request.has_y = true;
                    request.y = point["y"].get<float>();
                }
                requests.push_back(request);
                continue;
            }

            if(!point.is_array() || point.size() < 2)
            {
                throw std::runtime_error("terrain_probe: each point must be [x,z], [x,y,z], or an object with 'x' and 'z'");
            }

            sample_request request;
            request.x = point[0].get<float>();
            if(point.size() >= 3)
            {
                request.has_y = true;
                request.y = point[1].get<float>();
                request.z = point[2].get<float>();
            }
            else
            {
                request.z = point[1].get<float>();
            }
            requests.push_back(request);
        }
    }
    else if(params.contains("pos") && params["pos"].is_array() && params["pos"].size() >= 3)
    {
        requests.push_back({params["pos"][0].get<float>(), params["pos"][2].get<float>(), true, params["pos"][1].get<float>()});
    }
    else if(params.contains("x") && params.contains("z"))
    {
        sample_request request;
        request.x = params["x"].get<float>();
        request.z = params["z"].get<float>();
        if(params.contains("y"))
        {
            request.has_y = true;
            request.y = params["y"].get<float>();
        }
        requests.push_back(request);
    }
    else
    {
        throw std::runtime_error("terrain_probe: provide 'x' and 'z', 'pos':[x,y,z], or 'points'");
    }

    const auto& terrain = sys.get_login_terrain();
    json result;
    result["heightfield_valid"] = terrain.is_valid();
    result["width"] = terrain.width;
    result["height"] = terrain.height;
    result["height_min"] = terrain.height_min;
    result["height_max"] = terrain.height_max;
    result["world_width"] = terrain.world_width;
    result["world_depth"] = terrain.world_depth;
    result["login_status"] = make_login_status_result(sys.get_login_load_status());

    json samples = json::array();
    for(const auto& request : requests)
    {
        float sampled = 0.0f;
        const bool ok = sys.sample_login_terrain(request.x, request.z, sampled);

        json sample;
        sample["x"] = request.x;
        sample["z"] = request.z;
        sample["ok"] = ok;
        if(ok)
        {
            sample["height"] = sampled;
            if(request.has_y)
            {
                sample["delta_y"] = request.y - sampled;
            }
        }
        samples.push_back(std::move(sample));
    }
    result["samples"] = std::move(samples);
    return result;
}

auto make_scene_detail_result(rtti::context& ctx, const json& params) -> json
{
    auto& scn = ctx.get_cached<ecs>().get_scene();
    auto& reg = *scn.registry;

    const int raw_limit = params.value("limit", 128);
    const size_t limit = static_cast<size_t>(std::clamp(raw_limit, 1, 1000));
    const int raw_submesh_limit = params.value("submesh_limit", 8);
    const size_t submesh_limit = static_cast<size_t>(std::clamp(raw_submesh_limit, 0, 128));
    const bool include_materials = params.value("include_materials", true);
    const bool include_bounds = params.value("include_bounds", true);
    const bool model_only = params.value("model_only", false);

    const auto matches = resolve_entities(reg, params);
    json entities = json::array();
    size_t matched_after_filter = 0;
    for(auto e : matches)
    {
        if(model_only && !reg.all_of<model_component>(e))
        {
            continue;
        }

        ++matched_after_filter;
        if(entities.size() >= limit)
        {
            continue;
        }

        entities.push_back(entity_to_detail_json(ctx, reg, e, include_materials, include_bounds, submesh_limit));
    }

    json result;
    result["scene"] = scn.tag;
    result["matched"] = matched_after_filter;
    result["returned"] = entities.size();
    result["limit"] = limit;
    result["entities"] = std::move(entities);
    return result;
}

auto make_log_tail_result(const json& params) -> json
{
    auto path = params.value("path", std::string{"Log.txt"});
    const int raw_max_bytes = params.value("max_bytes", 32768);
    const std::streamoff max_bytes = static_cast<std::streamoff>(std::clamp(raw_max_bytes, 1, 1024 * 1024));

    std::ifstream file(path, std::ios::binary);
    json result;
    result["path"] = std::filesystem::absolute(path).string();
    result["exists"] = static_cast<bool>(file);
    if(!file)
    {
        result["tail"] = "";
        result["bytes"] = 0;
        return result;
    }

    file.seekg(0, std::ios::end);
    const auto end = file.tellg();
    const auto size = end < 0 ? std::streamoff{0} : static_cast<std::streamoff>(end);
    const auto start = size > max_bytes ? size - max_bytes : std::streamoff{0};
    file.seekg(start, std::ios::beg);

    std::string tail(static_cast<size_t>(size - start), '\0');
    file.read(tail.data(), static_cast<std::streamsize>(tail.size()));
    tail.resize(static_cast<size_t>(file.gcount()));

    result["bytes"] = tail.size();
    result["truncated"] = start > 0;
    result["tail"] = std::move(tail);
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
        else if(method == "scene_detail")
        {
            result = make_scene_detail_result(ctx, params);
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
        else if(method == "screenshot_status")
        {
            result = make_screenshot_status_result(sys.get_screenshot_status());
        }
        else if(method == "load_map")
        {
            result = make_load_map_result(ctx, sys, params);
        }
        else if(method == "load_login")
        {
            result = make_load_login_result(sys, params);
        }
        else if(method == "login_status")
        {
            result = make_login_status_result(sys.get_login_load_status());
        }
        else if(method == "terrain_probe")
        {
            result = make_terrain_probe_result(sys, params);
        }
        else if(method == "log_tail")
        {
            result = make_log_tail_result(params);
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
