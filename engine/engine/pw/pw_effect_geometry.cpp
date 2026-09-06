#include "pw_effect_geometry.h"
#include "pw_effect_shader.h"
#include "pw_effect_vertex_upload.h"

#include <engine/ecs/components/transform_component.h>
#include <engine/ecs/scene.h>
#include <engine/rendering/camera.h>
#include <engine/rendering/default_textures.h>
#include <engine/rendering/gpu_program.h>
#include <bgfx/bgfx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace unravel
{
namespace
{
auto blend_factor(int native) -> uint64_t
{
    switch(native)
    {
        case 1: return BGFX_STATE_BLEND_ZERO;
        case 2: return BGFX_STATE_BLEND_ONE;
        case 3: return BGFX_STATE_BLEND_SRC_COLOR;
        case 4: return BGFX_STATE_BLEND_INV_SRC_COLOR;
        case 5: return BGFX_STATE_BLEND_SRC_ALPHA;
        case 6: return BGFX_STATE_BLEND_INV_SRC_ALPHA;
        case 7: return BGFX_STATE_BLEND_DST_ALPHA;
        case 8: return BGFX_STATE_BLEND_INV_DST_ALPHA;
        case 9: return BGFX_STATE_BLEND_DST_COLOR;
        case 10: return BGFX_STATE_BLEND_INV_DST_COLOR;
        case 11: return BGFX_STATE_BLEND_SRC_ALPHA_SAT;
        default: throw std::runtime_error("unsupported Angelica blend factor");
    }
}

auto projected_extent(const camera& camera, const math::vec3& position, const math::vec2& size) -> math::vec2
{
    const auto view_projection = camera.get_view_projection();
    const math::vec4 center = view_projection * math::vec4(position, 1);
    const math::vec4 right = view_projection * math::vec4(position + camera.get_view_inverse().x_unit_axis(), 1);
    if(center.w <= 0 || right.w <= 0) return {};
    const float scale_x = std::abs(right.x / right.w - center.x / center.w) * 0.5f;
    const auto viewport = camera.get_viewport_size();
    const float aspect = static_cast<float>(viewport.width) / std::max(1.0f, static_cast<float>(viewport.height));
    return {size.x * scale_x, size.y * scale_x * aspect};
}

void append_quad(std::vector<pw_effect_vertex>& vertices, const pw_effect_quad& quad, const camera& camera, bool warp)
{
    math::vec3 right = quad.camera_facing ? camera.get_view_inverse().x_unit_axis() : quad.right;
    math::vec3 up = quad.camera_facing ? camera.get_view_inverse().y_unit_axis() : quad.up;
    if(quad.velocity_facing && math::length2(right) > 0.000001f)
    {
        right = math::normalize(right);
        const auto facing_up = math::cross(quad.position - camera.get_position(), right);
        if(math::length2(facing_up) > 0.000001f) up = math::normalize(facing_up);
    }
    const float cosine = std::cos(quad.rotation);
    const float sine = std::sin(quad.rotation);
    const math::vec3 rotated_right = right * cosine + up * sine;
    up = up * cosine - right * sine;
    right = rotated_right;
    const std::array<math::vec2, 4> corners{{{-quad.pivot.x, -quad.pivot.y}, {1 - quad.pivot.x, -quad.pivot.y},
                                           {1 - quad.pivot.x, 1 - quad.pivot.y}, {-quad.pivot.x, 1 - quad.pivot.y}}};
    const math::vec2 warp_strength = !warp ? math::vec2{} : quad.warp_screen_space ?
        pw_screen_warp_strength(projected_extent(camera, quad.position, quad.size), quad.color.a) :
        math::vec2(quad.color.a * 0.5f);
    if(warp && warp_strength == math::vec2{}) return;
    for(const int index : {0, 1, 2, 2, 3, 0})
        vertices.push_back({quad.position + right * (corners[index].x * quad.size.x) +
                            up * (corners[index].y * quad.size.y), quad.uv[index], quad.color, warp_strength});
}

void append_ribbon(std::vector<pw_effect_vertex>& vertices, const pw_effect_ribbon& ribbon, const camera& camera)
{
    if(ribbon.points.size() < 2) return;
    std::array<pw_effect_vertex, 2> previous;
    math::vec3 previous_up{};
    for(size_t index = 0; index < ribbon.points.size(); ++index)
    {
        const auto& point = ribbon.points[index];
        const math::vec3 tangent = index == 0 ? ribbon.points[1].position - point.position : point.position - ribbon.points[index - 1].position;
        math::vec3 up = math::cross(tangent, ribbon.use_normal ? ribbon.normal : point.position - camera.get_position());
        if(math::length2(up) > 0.000001f) up = math::normalize(up);
        else up = index == 0 ? camera.get_view_inverse().y_unit_axis() : previous_up;
        if(math::dot(up, previous_up) < 0) up = -up;
        const std::array<pw_effect_vertex, 2> current{{
            {point.position - up * point.half_width, point.uv0, point.color},
            {point.position + up * point.half_width, point.uv1, point.color}}};
        if(index != 0)
        {
            vertices.insert(vertices.end(), {previous[0], previous[1], current[0], current[0], previous[1], current[1]});
        }
        previous = current;
        previous_up = up;
    }
}
} // namespace

auto has_pw_warp_geometry(scene& scene) -> bool
{
    for(const auto entity : scene.registry->view<pw_effect_geometry_component, active_component>())
    {
        const auto& geometry = scene.registry->get<pw_effect_geometry_component>(entity);
        if(geometry.shader == pw_effect_shader_mode::warp && (!geometry.triangles.empty() || !geometry.quads.empty() || !geometry.ribbons.empty())) return true;
    }
    return false;
}

auto render_pw_effect_geometry(scene& scene, const camera& camera, uint16_t view, gpu_program& program,
                              const gfx::texture* scene_color, bool warp_pass) -> uint32_t
{
    if(!program.begin()) return 0;
    struct entry { pw_effect_geometry_component* geometry; float depth; };
    std::vector<entry> entries;
    scene.registry->view<pw_effect_geometry_component, transform_component, active_component>().each(
        [&](auto, auto& geometry, auto& transform, auto&)
        {
            if((geometry.shader == pw_effect_shader_mode::warp) != warp_pass) return;
            const auto position = geometry.rotate_from_view ? geometry.view_origin :
                                  !geometry.quads.empty() ? geometry.quads.front().position :
                                  !geometry.ribbons.empty() && !geometry.ribbons.front().points.empty() ? geometry.ribbons.front().points.front().position :
                                  !geometry.triangles.empty() ? geometry.triangles.front().position :
                                  transform.get_position_global();
            entries.push_back({&geometry, math::distance2(camera.get_position(), position)});
        });
    std::stable_sort(entries.begin(), entries.end(), [](const auto& left, const auto& right)
    {
        if(left.geometry->render_layer != right.geometry->render_layer)
            return left.geometry->render_layer < right.geometry->render_layer;
        return left.depth > right.depth;
    });
    uint32_t triangles = 0;
    std::vector<pw_effect_vertex> vertices;
    for(const auto& entry : entries)
    {
        const auto& geometry = *entry.geometry;
        const auto texture = geometry.untextured ? default_textures::get().white_texture() : geometry.texture.get_if_ready();
        if(!texture || !texture->is_valid()) continue;
        const bool warp = geometry.shader == pw_effect_shader_mode::warp;
        const bool flow = geometry.shader == pw_effect_shader_mode::fluid;
        const auto shader_texture = flow ? geometry.shader_texture.get_if_ready() : texture;
        if(flow && (!shader_texture || !shader_texture->is_valid())) continue;
        if(warp && (!scene_color || !scene_color->is_valid())) continue;
        vertices = geometry.triangles;
        if(geometry.rotate_from_view)
        {
            const auto axis = math::normalize(geometry.view_axis);
            auto up = math::cross(geometry.view_origin - camera.get_position(), axis);
            if(math::length2(up) > 0.000001f) up = math::normalize(up);
            else up = camera.get_view_inverse().x_unit_axis();
            const auto plane = math::normalize(math::cross(axis, up));
            for(auto& vertex : vertices)
                vertex.position = geometry.view_origin - vertex.position.y * axis + vertex.position.x * up + vertex.position.z * plane;
        }
        for(const auto& ribbon : geometry.ribbons) append_ribbon(vertices, ribbon, camera);
        if(warp)
        {
            const auto extent = projected_extent(camera, geometry.warp_center, geometry.warp_size);
            for(auto& vertex : vertices)
                vertex.warp_strength = geometry.warp_screen_space ? pw_screen_warp_strength(extent, vertex.color.a) : math::vec2(vertex.color.a * 0.5f);
        }
        vertices.reserve(vertices.size() + geometry.quads.size() * 6);
        for(const auto& quad : geometry.quads) append_quad(vertices, quad, camera, warp);
        if(vertices.empty()) continue;
        const uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
            (geometry.depth_test ? BGFX_STATE_DEPTH_TEST_LEQUAL : uint64_t(0)) |
            (geometry.depth_write && !warp ? BGFX_STATE_WRITE_Z : uint64_t(0)) |
            (warp ? uint64_t(0) : BGFX_STATE_BLEND_FUNC(blend_factor(geometry.source_blend), blend_factor(geometry.destination_blend)));
        const auto& layout = pw_effect_gpu_vertex_layout();
        const uint32_t count = static_cast<uint32_t>(vertices.size());
        program.set_texture(0, "s_texColor", texture.get());
        program.set_texture(1, "s_shaderTexture", shader_texture.get());
        program.set_texture(2, "s_sceneColor", warp ? scene_color : texture.get(), BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
        program.set_uniform("u_pwEffectMode", math::vec4(warp ? 1.0f : flow ? 2.0f : 0.0f, bgfx::getCaps()->originBottomLeft ? 1.0f : 0.0f,
                                                       geometry.alpha_test ? geometry.alpha_cutoff : -1.0f, 0));
        program.set_uniform("u_pwFlowOffset", geometry.flow_offset);
        program.set_uniform("u_pwFlowBlend", geometry.flow_blend);
        bgfx::setState(state);
        if(bgfx::getAvailTransientVertexBuffer(count, layout) == count)
        {
            bgfx::TransientVertexBuffer buffer;
            bgfx::allocTransientVertexBuffer(&buffer, count, layout);
            copy_pw_effect_vertices(buffer.data, vertices.data(), vertices.size());
            bgfx::setVertexBuffer(0, &buffer);
            bgfx::submit(view, program.native_handle());
        }
        else
        {
            // Dedicated storage avoids silently dropping geometry when the shared transient pool is full.
            const auto* memory = bgfx::alloc(count * sizeof(pw_effect_gpu_vertex));
            copy_pw_effect_vertices(memory->data, vertices.data(), vertices.size());
            const auto buffer = bgfx::createVertexBuffer(memory, layout);
            if(!bgfx::isValid(buffer)) continue;
            bgfx::setVertexBuffer(0, buffer);
            bgfx::submit(view, program.native_handle());
            bgfx::destroy(buffer);
        }
        triangles += count / 3;
    }
    program.end();
    return triangles;
}
} // namespace unravel
