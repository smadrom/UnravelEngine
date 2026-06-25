#pragma once

#include "mcp_control_server.h"
#include "pw_terrain_height_sampler.h"

#include <base/basetypes.hpp>
#include <bgfx/bgfx.h>
#include <context/context.hpp>
#include <entt/entt.hpp>
#include <math/math.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace unravel
{
class mcp_system
{
public:
    struct pw_login_load_status
    {
        std::string status = "idle";
        std::string content_root;
        std::string error;
        uint32_t done = 0;
        uint32_t total = 0;
        uint32_t created = 0;
        uint32_t skipped = 0;
        bool terrain = false;
    };

    struct pw_login_building
    {
        std::string name;
        std::string model;
        std::string texture;
        math::vec3 position{};
        math::vec3 forward{0.0f, 0.0f, 1.0f};
        float pw_position_y = 0.0f;
        float sampled_terrain_y = 0.0f;
        bool terrain_sample_valid = false;
        uint32_t attempts = 0;
        bool texture_request_logged = false;
        bool texture_ready_logged = false;
        bool placement_logged = false;
    };

    auto init(rtti::context& ctx) -> bool;
    auto deinit(rtti::context& ctx) -> bool;
    void on_frame_end(rtti::context& ctx, delta_t dt);
    auto ensure_camera(rtti::context& ctx) -> entt::handle;
    void invalidate_camera();
    void request_screenshot(const std::string& path, uint32_t w, uint32_t h);
    void start_pw_login_load(const std::string& content_root, uint32_t buildings_per_frame, bool restart);
    auto get_pw_login_load_status() const -> pw_login_load_status;

private:
    void service_pending_screenshot(rtti::context& ctx);
    void service_pw_login_loader(rtti::context& ctx);
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

    struct pw_login_loader
    {
        std::string content_root;
        std::string status = "idle";
        std::string error;
        std::vector<pw_login_building> buildings;
        pw_terrain_heightfield terrain;
        uint32_t buildings_per_frame = 3;
        uint32_t cursor = 0;
        uint32_t created = 0;
        uint32_t skipped = 0;
        bool active = false;
        bool completed = false;
        bool terrain_created = false;
        bool environment_created = false;
    };

    pw_login_loader pw_login_;
};
} // namespace unravel
