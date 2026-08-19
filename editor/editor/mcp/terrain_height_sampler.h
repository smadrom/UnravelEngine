#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace unravel
{
struct terrain_heightfield
{
    std::vector<float> heights;
    uint32_t width = 0;
    uint32_t height = 0;
    float height_min = 0.0f;
    float height_max = 0.0f;
    float world_width = 0.0f;
    float world_depth = 0.0f;
    float world_left = 0.0f;
    float world_top = 0.0f;
    bool has_world_origin = false;

    [[nodiscard]] auto is_valid() const -> bool
    {
        const bool has_valid_world_rect =
            !has_world_origin ||
            (std::isfinite(world_left) && std::isfinite(world_top) && std::isfinite(world_left + world_width) &&
             std::isfinite(world_top - world_depth));
        return width >= 2 && height >= 2 && heights.size() >= static_cast<size_t>(width) * static_cast<size_t>(height) &&
               std::isfinite(height_min) && std::isfinite(height_max) && height_max >= height_min &&
               std::isfinite(world_width) && std::isfinite(world_depth) && world_width > 0.0f && world_depth > 0.0f &&
               has_valid_world_rect;
    }

    [[nodiscard]] auto height_range() const -> float
    {
        return height_max - height_min;
    }

    [[nodiscard]] auto heightfield_entity_y() const -> float
    {
        return height_min;
    }

    [[nodiscard]] auto heightfield_mesh_scale() const -> float
    {
        return -height_range();
    }

    [[nodiscard]] auto sample_terrain_normalized(float world_x, float world_z, float& out_height) const -> bool
    {
        if(!is_valid() || !std::isfinite(world_x) || !std::isfinite(world_z))
        {
            return false;
        }

        const float u = has_world_origin ? (world_x - world_left) / world_width : (world_x / world_width) + 0.5f;
        const float v = has_world_origin ? (world_top - world_z) / world_depth : 0.5f - (world_z / world_depth);
        if(u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
        {
            return false;
        }

        const float fx = u * static_cast<float>(width - 1u);
        const float fz = v * static_cast<float>(height - 1u);
        const uint32_t x0 = std::min(static_cast<uint32_t>(std::floor(fx)), width - 2u);
        const uint32_t z0 = std::min(static_cast<uint32_t>(std::floor(fz)), height - 2u);
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

        if(tx + tz <= 1.0f)
        {
            out_height = h00 + (h10 - h00) * tx + (h01 - h00) * tz;
        }
        else
        {
            out_height = h11 + (h10 - h11) * (1.0f - tz) + (h01 - h11) * (1.0f - tx);
        }
        return std::isfinite(out_height);
    }

    [[nodiscard]] auto sample_terrain_height(float world_x, float world_z, float& out_height) const -> bool
    {
        float normalized = 0.0f;
        if(!sample_terrain_normalized(world_x, world_z, normalized))
        {
            return false;
        }

        out_height = height_min + normalized * height_range();
        return std::isfinite(out_height);
    }
};
} // namespace unravel
