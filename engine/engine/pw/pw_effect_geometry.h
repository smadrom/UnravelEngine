#pragma once

#include <engine/assets/asset_handle.h>
#include <graphics/texture.h>
#include <math/math.h>

#include <array>
#include <vector>

namespace unravel
{
class scene;
class camera;
class gpu_program;

struct pw_effect_vertex
{
    math::vec3 position{};
    math::vec2 uv{};
    math::vec4 color{1.0f};
    math::vec2 warp_strength{};
};

struct pw_effect_quad
{
    math::vec3 position{};
    math::vec3 right{1.0f, 0.0f, 0.0f};
    math::vec3 up{0.0f, 1.0f, 0.0f};
    math::vec2 size{1.0f};
    math::vec2 pivot{0.5f};
    math::vec4 color{1.0f};
    std::array<math::vec2, 4> uv{{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
    float rotation = 0;
    bool camera_facing = false;
    bool warp_screen_space = false;
    bool velocity_facing = false;
};

enum class pw_effect_shader_mode { textured, warp, fluid };

struct pw_effect_ribbon_point
{
    math::vec3 position{};
    float half_width = 0;
    math::vec4 color{1};
    math::vec2 uv0{};
    math::vec2 uv1{};
};

struct pw_effect_ribbon
{
    std::vector<pw_effect_ribbon_point> points;
    math::vec3 normal{0, 1, 0};
    bool use_normal = false;
};

/** Transient render data. The map runtime reconstructs this component after scene clone/restore. */
struct pw_effect_geometry_component
{
    std::vector<pw_effect_vertex> triangles;
    std::vector<pw_effect_quad> quads;
    std::vector<pw_effect_ribbon> ribbons;
    bool rotate_from_view = false;
    math::vec3 view_origin{};
    math::vec3 view_axis{0, 0, 1};
    asset_handle<gfx::texture> texture;
    bool untextured = false;
    asset_handle<gfx::texture> shader_texture;
    pw_effect_shader_mode shader = pw_effect_shader_mode::textured;
    math::vec4 flow_offset{};
    math::vec4 flow_blend{};
    bool warp_screen_space = false;
    math::vec3 warp_center{};
    math::vec2 warp_size{};
    int source_blend = 5;
    int destination_blend = 6;
    int render_layer = 0;
    bool depth_test = true;
    bool depth_write = false;
    bool alpha_test = false;
    float alpha_cutoff = 0;
};

/** Draw native triangles/quads in the transparent pass, preserving authored blend factors. */
auto has_pw_warp_geometry(scene& scene) -> bool;
auto render_pw_effect_geometry(scene& scene, const camera& camera, uint16_t view, gpu_program& program,
                              const gfx::texture* scene_color = nullptr, bool warp_pass = false) -> uint32_t;
} // namespace unravel
