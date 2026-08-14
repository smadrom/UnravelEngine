#pragma once

#include "mcp_control_server.h"
#include "pw_runtime_client.h"
#include "pw_session_controller.h"
#include "terrain_height_sampler.h"

#include <base/basetypes.hpp>
#include <bgfx/bgfx.h>
#include <context/context.hpp>
#include <entt/entt.hpp>
#include <math/math.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace unravel
{
constexpr int kLoginSceneCameraCount = 39;
constexpr int kLoginSceneLoginIndex = 0;
constexpr int kLoginSceneSelcharIndex = 1;
constexpr int kLoginSceneCreateIndex = 14;
constexpr int kLoginSceneChooseIndex = 38;

struct login_scene_camera
{
    math::vec3 pos{};
    math::vec3 dir{0.0f, 0.0f, 1.0f};
    math::vec3 up{0.0f, 1.0f, 0.0f};
    bool valid = false;
};

struct login_scene_config
{
    std::array<login_scene_camera, kLoginSceneCameraCount> cameras{};
    std::vector<math::vec3> new_char_positions;
    math::vec3 new_char_center{};
    bool loaded = false;
};

class mcp_system
{
public:
    mcp_system();

    struct login_load_status
    {
        std::string status = "idle";
        std::string content_root;
        std::string error;
        uint32_t done = 0;
        uint32_t total = 0;
        uint32_t created = 0;
        uint32_t skipped = 0;
        uint32_t foliage_done = 0;
        uint32_t foliage_total = 0;
        uint32_t foliage_created = 0;
        uint32_t foliage_skipped = 0;
        uint32_t water_total = 0;
        uint32_t water_created = 0;
        uint32_t water_skipped = 0;
        uint32_t current_index = 0;
        uint32_t current_attempts = 0;
        std::string current_kind;
        std::string current_name;
        std::string current_model;
        std::string current_texture;
        math::vec3 current_position{};
        bool has_current = false;
        bool terrain = false;
    };

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

    struct login_building
    {
        std::string name;
        std::string model;
        std::string texture;
        std::vector<std::string> textures;
        math::vec3 position{};
        math::vec3 forward{0.0f, 0.0f, 1.0f};
        math::vec3 up{0.0f, 1.0f, 0.0f};
        bool alpha_blend = false;
        bool alpha_test = false;
        float source_position_y = 0.0f;
        float terrain_surface_y = 0.0f;
        float local_min_y = 0.0f;
        bool terrain_sample_valid = false;
        uint32_t attempts = 0;
        bool texture_request_logged = false;
        bool texture_ready_logged = false;
        bool placement_logged = false;
    };

    struct login_foliage
    {
        std::string name;
        std::string model;
        std::string texture;
        std::vector<std::string> textures;
        math::vec3 position{};
        bool alpha_blend = false;
        bool alpha_test = true;
        float source_position_y = 0.0f;
        float terrain_surface_y = 0.0f;
        float local_min_y = 0.0f;
        int tree_type = -1;
        bool terrain_sample_valid = false;
        uint32_t attempts = 0;
        bool texture_request_logged = false;
        bool texture_ready_logged = false;
        bool placement_logged = false;
    };

    struct login_water
    {
        std::string name;
        std::string payload;
        uint32_t visible_cells = 0;
    };

    auto init(rtti::context& ctx) -> bool;
    auto deinit(rtti::context& ctx) -> bool;
    void on_frame_end(rtti::context& ctx, delta_t dt);
    auto ensure_camera(rtti::context& ctx) -> entt::handle;
    void invalidate_camera();
    void request_screenshot(const std::string& path, uint32_t w, uint32_t h, bool render_ui = false);
    void start_login_load(const std::string& content_root, uint32_t buildings_per_frame, bool restart);
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
    void service_login_loader(rtti::context& ctx);
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

    struct login_loader
    {
        std::string content_root;
        std::string status = "idle";
        std::string error;
        std::vector<login_building> buildings;
        std::vector<login_foliage> foliage;
        std::vector<login_water> water;
        terrain_heightfield terrain;
        uint32_t buildings_per_frame = 3;
        uint32_t cursor = 0;
        uint32_t created = 0;
        uint32_t skipped = 0;
        uint32_t foliage_cursor = 0;
        uint32_t foliage_created = 0;
        uint32_t foliage_skipped = 0;
        uint32_t water_created = 0;
        uint32_t water_skipped = 0;
        bool active = false;
        bool completed = false;
        bool terrain_created = false;
        bool water_created_flag = false;
        bool environment_created = false;
        uint32_t character_attempts = 0;
    };

    login_loader login_;
};
} // namespace unravel
