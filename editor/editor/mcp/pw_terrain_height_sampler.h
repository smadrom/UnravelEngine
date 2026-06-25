#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace unravel
{
struct pw_terrain_heightfield
{
    std::vector<float> heights;
    uint32_t width = 0;
    uint32_t height = 0;
    float height_min = 0.0f;
    float height_max = 0.0f;
    float world_width = 0.0f;
    float world_depth = 0.0f;

    [[nodiscard]] auto is_valid() const -> bool
    {
        return width >= 2 && height >= 2 && heights.size() >= static_cast<size_t>(width) * static_cast<size_t>(height) &&
               std::isfinite(height_min) && std::isfinite(height_max) && height_max >= height_min &&
               std::isfinite(world_width) && std::isfinite(world_depth) && world_width > 0.0f && world_depth > 0.0f;
    }

    [[nodiscard]] auto sample_terrain_height(float world_x, float world_z, float& out_height) const -> bool
    {
        if(!is_valid() || !std::isfinite(world_x) || !std::isfinite(world_z))
        {
            return false;
        }

        const float u = (world_x / world_width) + 0.5f;
        const float v = (world_z / world_depth) + 0.5f;
        if(u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
        {
            return false;
        }

        const float fx = u * static_cast<float>(width - 1u);
        const float fz = v * static_cast<float>(height - 1u);
        const uint32_t x0 = static_cast<uint32_t>(std::floor(fx));
        const uint32_t z0 = static_cast<uint32_t>(std::floor(fz));
        const uint32_t x1 = std::min(x0 + 1u, width - 1u);
        const uint32_t z1 = std::min(z0 + 1u, height - 1u);
        const float tx = fx - static_cast<float>(x0);
        const float tz = fz - static_cast<float>(z0);

        const auto height_at = [this](uint32_t x, uint32_t z) -> float
        {
            return heights[static_cast<size_t>(z) * static_cast<size_t>(width) + static_cast<size_t>(x)];
        };

        const float h00 = height_at(x0, z0);
        const float h10 = height_at(x1, z0);
        const float h01 = height_at(x0, z1);
        const float h11 = height_at(x1, z1);
        const float h0 = h00 + (h10 - h00) * tx;
        const float h1 = h01 + (h11 - h01) * tx;
        const float normalized = h0 + (h1 - h0) * tz;

        out_height = height_min + normalized * (height_max - height_min);
        return std::isfinite(out_height);
    }
};
} // namespace unravel
