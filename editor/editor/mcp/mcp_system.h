#pragma once

#include "mcp_control_server.h"

#include <base/basetypes.hpp>
#include <bgfx/bgfx.h>
#include <context/context.hpp>
#include <entt/entt.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace unravel
{
class mcp_system
{
public:
    auto init(rtti::context& ctx) -> bool;
    auto deinit(rtti::context& ctx) -> bool;
    void on_frame_end(rtti::context& ctx, delta_t dt);
    auto ensure_camera(rtti::context& ctx) -> entt::handle;
    void invalidate_camera();
    void request_screenshot(const std::string& path, uint32_t w, uint32_t h);

private:
    void service_pending_screenshot(rtti::context& ctx);
    void clear_pending_readback();

    McpControlServer server_;
    std::shared_ptr<int> sentinel_ = std::make_shared<int>(0);
    entt::handle mcp_cam_;

    struct pending_screenshot
    {
        std::string path;
        uint32_t w = 0;
        uint32_t h = 0;
        int frames_left = 0;
        int readback_frames_left = 0;
        bool active = false;
        bool readback_started = false;
        bgfx::TextureHandle readback_texture = BGFX_INVALID_HANDLE;
        std::vector<uint8_t> pixels;
    };

    pending_screenshot pending_;
};
} // namespace unravel
