#include "mcp_system.h"

#include "mcp_commands.h"

#include "json.hpp"

#include <engine/assets/asset_manager.h>
#include <engine/defaults/defaults.h>
#include <engine/ecs/components/tag_component.h>
#include <engine/ecs/components/transform_component.h>
#include <engine/ecs/ecs.h>
#include <engine/ecs/scene.h>
#include <engine/events.h>
#include <engine/rendering/ecs/components/camera_component.h>
#include <engine/rendering/ecs/components/light_component.h>
#include <engine/rendering/ecs/components/model_component.h>
#include <engine/rendering/ecs/components/reflection_probe_component.h>
#include <engine/rendering/ecs/systems/rendering_system.h>
#include <engine/rendering/material.h>
#include <engine/rendering/mesh.h>
#include <engine/rendering/model.h>
#include <filesystem/filesystem.h>
#include <graphics/render_pass.h>
#include <graphics/texture.h>
#include <graphics/utils/bgfx_utils.h>
#include <graphics/vertex_decl.h>
#include <logging/logging.h>

#include <bimg/encode.h>
#include <bx/file.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace unravel
{
namespace
{
constexpr auto kMcpFrameDt = delta_t(0.016667f);
constexpr uint32_t kLoginMaxAssetWaitFrames = 900;
constexpr float kDuplicatePositionEpsilon = 0.01f;

using json = nlohmann::json;

auto normalize_content_root(std::string root) -> std::string
{
    if(root.empty())
    {
        root = "app:/data/login";
    }

    while(!root.empty() && (root.back() == '/' || root.back() == '\\'))
    {
        root.pop_back();
    }
    return root;
}

auto make_asset_key(const std::string& content_root, std::string relative) -> std::string
{
    std::replace(relative.begin(), relative.end(), '\\', '/');
    while(!relative.empty() && relative.front() == '/')
    {
        relative.erase(relative.begin());
    }
    return normalize_content_root(content_root) + "/" + relative;
}

auto read_json_asset(const std::string& asset_key) -> json
{
    const auto path = fs::resolve_protocol(asset_key);
    std::ifstream file(path);
    if(!file)
    {
        throw std::runtime_error("load_login: failed to open '" + asset_key + "'");
    }

    auto doc = json::parse(file, nullptr, false);
    if(doc.is_discarded())
    {
        throw std::runtime_error("load_login: failed to parse '" + asset_key + "'");
    }
    return doc;
}

auto read_material_texture_ref(const std::string& content_root, const std::string& material_ref) -> std::string
{
    const auto material_doc = read_json_asset(make_asset_key(content_root, material_ref));
    std::string texture_ref = material_doc.value("textureRef", std::string{});
    if(texture_ref.empty() && material_doc.contains("params") && material_doc["params"].is_object())
    {
        texture_ref = material_doc["params"].value("diffuseTextureRef", std::string{});
    }
    if(texture_ref.empty())
    {
        throw std::runtime_error("load_login: material '" + material_ref + "' has no textureRef");
    }
    return texture_ref;
}

auto read_vec3_member(const json& item, const char* key) -> math::vec3
{
    if(!item.contains(key) || !item[key].is_array() || item[key].size() < 3)
    {
        throw std::runtime_error(std::string("load_login: building has no valid '") + key + "'");
    }

    return {item[key][0].get<float>(), item[key][1].get<float>(), item[key][2].get<float>()};
}

auto map_source_position_to_unravel(const math::vec3& source_pos) -> math::vec3
{
    return {source_pos.x, source_pos.y, -source_pos.z};
}

auto map_source_forward_to_unravel(const math::vec3& source_dir) -> math::vec3
{
    math::vec3 forward{source_dir.x, source_dir.y, -source_dir.z};
    if(math::dot(forward, forward) <= 0.000001f)
    {
        return {0.0f, 0.0f, 1.0f};
    }

    return math::normalize(forward);
}

auto decode_r16_heightmap(const std::string& heightmap_key, uint32_t& width, uint32_t& height) -> std::vector<float>
{
    const auto resolved = fs::resolve_protocol(heightmap_key).string();
    auto* image = imageLoad(bx::FilePath(resolved.c_str()), bgfx::TextureFormat::Count);
    if(image == nullptr || image->m_data == nullptr)
    {
        throw std::runtime_error("load_login: failed to decode '" + heightmap_key + "'");
    }

    width = image->m_width;
    height = image->m_height;
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<float> heights(count);

    switch(image->m_format)
    {
        case bimg::TextureFormat::R16:
        {
            const auto* src = static_cast<const uint16_t*>(image->m_data);
            for(size_t i = 0; i < count; ++i)
            {
                heights[i] = static_cast<float>(src[i]) / 65535.0f;
            }
            break;
        }
        case bimg::TextureFormat::RGBA16:
        {
            const auto* src = static_cast<const uint16_t*>(image->m_data);
            for(size_t i = 0; i < count; ++i)
            {
                heights[i] = static_cast<float>(src[i * 4]) / 65535.0f;
            }
            break;
        }
        case bimg::TextureFormat::R8:
        {
            const auto* src = static_cast<const uint8_t*>(image->m_data);
            for(size_t i = 0; i < count; ++i)
            {
                heights[i] = static_cast<float>(src[i]) / 255.0f;
            }
            break;
        }
        case bimg::TextureFormat::RGBA8:
        {
            const auto* src = static_cast<const uint8_t*>(image->m_data);
            for(size_t i = 0; i < count; ++i)
            {
                heights[i] = static_cast<float>(src[i * 4]) / 255.0f;
            }
            break;
        }
        default:
            bimg::imageFree(image);
            throw std::runtime_error("load_login: unsupported heightmap format for '" + heightmap_key + "'");
    }

    bimg::imageFree(image);
    return heights;
}

auto load_login_terrain_heightfield(const std::string& content_root) -> terrain_heightfield
{
    const auto layers_doc = read_json_asset(make_asset_key(content_root, "terrain/login/layers.json"));
    const auto heightmap_doc = layers_doc.contains("heightmap") && layers_doc["heightmap"].is_object()
                                   ? layers_doc["heightmap"]
                                   : json::object();

    const auto heightmap_ref = heightmap_doc.value("path", std::string("terrain/login/height.r16.png"));

    terrain_heightfield terrain;
    terrain.height_min = heightmap_doc.value("heightMin", 134.59677124023438f);
    terrain.height_max = heightmap_doc.value("heightMax", 424.2374267578125f);
    terrain.world_width = layers_doc.contains("world") && layers_doc["world"].is_object()
                              ? layers_doc["world"].value("widthM", 1024.0f)
                              : 1024.0f;
    terrain.world_depth = layers_doc.contains("world") && layers_doc["world"].is_object()
                              ? layers_doc["world"].value("depthM", 1024.0f)
                              : 1024.0f;
    terrain.heights = decode_r16_heightmap(make_asset_key(content_root, heightmap_ref), terrain.width, terrain.height);
    if(!terrain.is_valid())
    {
        throw std::runtime_error("load_login: heightmap is too small or invalid");
    }

    return terrain;
}

struct login_buildings_parse_result
{
    std::vector<mcp_system::login_building> buildings;
    uint32_t duplicate_skipped = 0;
};

auto positions_match(const math::vec3& lhs, const math::vec3& rhs) -> bool
{
    const auto delta = lhs - rhs;
    return math::dot(delta, delta) <= kDuplicatePositionEpsilon * kDuplicatePositionEpsilon;
}

auto make_login_buildings(const std::string& content_root, const terrain_heightfield& terrain) -> login_buildings_parse_result
{
    const auto scene_doc = read_json_asset(make_asset_key(content_root, "maps/login/scene.eds.json"));
    if(!scene_doc.contains("buildings") || !scene_doc["buildings"].is_array())
    {
        throw std::runtime_error("load_login: scene.eds.json has no buildings[]");
    }

    login_buildings_parse_result result;
    std::vector<math::vec3> accepted_positions;
    for(const auto& item : scene_doc["buildings"])
    {
        if(!item.contains("openFormat") || !item["openFormat"].is_object())
        {
            continue;
        }

        const auto& open_format = item["openFormat"];
        if(!open_format.value("convertedToOpenFormat", false))
        {
            continue;
        }

        const auto model_ref = open_format.value("model", std::string{});
        const auto material_ref = open_format.value("material", std::string{});
        if(model_ref.empty() || material_ref.empty())
        {
            continue;
        }

        const auto source_position = read_vec3_member(item, "pos");
        const auto mapped_position = map_source_position_to_unravel(source_position);
        const bool duplicate = std::any_of(accepted_positions.begin(),
                                           accepted_positions.end(),
                                           [&](const math::vec3& accepted)
                                           {
                                               return positions_match(accepted, mapped_position);
                                           });
        if(duplicate)
        {
            ++result.duplicate_skipped;
            continue;
        }

        mcp_system::login_building building;
        building.name = item.value("name", std::string("Login Building ") + std::to_string(result.buildings.size()));
        building.model = model_ref;
        building.texture = read_material_texture_ref(content_root, material_ref);
        building.position = mapped_position;
        building.forward = map_source_forward_to_unravel(read_vec3_member(item, "dir"));
        building.source_position_y = source_position.y;
        building.terrain_sample_valid =
            terrain.sample_terrain_height(building.position.x, building.position.z, building.terrain_surface_y);
        if(building.terrain_sample_valid)
        {
            building.position.y = building.terrain_surface_y;
        }
        accepted_positions.emplace_back(mapped_position);
        result.buildings.emplace_back(std::move(building));
    }

    APPLOG_INFO("load_login parsed buildings: placed={} skipped={}",
                result.buildings.size(),
                result.duplicate_skipped);

    return result;
}

void create_login_terrain(rtti::context& ctx, const terrain_heightfield& terrain)
{
    auto terrain_mesh = std::make_shared<mesh>();
    const bool created = terrain_mesh->create_heightfield(gfx::mesh_vertex::get_layout(),
                                                          terrain.heights,
                                                          terrain.width - 1,
                                                          terrain.height - 1,
                                                          terrain.world_width * 0.5f,
                                                          terrain.world_depth * 0.5f,
                                                          terrain.heightfield_mesh_scale(),
                                                          mesh_create_origin::center,
                                                          true);
    if(!created)
    {
        throw std::runtime_error("load_login: failed to create terrain heightfield");
    }

    auto& am = ctx.get_cached<asset_manager>();
    auto terrain_handle = am.get_asset_from_instance<mesh>("app:/generated/login_terrain", terrain_mesh);

    auto material_instance = std::make_shared<pbr_material>();
    material_instance->set_base_color({1.0f, 1.0f, 1.0f, 1.0f});
    material_instance->set_metalness(0.0f);
    material_instance->set_roughness(0.85f);
    material_instance->set_cull_type(cull_type::none);

    model terrain_model;
    terrain_model.set_lod(terrain_handle, 0);
    if(auto terrain = terrain_handle.get())
    {
        const auto submeshes = terrain->get_submeshes_count(0);
        for(uint32_t i = 0; i < submeshes; ++i)
        {
            terrain_model.set_material_instance(material_instance, i);
        }
    }

    auto& scn = ctx.get_cached<ecs>().get_scene();
    auto entity = scene::create_entity(*scn.registry, "Login Terrain");
    entity.get<transform_component>().set_position_local({0.0f, terrain.heightfield_entity_y(), 0.0f});
    entity.emplace<model_component>().set_model(terrain_model);
}

auto scene_has_entity_named(scene& scn, const std::string& name) -> bool
{
    auto view = scn.registry->view<tag_component>();
    for(auto e : view)
    {
        if(view.get<tag_component>(e).name == name)
        {
            return true;
        }
    }

    return false;
}

void create_login_environment(rtti::context& ctx)
{
    auto& scn = ctx.get_cached<ecs>().get_scene();

    if(!scene_has_entity_named(scn, "Volume"))
    {
        defaults::create_volume_entity(ctx, scn, "Volume", volume_mode::global);
    }

    if(!scene_has_entity_named(scn, "Sun Light"))
    {
        auto sun = defaults::create_light_entity(ctx, scn, light_type::directional, "Sun");

        auto& transform = sun.get<transform_component>();
        transform.set_rotation_euler_local({50.0f, -30.0f, 0.0f});

        auto& skylight = sun.get_or_emplace<skylight_component>();
        skylight.set_cloud_mode(skylight_component::cloud_mode::none);
        skylight.set_irradiance_intensity(0.20f);
    }

    if(!scene_has_entity_named(scn, "Reflection Probe Global"))
    {
        auto probe_entity = defaults::create_reflection_probe_entity(ctx, scn, probe_type::sphere, " Global");
        auto& reflection_comp = probe_entity.get_or_emplace<reflection_probe_component>();
        auto probe = reflection_comp.get_probe();
        probe.method = reflect_method::environment;
        probe.sphere_data.range = 1600.0f;
        reflection_comp.set_probe(probe);
    }
}

auto create_login_building(rtti::context& ctx,
                           const std::string& content_root,
                           mcp_system::login_building& building,
                           bool allow_untextured) -> bool
{
    auto& am = ctx.get_cached<asset_manager>();
    const auto mesh_key = make_asset_key(content_root, building.model);
    const auto texture_key = make_asset_key(content_root, building.texture);
    const auto flags = load_flags::standard;

    auto mesh_handle = am.get_asset<mesh>(mesh_key, flags);
    mesh_handle.submit();
    if(!mesh_handle.is_ready())
    {
        ++building.attempts;
        return false;
    }

    auto mesh_instance = mesh_handle.get(false);
    if(!mesh_instance || mesh_instance->get_submeshes_count(0) == 0)
    {
        ++building.attempts;
        return false;
    }

    auto texture_handle = am.get_asset<gfx::texture>(texture_key, flags);
    texture_handle.submit();
    if(!building.texture_request_logged)
    {
        APPLOG_INFO("load_login texture requested: building='{}' key='{}' handle_valid={} ready={} loading={}",
                    building.name,
                    texture_key,
                    texture_handle.is_valid(),
                    texture_handle.is_ready(),
                    texture_handle.is_loading());
        building.texture_request_logged = true;
    }

    auto texture_instance = texture_handle.is_ready() ? texture_handle.get(false) : std::shared_ptr<gfx::texture>{};
    const bool texture_loaded = static_cast<bool>(texture_instance);
    const bool native_valid = texture_instance && texture_instance->is_valid();
    const auto native_idx = native_valid ? texture_instance->native_handle().idx : bgfx::kInvalidHandle;
    const auto width = texture_instance ? texture_instance->info.width : 0;
    const auto height = texture_instance ? texture_instance->info.height : 0;
    const auto format = texture_instance ? static_cast<int>(texture_instance->info.format) : -1;
    const bool texture_usable = native_valid && width > 0 && height > 0;

    if(!building.texture_ready_logged && (texture_handle.is_ready() || allow_untextured))
    {
        APPLOG_INFO("load_login texture resolved: building='{}' key='{}' handle_valid={} ready={} loaded={} "
                    "native_valid={} native_idx={} width={} height={} format={} usable={}",
                    building.name,
                    texture_key,
                    texture_handle.is_valid(),
                    texture_handle.is_ready(),
                    texture_loaded,
                    native_valid,
                    native_idx,
                    width,
                    height,
                    format,
                    texture_usable);
        building.texture_ready_logged = true;
    }

    if(!allow_untextured && !texture_usable)
    {
        ++building.attempts;
        return false;
    }

    auto material_instance = std::make_shared<pbr_material>();
    material_instance->set_base_color({1.0f, 1.0f, 1.0f, 1.0f});
    material_instance->set_metalness(0.0f);
    material_instance->set_roughness(0.85f);
    material_instance->set_cull_type(cull_type::none);
    if(texture_usable)
    {
        material_instance->set_color_map(texture_handle);
    }

    model building_model;
    building_model.set_lod(mesh_handle, 0);

    const auto submeshes = mesh_instance->get_submeshes_count(0);
    for(size_t i = 0; i < submeshes; ++i)
    {
        building_model.set_material_instance(material_instance, static_cast<uint32_t>(i));
    }

    auto& scn = ctx.get_cached<ecs>().get_scene();
    auto entity = scene::create_entity(*scn.registry, building.name);
    auto& transform = entity.get<transform_component>();
    auto position = building.position;
    const auto& local_bounds = mesh_instance->get_bounds();
    building.local_min_y = local_bounds.is_populated() && std::isfinite(local_bounds.min.y) ? local_bounds.min.y : 0.0f;
    if(building.terrain_sample_valid)
    {
        position.y = building.terrain_surface_y - building.local_min_y;
    }
    else
    {
        position.y = building.source_position_y - building.local_min_y;
    }
    const float base_y = position.y + building.local_min_y;

    if(!building.placement_logged)
    {
        APPLOG_INFO("load_login placement: building='{}' x={} z={} terrain_surface_y={} final_y={} source_y={} "
                    "local_min_y={} base_y={} sample_valid={}",
                    building.name,
                    position.x,
                    position.z,
                    building.terrain_surface_y,
                    position.y,
                    building.source_position_y,
                    building.local_min_y,
                    base_y,
                    building.terrain_sample_valid);
        building.placement_logged = true;
    }

    transform.set_position_local(position);
    transform.look_at(position + building.forward, {0.0f, 1.0f, 0.0f});
    entity.emplace<model_component>().set_model(building_model);
    return true;
}
}

auto mcp_system::init(rtti::context& ctx) -> bool
{
    auto& ev = ctx.get_cached<events>();
    ev.on_frame_end.connect(sentinel_, -1000, this, &mcp_system::on_frame_end);

    const char* port_env = std::getenv("PW_MCP_PORT");
    if(port_env != nullptr)
    {
        server_.Start(std::atoi(port_env));
    }

    return true;
}

auto mcp_system::deinit(rtti::context& ctx) -> bool
{
    (void)ctx;
    server_.Stop();
    clear_pending_readback();
    return true;
}

void mcp_system::on_frame_end(rtti::context& ctx, delta_t dt)
{
    (void)dt;
    service_login_loader(ctx);
    service_pending_screenshot(ctx);
    server_.Drain([&](const std::string& req)
    {
        return mcp_commands::dispatch(ctx, req, *this);
    });
}

auto mcp_system::ensure_camera(rtti::context& ctx) -> entt::handle
{
    auto& scn = ctx.get_cached<ecs>().get_scene();
    const bool camera_valid = mcp_cam_.valid() && mcp_cam_.all_of<transform_component, camera_component>();
    if(!camera_valid)
    {
        mcp_cam_ = defaults::create_camera_entity(ctx, scn, "MCP Camera");
    }

    return mcp_cam_;
}

void mcp_system::invalidate_camera()
{
    mcp_cam_ = {};
}

void mcp_system::request_screenshot(const std::string& path, uint32_t w, uint32_t h)
{
    clear_pending_readback();
    pending_.path = path;
    pending_.w = w;
    pending_.h = h;
    pending_.frames_left = 2;
    pending_.readback_frames_left = 0;
    pending_.active = true;
    pending_.readback_started = false;
    pending_.pixels.clear();
}

void mcp_system::start_login_load(const std::string& content_root, uint32_t buildings_per_frame, bool restart)
{
    const auto normalized_root = normalize_content_root(content_root);
    if(!restart && (login_.active || login_.completed) && login_.content_root == normalized_root)
    {
        return;
    }
    if(login_.active && login_.content_root != normalized_root)
    {
        throw std::runtime_error("load_login: another content_root is already loading");
    }

    login_ = {};
    login_.content_root = normalized_root;
    login_.buildings_per_frame = std::clamp(buildings_per_frame, 1u, 8u);
    login_.terrain = load_login_terrain_heightfield(login_.content_root);
    auto parsed = make_login_buildings(login_.content_root, login_.terrain);
    login_.skipped = parsed.duplicate_skipped;
    login_.buildings = std::move(parsed.buildings);
    login_.active = true;
    login_.completed = false;
    login_.status = "loading";
}

auto mcp_system::get_login_load_status() const -> login_load_status
{
    login_load_status result;
    result.status = login_.status;
    result.content_root = login_.content_root;
    result.error = login_.error;
    result.done = login_.cursor;
    result.total = static_cast<uint32_t>(login_.buildings.size());
    result.created = login_.created;
    result.skipped = login_.skipped;
    result.terrain = login_.terrain_created;

    if(!login_.error.empty())
    {
        result.status = "error";
    }
    else if(login_.completed)
    {
        result.status = "done";
    }
    else if(!login_.active && login_.buildings.empty())
    {
        result.status = "idle";
    }

    return result;
}

void mcp_system::service_login_loader(rtti::context& ctx)
{
    if(!login_.active)
    {
        return;
    }

    try
    {
        uint32_t processed_this_frame = 0;
        login_.status = "loading";

        if(!login_.environment_created)
        {
            create_login_environment(ctx);
            login_.environment_created = true;
        }

        while(login_.cursor < login_.buildings.size() && processed_this_frame < login_.buildings_per_frame)
        {
            auto& building = login_.buildings[login_.cursor];
            const bool wait_limit_reached = building.attempts >= kLoginMaxAssetWaitFrames;
            if(!create_login_building(ctx, login_.content_root, building, wait_limit_reached))
            {
                if(building.attempts < kLoginMaxAssetWaitFrames)
                {
                    login_.status = "waiting_assets";
                    return;
                }

                ++login_.skipped;
                ++login_.cursor;
                continue;
            }

            ++login_.created;
            ++login_.cursor;
            ++processed_this_frame;
        }

        if(login_.cursor >= login_.buildings.size())
        {
            if(!login_.terrain_created)
            {
                create_login_terrain(ctx, login_.terrain);
                login_.terrain_created = true;
            }

            login_.active = false;
            login_.completed = true;
            login_.status = "done";
            APPLOG_INFO("load_login done: placed={} skipped={}", login_.created, login_.skipped);
            invalidate_camera();
        }
    }
    catch(const std::exception& e)
    {
        login_.active = false;
        login_.completed = false;
        login_.status = "error";
        login_.error = e.what();
    }
}

void mcp_system::service_pending_screenshot(rtti::context& ctx)
{
    if(!pending_.active)
    {
        return;
    }

    try
    {
        if(pending_.readback_started)
        {
            --pending_.readback_frames_left;
            if(pending_.readback_frames_left <= 0)
            {
                bx::FilePath file_path(pending_.path.c_str());
                if(!bx::makeAll(file_path.getPath()))
                {
                    throw std::runtime_error("screenshot: failed to create output directory");
                }

                bx::FileWriter writer;
                if(!bx::open(&writer, file_path))
                {
                    throw std::runtime_error("screenshot: failed to open output file");
                }

                bx::Error err;
                bimg::imageWritePng(&writer,
                                    pending_.w,
                                    pending_.h,
                                    pending_.w * 4,
                                    pending_.pixels.data(),
                                    static_cast<bimg::TextureFormat::Enum>(bgfx::TextureFormat::RGBA8),
                                    false,
                                    &err);
                bx::close(&writer);

                clear_pending_readback();
                pending_.active = false;
            }
            return;
        }

        auto& scn = ctx.get_cached<ecs>().get_scene();
        auto camera = ensure_camera(ctx);
        auto& camera_comp = camera.get<camera_component>();
        camera_comp.set_viewport_size({pending_.w, pending_.h});

        auto& rpath = ctx.get_cached<rendering_system>();
        rpath.on_frame_update(scn, kMcpFrameDt);
        rpath.on_frame_before_render(scn, kMcpFrameDt);
        rpath.render_scene(camera, camera_comp, scn, kMcpFrameDt, false);

        --pending_.frames_left;
        if(pending_.frames_left <= 0)
        {
            const auto& obuffer = camera_comp.get_render_view().fbo_safe_get("OBUFFER");
            if(!obuffer)
            {
                throw std::runtime_error("screenshot: OBUFFER unavailable");
            }

            const auto input_tex = bgfx::getTexture(obuffer->native_handle());
            const auto format = bgfx::TextureFormat::RGBA8;
            const uint64_t flags = BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | BGFX_SAMPLER_U_CLAMP |
                                   BGFX_SAMPLER_V_CLAMP;
            pending_.readback_texture =
                bgfx::createTexture2D(static_cast<uint16_t>(pending_.w),
                                      static_cast<uint16_t>(pending_.h),
                                      false,
                                      1,
                                      format,
                                      flags,
                                      nullptr);

            bgfx::TextureInfo info;
            bgfx::calcTextureSize(info, pending_.w, pending_.h, 1, false, false, 1, format);
            pending_.pixels.resize(info.storageSize);

            bgfx::ViewId view_id = gfx::render_pass("MCP Blit").id;
            bgfx::touch(view_id);
            bgfx::blit(view_id, pending_.readback_texture, 0, 0, input_tex);
            bgfx::readTexture(pending_.readback_texture, pending_.pixels.data());

            pending_.readback_started = true;
            pending_.readback_frames_left = 3;
        }
    }
    catch(...)
    {
        clear_pending_readback();
        pending_.active = false;
    }
}

void mcp_system::clear_pending_readback()
{
    if(bgfx::isValid(pending_.readback_texture))
    {
        bgfx::destroy(pending_.readback_texture);
        pending_.readback_texture = BGFX_INVALID_HANDLE;
    }
    pending_.readback_started = false;
    pending_.readback_frames_left = 0;
    pending_.pixels.clear();
}
} // namespace unravel
