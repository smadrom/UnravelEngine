#include "editor/editor/mcp/pw_terrain_height_sampler.h"

#include <cassert>
#include <cmath>
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
    unravel::pw_terrain_heightfield terrain;
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
    expect_near(sampled, 10.0f);

    assert(terrain.sample_terrain_height(2.0f, 2.0f, sampled));
    expect_near(sampled, 30.0f);

    assert(terrain.sample_terrain_height(0.0f, 0.0f, sampled));
    expect_near(sampled, 25.0f);

    assert(!terrain.sample_terrain_height(2.1f, 0.0f, sampled));
    return 0;
}
