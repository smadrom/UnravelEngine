#include "mcp_system.h"

#include "mcp_commands.h"

#include <engine/defaults/defaults.h>
#include <engine/ecs/components/transform_component.h>
#include <engine/ecs/ecs.h>
#include <engine/events.h>
#include <engine/rendering/ecs/components/camera_component.h>
#include <engine/rendering/ecs/systems/rendering_system.h>
#include <graphics/render_pass.h>
#include <graphics/utils/bgfx_utils.h>

#include <bimg/encode.h>
#include <bx/file.h>

#include <cstdlib>
#include <stdexcept>

namespace unravel
{
namespace
{
constexpr auto kMcpFrameDt = delta_t(0.016667f);
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
