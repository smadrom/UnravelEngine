#include "editor/editor/mcp/terrain_height_sampler.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
void expect_near(float actual, float expected)
{
    assert(std::fabs(actual - expected) < 0.0001f);
}
} // namespace

int main()
{
    unravel::terrain_heightfield terrain;
    terrain.heights = {
        0.0f, 0.5f, 1.0f,
        0.25f, 0.75f, 1.0f,
        0.5f, 0.75f, 1.0f,
    };
    terrain.width = 3;
    terrain.height = 3;
    terrain.height_min = 10.0f;
    terrain.height_max = 30.0f;
    terrain.world_width = 4.0f;
    terrain.world_depth = 4.0f;

    float sampled = 0.0f;

    assert(terrain.sample_terrain_height(-2.0f, -2.0f, sampled));
    expect_near(sampled, 20.0f);

    assert(terrain.sample_terrain_height(2.0f, 2.0f, sampled));
    expect_near(sampled, 30.0f);

    assert(terrain.sample_terrain_height(0.0f, 0.0f, sampled));
    expect_near(sampled, 25.0f);

    assert(terrain.sample_terrain_height(0.5f, 1.0f, sampled));
    expect_near(sampled, 25.0f);

    expect_near(terrain.height_range(), 20.0f);
    expect_near(terrain.heightfield_entity_y(), 10.0f);
    expect_near(terrain.heightfield_mesh_scale(), -20.0f);

    assert(!terrain.sample_terrain_height(2.1f, 0.0f, sampled));

    terrain.world_left = -10.0f;
    terrain.world_top = 20.0f;
    terrain.has_world_origin = true;
    assert(terrain.is_valid());

    assert(terrain.sample_terrain_height(-10.0f, 16.0f, sampled));
    expect_near(sampled, 20.0f);

    assert(terrain.sample_terrain_height(-6.0f, 20.0f, sampled));
    expect_near(sampled, 30.0f);

    assert(terrain.sample_terrain_height(-8.0f, 18.0f, sampled));
    expect_near(sampled, 25.0f);

    assert(!terrain.sample_terrain_height(-5.9f, 18.0f, sampled));

    terrain.world_left = std::numeric_limits<float>::infinity();
    assert(!terrain.is_valid());
    return 0;
}
