#pragma once

#include <engine/pw/detail/json.hpp>
#include <engine/assets/asset_handle.h>
#include <graphics/texture.h>
#include <filesystem/filesystem.h>
#include <context/context.hpp>
#include <entt/entt.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace unravel
{
class asset_manager;
struct pw_effect_document;
struct pw_effect_model_prepared;

/** An ordered Angelica field. Repeated names are intentionally retained. */
struct pw_effect_field
{
    std::string name;
    std::string value;
    std::vector<double> numeric_values;
};

/** Native controller data, including fields needed by the specific controller type. */
struct pw_effect_controller
{
    int type = -1;
    double start_seconds = 0;
    double end_seconds = -1;
    std::vector<pw_effect_field> fields;
    // Worker-prepared native noise table and arc-length sampled Bezier path.
    std::vector<std::array<float, 3>> noise_values;
    std::vector<std::array<float, 3>> noise_octaves; // start, wavelength, amplitude
    std::vector<std::array<float, 4>> curve_samples; // xyz, normalized arc length
};

/** Authored keypoint; durations are milliseconds, quaternion order is XYZW. */
struct pw_effect_keypoint
{
    int interpolation = 0;
    uint32_t duration_ms = 0;
    std::array<float, 3> position{};
    std::array<float, 4> direction{};
    uint32_t color_argb = 0;
    float scale = 0;
    float rotation_2d = 0;
    std::vector<pw_effect_controller> controllers;
};

struct pw_effect_dependency
{
    std::string kind;
    std::string source_path;
    std::string output;
    bool required = true;
    std::shared_ptr<pw_effect_document> nested;
    nlohmann::json model_descriptor;
    std::shared_ptr<pw_effect_model_prepared> model;
    std::string implementation;
};

/** CPU-only parsed element. Geometry and emitter fields retain original ordering. */
struct pw_effect_element
{
    size_t index = 0;
    int type = 0;
    std::string name;
    std::vector<pw_effect_field> base_fields;
    std::vector<pw_effect_field> type_fields;
    std::vector<pw_effect_controller> particle_affectors;
    uint32_t start_ms = 0;
    std::vector<pw_effect_keypoint> keypoints;
    std::vector<pw_effect_dependency> dependencies;
    pw_effect_controller geometry_noise;
};

struct pw_effect_document
{
    bool valid = false;
    std::string error;
    std::string source_id;
    int version = 0;
    float default_scale = 1;
    float default_speed = 1;
    float default_alpha = 1;
    std::vector<pw_effect_field> header_fields;
    std::vector<pw_effect_element> elements;
};

/** Decode/validate converter source records without engine services or GPU access. */
auto parse_pw_effect_document(const nlohmann::json& document) -> pw_effect_document;

/** One source placement, identified independently of GFX asset reuse. */
struct pw_effect_instance
{
    std::string source_id;
    std::string effect_ref;
    std::array<float, 3> position{};
    std::array<float, 3> forward{};
    std::array<float, 3> up{};
    float scale = 1;
    float speed = 1;
    float alpha = 1;
    int valid_time = 0;
    pw_effect_document document;
};

struct pw_effect_preparation
{
    bool valid = false;
    std::string error;
    size_t raw_references = 0;
    size_t duplicate_references = 0;
    float day_night_factor = 0;
    std::vector<pw_effect_instance> instances;
};

/** Run on the map preparation worker after manifest/hash validation. No asset-manager calls. */
auto prepare_pw_map_effects(const fs::path& content_root, const nlohmann::json& scene) -> pw_effect_preparation;

/** Strict numeric access. No default is substituted for missing/malformed authored data. */
auto read_pw_effect_numbers(const std::vector<pw_effect_field>& fields, const std::string& name,
                            size_t occurrence, size_t count) -> std::vector<double>;

/** Native CPU geometry helpers; ring output is an alternating lower/upper strip. */
auto make_pw_effect_ring(float radius, float height, float pitch, uint32_t sectors, bool centered)
    -> std::vector<std::array<float, 3>>;
auto sample_pw_effect_box(const std::array<float, 3>& size, bool surface, const std::array<float, 4>& random)
    -> std::array<float, 3>;
/** Native lightning amplitude: scalar before GFX 102, authored transition track from 102 onward. */
auto sample_pw_effect_lightning_amplitude(const pw_effect_element& element, int document_version, float elapsed_seconds)
    -> float;

enum class pw_effect_resource_status { waiting, ready, failed };

struct pw_effect_texture_state
{
    std::string key;
    asset_handle<gfx::texture> handle;
};

/** Poll one frame. Never block; readiness means a usable native texture, not metadata. */
auto poll_pw_effect_textures(asset_manager& manager, const std::string& content_root,
                            const pw_effect_element& element, std::vector<pw_effect_texture_state>& state,
                            std::string& error) -> pw_effect_resource_status;

enum class pw_effect_stage { waiting, ready, failed };

/** Per-frame simulation policy for placed effects. Distances are metres in map space. */
struct pw_effect_update_policy
{
    /** Instances farther than this from every observer are frozen and hidden. <= 0 disables culling. */
    float update_radius = 200.0f;
    /** Upper bound on instances simulated in one frame; 0 means unlimited. Near instances rotate fairly. */
    uint32_t max_updates_per_frame = 128;
};

struct pw_effect_update_plan
{
    /** Instance indices to simulate this frame. */
    std::vector<size_t> update;
    /** Instances outside the radius: geometry is cleared once, state is kept. */
    std::vector<size_t> freeze;
    /** Near instances postponed to a later frame by the budget. */
    size_t deferred = 0;
};

/** Pure planning step without engine access. `cursor` persists across frames for round-robin fairness. */
auto plan_pw_effect_updates(const std::vector<pw_effect_instance>& instances,
                            const std::vector<std::array<float, 3>>& observers,
                            const pw_effect_update_policy& policy, size_t& cursor) -> pw_effect_update_plan;

/** Map-owned native effect runtime. Candidate creation stays inactive until its owner is published. */
class pw_map_effects_runtime
{
public:
    pw_map_effects_runtime();
    ~pw_map_effects_runtime();
    pw_map_effects_runtime(pw_map_effects_runtime&&) noexcept;
    auto operator=(pw_map_effects_runtime&&) noexcept -> pw_map_effects_runtime&;
    pw_map_effects_runtime(const pw_map_effects_runtime&) = delete;
    auto operator=(const pw_map_effects_runtime&) -> pw_map_effects_runtime& = delete;

    /** Begin with worker-prepared data; a fresh runtime must own each loading generation. */
    void begin(pw_effect_preparation prepared, std::string content_root, uint64_t generation);
    /** Bounded per-frame creation. Appends owned handles/keys for the map's cancellation cleanup. */
    auto stage(rtti::context& context, entt::handle owner, std::vector<entt::handle>& created_entities,
               std::vector<std::string>& generated_mesh_keys) -> pw_effect_stage;
    /** Advance authored timelines/particles after publication. Never performs asset loading. */
    void update(rtti::context& context, float delta_seconds);
    /** Observer positions (cameras) for distance culling in the next update; empty simulates everything. */
    void set_observers(std::vector<std::array<float, 3>> observers);
    void set_update_policy(const pw_effect_update_policy& policy);
    /** Plan applied by the most recent update, for diagnostics. */
    auto last_update_plan() const -> const pw_effect_update_plan&;
    /** Remove only entities/resources created by this runtime; safe after parent cleanup. */
    void destroy(rtti::context& context);
    auto error() const -> const std::string&;
    auto ready_ids() const -> const std::vector<std::string>&;

private:
    struct implementation;
    std::unique_ptr<implementation> impl_;
};

} // namespace unravel
