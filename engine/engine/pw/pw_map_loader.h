#pragma once
#include "terrain_height_sampler.h"
#include <base/basetypes.hpp>
#include <context/context.hpp>
#include <entt/entt.hpp>
#include <math/math.h>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>
#include <threadpp/thread_pool.h>
#include <uuid/uuid.h>
#include <utility>

namespace unravel
{
class mesh;
struct pw_map_manifest_result;
class pw_map_effects_runtime;

struct pw_map_material_slot
{
    std::string texture;
    bool alpha_test = false;
    bool alpha_blend = false;
    bool two_sided = false;
    float alpha_cutoff = 0.5f;
};
/** Decode independent neutral material slots without loading GPU assets. */
auto parse_pw_map_material_slots(const std::string& document, float default_cutoff = 0.5f,
                                bool force_alpha_test = false) -> std::vector<pw_map_material_slot>;

/** Stable identity across editor scene serialization; never retains another registry's handles. */
struct pw_map_ownership
{
    std::string root_tag;
    hpp::uuid root_id;
    std::vector<hpp::uuid> entity_ids;
    hpp::uuid descriptor_id;
    uint64_t generation = 0;
    auto rebind(entt::registry& registry, entt::handle& root, std::vector<entt::handle>& entities,
                bool require_all = true) const -> bool;
};
using pw_map_environment_state = std::vector<std::pair<hpp::uuid, bool>>;
void restore_pw_map_environment(entt::registry& registry, pw_map_environment_state& original_states);

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

/** Runtime map ownership and loading, shared by editor and player. */
class pw_map_loader
{
public:
    struct login_load_status
    {
        std::string status = "idle";
        std::string content_root;
        std::string map;
        std::string error;
        std::string attempted_content_root;
        std::string attempted_map;
        std::string start_error;
        uint32_t done = 0;
        uint32_t total = 0;
        uint32_t created = 0;
        uint32_t skipped = 0;
        uint32_t foliage_done = 0;
        uint32_t foliage_total = 0;
        uint32_t foliage_created = 0;
        uint32_t foliage_skipped = 0;
        uint32_t grass_total = 0;
        uint32_t grass_created = 0;
        uint64_t grass_blades = 0;
        uint32_t ecmodels_total = 0;
        uint32_t ecmodels_created = 0;
        uint32_t effects_total = 0;
        uint32_t effects_created = 0;
        uint32_t effects_updated = 0;   // simulated in the last frame
        uint32_t effects_frozen = 0;    // outside the update radius of every observer
        uint32_t effects_deferred = 0;  // near, but postponed by the per-frame budget
        std::vector<std::string> ready_effect_ids;
        std::vector<std::string> ready_grass_ids;
        std::vector<std::string> ready_ecmodel_ids;
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
        bool full = false;
        uint64_t generation = 0;
        std::string active_map;
        uint32_t duplicate_references = 0;
        uint32_t water_duplicate_references = 0;
        std::vector<std::string> ready_building_ids;
        std::vector<std::string> ready_water_ids;
    };

    struct login_building
    {
        std::string kind = "building";
        std::string animation;
        bool animation_loop = true;
        float alpha_cutoff = 0.5f;
        std::string source_id;
        std::string name;
        std::string model;
        std::string texture;
        std::vector<std::string> textures;
        std::vector<pw_map_material_slot> materials;
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
        bool ready = false;
        std::chrono::steady_clock::time_point waiting_since{};
        bool texture_request_logged = false;
        bool texture_ready_logged = false;
        bool placement_logged = false;
    };

    struct login_foliage
    {
        std::string source_id;
        std::string name;
        std::string model;
        std::string texture;
        std::vector<std::string> textures;
        std::vector<pw_map_material_slot> materials;
        math::vec3 position{};
        bool alpha_blend = false;
        bool alpha_test = true;
        float source_position_y = 0.0f;
        float terrain_surface_y = 0.0f;
        float local_min_y = 0.0f;
        int tree_type = -1;
        bool terrain_sample_valid = false;
        uint32_t attempts = 0;
        bool ready = false;
        std::chrono::steady_clock::time_point waiting_since{};
        bool texture_request_logged = false;
        bool texture_ready_logged = false;
        bool placement_logged = false;
    };

    struct login_water
    {
        std::string source_id;
        std::string name;
        std::string payload;
        std::string surface_id;
        uint32_t visible_cells = 0;
    };

    struct login_light
    {
        enum class kind
        {
            directional,
            point
        };

        kind type = kind::directional;
        math::vec3 position{};
        math::vec3 direction{0.0f, -1.0f, 0.0f};
        math::color color{1.0f, 1.0f, 1.0f, 1.0f};
        float intensity = 0.0f;
        float range = 0.0f;
        uint32_t source_index = 0;
        bool has_intensity = false;
        bool has_range = false;
    };

    auto init(rtti::context& ctx) -> bool;
    auto deinit(rtti::context& ctx) -> bool;
    void on_frame_end(rtti::context& ctx, delta_t dt);
    void on_play_before_begin(rtti::context& ctx);
    void on_play_after_end(rtti::context& ctx);
    void on_play_transition(rtti::context& ctx);
    auto generation() const -> uint64_t;
    void start_map_load(rtti::context& ctx,
                        const std::string& content_root,
                        const std::string& map_slug,
                        uint32_t buildings_per_frame,
                        bool restart, bool require_full = true);
    void start_login_load(rtti::context& ctx, const std::string& content_root, uint32_t buildings_per_frame, bool restart);
    auto get_login_load_status() const -> login_load_status;
    auto has_login_terrain() const -> bool;
    auto sample_login_terrain(float world_x, float world_z, float& out_height) const -> bool;
    auto get_login_terrain() const -> const terrain_heightfield&;
    auto get_login_scene_config() const -> login_scene_config;
    auto get_login_content_root() const -> const std::string&;
    /** Extra observer for effect distance culling (the editor's Scene camera lives outside the game scene).
     *  Pass nullptr to clear. Scene cameras are always observers. */
    void set_effect_observer(const math::vec3* position);
private:
    friend struct pw_map_loader_test_access;
    void update_map_effects(rtti::context& ctx, delta_t dt);
    void service_scene_descriptor(rtti::context& ctx);
    void update_scene_descriptor_status(rtti::context& ctx);
    void on_skip_next_frame(rtti::context& ctx);
    void service_login_loader(rtti::context& ctx);
    void despawn_loaded_map(rtti::context& ctx);
    void reset_login_loader_if_scene_changed(rtti::context& ctx, bool force_rebind = false);
    std::shared_ptr<int> sentinel_ = std::make_shared<int>(0);
    struct login_loader
    {
        std::shared_ptr<pw_map_manifest_result> manifest;
        std::shared_ptr<pw_map_effects_runtime> effects;
        std::shared_ptr<mesh> prepared_terrain;
        std::vector<std::string> terrain_albedo_refs;
        bool require_full = false;
        uint32_t duplicate_references = 0;
        uint32_t water_duplicate_references = 0;
        uint32_t building_scan = 0;
        uint32_t foliage_scan = 0;
        std::string content_root;
        std::string map_slug = "login";
        std::string status = "idle";
        std::string error;
        std::vector<login_building> buildings;
        std::vector<login_building> grass;
        std::vector<login_building> ecmodels;
        uint32_t grass_scan = 0;
        uint32_t grass_created = 0;
        uint32_t ecmodels_scan = 0;
        uint32_t ecmodels_created = 0;
        std::chrono::steady_clock::time_point resource_wait_started{};
        std::vector<login_foliage> foliage;
        std::vector<login_water> water;
        std::vector<login_light> lights;
        std::vector<entt::handle> created_entities;
        pw_map_environment_state shared_entity_rollbacks;
        pw_map_ownership ownership;
        hpp::uuid descriptor_id;
        std::vector<std::string> generated_mesh_keys;
        std::vector<std::string> generated_texture_keys;
        entt::handle scene_anchor;
        entt::registry* scene_registry = nullptr;
        terrain_heightfield terrain;
        uint64_t generation = 0;
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

    struct play_checkpoint
    {
        login_loader map;
        std::string attempted_content_root;
        std::string attempted_map_slug;
        std::string start_error;
    };

    void cancel_preparation(rtti::context& ctx);
    auto is_play_checkpoint(const login_loader& state) const -> bool;
    void destroy_map(rtti::context& ctx, login_loader& state);
    void install_prepared_map(rtti::context& ctx);
    void fail_candidate(rtti::context& ctx, const std::string& error);
    void bind_scene_descriptor(rtti::context& ctx, login_loader& state);
    auto active_state() const -> const login_loader&;
    login_loader login_;
    std::unique_ptr<login_loader> previous_;
    bool has_effect_observer_ = false;
    math::vec3 effect_observer_{};
    std::unique_ptr<play_checkpoint> play_checkpoint_;
    bool play_session_ = false;
    tpp::job_future<login_loader> preparation_;
    std::shared_ptr<std::atomic<uint64_t>> preparation_generation_ = std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<std::mutex> preparation_mutex_ = std::make_shared<std::mutex>();
    entt::registry* preparation_registry_ = nullptr;
    entt::handle preparation_anchor_;
    bool preparing_ = false;
    uint64_t next_map_generation_ = 0;
    std::string attempted_content_root_;
    std::string attempted_map_slug_;
    std::string map_start_error_;
    struct scene_descriptor_request
    {
        hpp::uuid id;
        uint64_t instance_token = 0;
        std::string content_root;
        std::string map_slug;
        uint32_t buildings_per_frame = 3;
        bool require_full = true;
        auto operator==(const scene_descriptor_request&) const -> bool = default;
    };
    scene_descriptor_request scene_descriptor_;
    uint64_t next_descriptor_token_ = 0;
    hpp::uuid preparation_descriptor_id_;
};
void apply_pw_login_camera_pose(entt::handle camera, const login_scene_config& config);
} // namespace unravel
