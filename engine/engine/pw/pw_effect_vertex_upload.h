#pragma once

#include "pw_effect_geometry.h"
#include <bgfx/bgfx.h>

#include <cstddef>
#include <cstring>

namespace unravel
{
// CPU math vectors are SIMD-aligned; the GPU stream must contain exactly 11 floats.
struct pw_effect_gpu_vertex
{
    float position[3];
    float uv[2];
    float color[4];
    float warp_strength[2];
};

static_assert(sizeof(pw_effect_gpu_vertex) == 44);
static_assert(offsetof(pw_effect_gpu_vertex, position) == 0);
static_assert(offsetof(pw_effect_gpu_vertex, uv) == 12);
static_assert(offsetof(pw_effect_gpu_vertex, color) == 20);
static_assert(offsetof(pw_effect_gpu_vertex, warp_strength) == 36);

inline auto pw_effect_gpu_vertex_layout() -> const bgfx::VertexLayout&
{
    static const bgfx::VertexLayout value = []
    {
        bgfx::VertexLayout layout;
        layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord1, 2, bgfx::AttribType::Float).end();
        return layout;
    }();
    return value;
}

inline void copy_pw_effect_vertices(void* destination, const pw_effect_vertex* source, size_t count)
{
    auto* bytes = static_cast<unsigned char*>(destination);
    for(size_t index = 0; index < count; ++index)
    {
        const auto& vertex = source[index];
        const pw_effect_gpu_vertex packed{
            {vertex.position.x, vertex.position.y, vertex.position.z},
            {vertex.uv.x, vertex.uv.y},
            {vertex.color.x, vertex.color.y, vertex.color.z, vertex.color.w},
            {vertex.warp_strength.x, vertex.warp_strength.y}};
        std::memcpy(bytes + index * sizeof(packed), &packed, sizeof(packed));
    }
}
} // namespace unravel
