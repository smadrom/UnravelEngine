#pragma once

#include "mcp_control_server.h"
#include "pw_runtime_client.h"
#include "pw_session_controller.h"
#include <engine/pw/pw_map_loader.h>

#include <base/basetypes.hpp>
#include <bgfx/bgfx.h>
#include <context/context.hpp>
#include <entt/entt.hpp>
#include <math/math.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace unravel
{
class mcp_system
{
public:
    mcp_system();

    using login_load_status = pw_map_loader::login_load_status;
    struct screenshot_status
    {
        std::string status = "idle";
        std::string path;
        std::string error;
        uint32_t w = 0;
        uint32_t h = 0;
        int frames_left = 0;
        int readback_frames_left = 0;
        bool active = false;
        bool readback_started = false;
        bool completed = false;
        uint64_t request_id = 0;
    };

    using login_building = pw_map_loader::login_building;
    using login_foliage = pw_map_loader::login_foliage;
    using login_water = pw_map_loader::login_water;
    using login_light = pw_map_loader::login_light;

    auto init(rtti::context& ctx) -> bool;
    auto deinit(rtti::context& ctx) -> bool;
    void on_frame_end(rtti::context& ctx, delta_t dt);
    auto ensure_camera(rtti::context& ctx) -> entt::handle;
    void invalidate_camera(rtti::context& ctx);
    void request_screenshot(const std::string& path, uint32_t w, uint32_t h, bool render_ui = false);
    void start_map_load(rtti::context& ctx,
                        const std::string& content_root,
                        const std::string& map_slug,
                        uint32_t buildings_per_frame,
                        bool restart, bool require_full = true);
    void start_login_load(rtti::context& ctx, const std::string& content_root, uint32_t buildings_per_frame, bool restart);
    auto get_login_load_status() const -> login_load_status;
    auto get_screenshot_status() const -> screenshot_status;
    auto has_login_terrain() const -> bool;
    auto sample_login_terrain(float world_x, float world_z, float& out_height) const -> bool;
    auto get_login_terrain() const -> const terrain_heightfield&;
    auto get_login_scene_config() const -> login_scene_config;
    auto get_login_content_root() const -> const std::string&;
    auto get_pw_runtime() -> pw_runtime_client&;
    auto get_pw_session() -> pw_session_controller&;
    /** Copies the mcp camera pose/params onto the scene-panel viewport camera. */
    void sync_camera_to_scene_viewport(rtti::context& ctx);

private:
    void service_pending_screenshot(rtti::context& ctx);
    void service_pw_session(rtti::context& ctx);
    void clear_pending_readback();

    McpControlServer server_;
    pw_runtime_client pw_runtime_;
    pw_session_controller pw_session_;
    pw_session_camera applied_pw_camera_ = pw_session_camera::none;
    std::shared_ptr<int> sentinel_ = std::make_shared<int>(0);
    entt::handle mcp_cam_;

    struct pending_screenshot
    {
        std::string path;
        uint32_t w = 0;
        uint32_t h = 0;
        bool render_ui = false;
        int frames_left = 0;
        int readback_frames_left = 0;
        bool active = false;
        bool readback_started = false;
        bgfx::TextureHandle readback_texture = BGFX_INVALID_HANDLE;
        std::vector<uint8_t> pixels;
        std::string last_path;
        std::string last_error;
        uint32_t last_w = 0;
        uint32_t last_h = 0;
        bool completed = false;
        uint64_t request_id = 0;
    };

    pending_screenshot pending_;

    pw_map_loader* map_loader_ = nullptr;
    uint64_t observed_map_generation_ = 0;
    uint32_t world_map_epoch_ = 0;
};
} // namespace unravel
