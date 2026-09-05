#include "mcp_system.h"

#include "mcp_commands.h"

#include "json.hpp"

#include <engine/animation/animation.h>
#include <engine/animation/ecs/components/animation_component.h>
#include <engine/assets/asset_manager.h>
#include <engine/defaults/defaults.h>
#include <engine/ecs/components/tag_component.h>
#include <engine/ecs/components/transform_component.h>
#include <engine/ecs/ecs.h>
#include <engine/ecs/scene.h>
#include <engine/events.h>
#include <engine/rendering/ecs/components/auto_exposure_component.h>
#include <engine/rendering/ecs/components/camera_component.h>
#include <engine/rendering/ecs/components/light_component.h>
#include <engine/rendering/ecs/components/model_component.h>
#include <engine/rendering/ecs/components/particle_emitter_component.h>
#include <engine/rendering/ecs/components/reflection_probe_component.h>
#include <engine/rendering/ecs/components/tonemapping_component.h>
#include <engine/rendering/ecs/systems/rendering_system.h>
#include <engine/rendering/material.h>
#include <engine/rendering/mesh.h>
#include <engine/rendering/model.h>
#include <filesystem/filesystem.h>
#include <graphics/graphics.h>
#include <graphics/render_pass.h>
#include <graphics/texture.h>
#include <graphics/utils/bgfx_utils.h>
#include <graphics/vertex_decl.h>
#include <logging/logging.h>

#include <bimg/encode.h>
#include <bx/file.h>

#include <editor/hub/hub.h>
#include <editor/hub/panels/scene_panel/scene_panel.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace unravel
{
namespace
{
using json = nlohmann::json;
constexpr auto kMcpFrameDt = delta_t(0.016667f);
}
mcp_system::mcp_system()
    : pw_session_(pw_runtime_)
{
}

auto mcp_system::init(rtti::context& ctx) -> bool
{
    map_loader_ = &ctx.get_cached<pw_map_loader>();
    auto& ev = ctx.get_cached<events>();
    ev.on_frame_end.connect(sentinel_, -1000, this, &mcp_system::on_frame_end);

    if(pw_runtime_.init())
    {
        APPLOG_INFO("PW runtime plugin loaded");
    }
    else
    {
        APPLOG_INFO("PW runtime plugin is optional and was not loaded: {}", pw_runtime_.get_last_error());
    }
    pw_session_.start();

    const char* port_env = std::getenv("PW_MCP_PORT");
    if(port_env != nullptr)
    {
        const int port = std::atoi(port_env);
        if(port > 0)
        {
            if(!server_.Start(port))
            {
                APPLOG_ERROR("mcp control server did not start on port {}", port);
            }
        }
        else
        {
            APPLOG_WARNING("mcp control server disabled: invalid PW_MCP_PORT='{}'", port_env);
        }
    }
    else
    {
        APPLOG_INFO("mcp control server disabled: PW_MCP_PORT is not set");
    }

    return true;
}

auto mcp_system::deinit(rtti::context& ctx) -> bool
{
    (void)ctx;
    server_.Stop();
    pw_session_.shutdown();
    pw_runtime_.deinit();
    clear_pending_readback();
    return true;
}

void mcp_system::on_frame_end(rtti::context& ctx, delta_t dt)
{
    (void)dt;
    if(observed_map_generation_ != map_loader_->generation())
    {
        observed_map_generation_ = map_loader_->generation();
        applied_pw_camera_ = pw_session_camera::none;
        invalidate_camera(ctx);
    }
    service_pw_session(ctx);
    service_pending_screenshot(ctx);
    server_.Drain([&](const std::string& req)
    {
        return mcp_commands::dispatch(ctx, req, *this);
    });
}

void mcp_system::service_pw_session(rtti::context& ctx)
{
    // Applies the session controller's desired login-scene camera on the main
    // thread. The controller's worker thread never touches the scene; it only
    // publishes a snapshot, and this frame boundary performs the mutation.
    const pw_session_snapshot snapshot = pw_session_.get_snapshot();
    if(snapshot.state == pw_session_state::in_world && snapshot.world.seq != 0)
    {
        if(world_map_epoch_ != snapshot.world.epoch)
        {
            world_map_epoch_ = snapshot.world.epoch;
            // The map transaction follows the session even while the login overlay is closed.
            if(snapshot.attested_instance_id == 161)
                start_map_load(ctx, "a61:", "a61", 3, true, true);
            else
                APPLOG_WARNING("No installed map binding for PW instance {}", snapshot.attested_instance_id);
        }
    }
    else if(snapshot.state != pw_session_state::in_world && world_map_epoch_ != 0)
    {
        world_map_epoch_ = 0;
        start_login_load(ctx, "app:/data/login", 3, true);
    }
    const pw_session_camera desired = snapshot.desired_camera;
    if(desired == pw_session_camera::none || desired == applied_pw_camera_)
    {
        return;
    }
    const login_scene_config config = get_login_scene_config();
    if(!config.loaded)
    {
        return; // preset would fail until the login scene config is parsed; retry later
    }
    try
    {
        const json request = {
            {"seq", 0},
            {"method", "camera_preset"},
            {"params", {{"preset", pw_session_camera_name(desired)}}},
        };
        const std::string response_json = mcp_commands::dispatch(ctx, request.dump(), *this);
        const json response = json::parse(response_json, nullptr, false);
        if(response.is_discarded() || !response.value("ok", false))
        {
            return; // camera not applicable yet; retry on a later frame
        }
        applied_pw_camera_ = desired;
        sync_camera_to_scene_viewport(ctx);
    }
    catch(const std::exception&)
    {
        // Camera failures must never escape the frame loop.
    }
}

auto mcp_system::ensure_camera(rtti::context& ctx) -> entt::handle
{
    auto& scn = ctx.get_cached<ecs>().get_scene();
    const bool camera_valid = mcp_cam_.registry() == scn.registry.get() && mcp_cam_.valid() &&
                              mcp_cam_.all_of<transform_component, camera_component>();
    if(!camera_valid)
    {
        mcp_cam_ = defaults::create_camera_entity(ctx, scn, "MCP Camera");
        apply_pw_login_camera_pose(mcp_cam_, get_login_scene_config());
    }
    return mcp_cam_;
}

void mcp_system::invalidate_camera(rtti::context& ctx)
{
    if(mcp_cam_.registry() == ctx.get_cached<ecs>().get_scene().registry.get() && mcp_cam_.valid())
        scene::destroy_entity(mcp_cam_);
    mcp_cam_ = {};
}

auto mcp_system::get_pw_runtime() -> pw_runtime_client&
{
    return pw_runtime_;
}

auto mcp_system::get_pw_session() -> pw_session_controller&
{
    return pw_session_;
}

void mcp_system::sync_camera_to_scene_viewport(rtti::context& ctx)
{
    auto source = ensure_camera(ctx);
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

void mcp_system::request_screenshot(const std::string& path, uint32_t w, uint32_t h, bool render_ui)
{
    clear_pending_readback();
    pending_.path = path;
    pending_.w = w;
    pending_.h = h;
    pending_.render_ui = render_ui;
    pending_.frames_left = 2;
    pending_.readback_frames_left = 0;
    pending_.active = true;
    pending_.readback_started = false;
    pending_.pixels.clear();
    pending_.last_path = path;
    pending_.last_error.clear();
    pending_.last_w = w;
    pending_.last_h = h;
    pending_.completed = false;
    ++pending_.request_id;
}

void mcp_system::start_map_load(rtti::context& ctx, const std::string& content_root,
                                const std::string& map_slug, uint32_t buildings_per_frame, bool restart, bool require_full)
{
    map_loader_->start_map_load(ctx, content_root, map_slug, buildings_per_frame, restart, require_full);
}
void mcp_system::start_login_load(rtti::context& ctx, const std::string& content_root,
                                  uint32_t buildings_per_frame, bool restart)
{
    map_loader_->start_login_load(ctx, content_root, buildings_per_frame, restart);
}
auto mcp_system::get_login_load_status() const -> login_load_status
{
    return map_loader_->get_login_load_status();
}
auto mcp_system::get_screenshot_status() const -> screenshot_status
{
    screenshot_status result;
    result.path = pending_.active ? pending_.path : pending_.last_path;
    result.error = pending_.last_error;
    result.w = pending_.active ? pending_.w : pending_.last_w;
    result.h = pending_.active ? pending_.h : pending_.last_h;
    result.frames_left = pending_.frames_left;
    result.readback_frames_left = pending_.readback_frames_left;
    result.active = pending_.active;
    result.readback_started = pending_.readback_started;
    result.completed = pending_.completed;
    result.request_id = pending_.request_id;

    if(pending_.active)
    {
        result.status = pending_.readback_started ? "readback" : "rendering";
    }
    else if(!pending_.last_error.empty())
    {
        result.status = "error";
    }
    else if(pending_.completed)
    {
        result.status = "done";
    }

    return result;
}

auto mcp_system::has_login_terrain() const -> bool { return map_loader_->has_login_terrain(); }
auto mcp_system::sample_login_terrain(float x, float z, float& height) const -> bool
{
    return map_loader_->sample_login_terrain(x, z, height);
}
auto mcp_system::get_login_terrain() const -> const terrain_heightfield& { return map_loader_->get_login_terrain(); }
auto mcp_system::get_login_scene_config() const -> login_scene_config { return map_loader_->get_login_scene_config(); }
auto mcp_system::get_login_content_root() const -> const std::string& { return map_loader_->get_login_content_root(); }
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

                pending_.last_path = pending_.path;
                pending_.last_w = pending_.w;
                pending_.last_h = pending_.h;
                pending_.last_error.clear();
                pending_.completed = true;
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
        rpath.render_scene(camera, camera_comp, scn, kMcpFrameDt, pending_.render_ui);

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
    catch(const std::exception& e)
    {
        pending_.last_path = pending_.path;
        pending_.last_w = pending_.w;
        pending_.last_h = pending_.h;
        pending_.last_error = e.what();
        pending_.completed = false;
        clear_pending_readback();
        pending_.active = false;
    }
    catch(...)
    {
        pending_.last_path = pending_.path;
        pending_.last_w = pending_.w;
        pending_.last_h = pending_.h;
        pending_.last_error = "screenshot: unknown failure";
        pending_.completed = false;
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
