#pragma once

#include <engine/pw/detail/json.hpp>
#include <engine/pw/pw_effect_geometry.h>
#include <filesystem/filesystem.h>

#include <memory>
#include <string>
#include <vector>

namespace unravel
{
class asset_manager;
class mesh;
struct animation_clip;
enum class pw_effect_resource_status;

struct pw_effect_model_prepared
{
    bool valid = false;
    std::string error;
    std::string model_ref;
    std::string animation_ref;
    double animation_duration = 0;
    size_t joint_count = 0;
    std::vector<std::string> textures;
};

/** Worker-only validation of the native model descriptor and material slot inventory. */
auto prepare_pw_effect_model(const fs::path& content_root, const nlohmann::json& dependency)
    -> std::shared_ptr<pw_effect_model_prepared>;

struct pw_effect_model_batch
{
    std::vector<pw_effect_vertex> triangles;
    asset_handle<gfx::texture> texture;
    bool untextured = false;
};

/** Immutable CPU snapshot of the imported mesh, palette and authored action. No scene entities. */
class pw_effect_model_geometry
{
public:
    pw_effect_model_geometry();
    ~pw_effect_model_geometry();
    auto prepare(mesh& source, const animation_clip* animation, size_t material_count) -> bool;
    auto error() const -> const std::string&;
    auto batch_count() const -> size_t;
    /** Appends to exactly batch_count material batches, so repeated source instances can share them. */
    auto append_triangles(std::vector<pw_effect_model_batch>& batches, double time_seconds, int loops,
                          const math::mat4& world, const math::vec4& color) const -> bool;

private:
    struct implementation;
    std::unique_ptr<implementation> impl_;
};

/** Polls the real imported mesh/animation and every required material texture without blocking. */
class pw_effect_model_runtime
{
public:
    pw_effect_model_runtime();
    ~pw_effect_model_runtime();
    pw_effect_model_runtime(pw_effect_model_runtime&&) noexcept;
    auto operator=(pw_effect_model_runtime&&) noexcept -> pw_effect_model_runtime&;
    void begin(std::shared_ptr<pw_effect_model_prepared> prepared, std::string protocol_root);
    auto poll(asset_manager& manager) -> pw_effect_resource_status;
    auto error() const -> const std::string&;
    auto batch_count() const -> size_t;
    auto append_triangles(std::vector<pw_effect_model_batch>& batches, double time_seconds, int loops,
                          const math::mat4& world, const math::vec4& color) const -> bool;

private:
    struct implementation;
    std::unique_ptr<implementation> impl_;
};
} // namespace unravel
