#pragma once

#include <engine/pw/pw_map_effects.h>
#include <math/math.h>

namespace unravel
{
struct pw_shader_constant_target
{
    int32_t interval_ms = 0;
    math::vec4 value{};
};

struct pw_shader_constant
{
    uint32_t index = 0;
    math::vec4 initial{};
    int32_t loops = 0;
    std::vector<pw_shader_constant_target> targets;
};

/** Decode the ordered PSFileVersion 1 constant records once during CPU preparation. */
auto parse_pw_effect_shader_constants(const std::vector<pw_effect_field>& fields) -> std::vector<pw_shader_constant>;
/** Native GfxPSConst interpolation, hold, velocity and finite/infinite loop semantics. */
auto evaluate_pw_effect_shader_constant(const pw_shader_constant& constant, uint64_t elapsed_ms) -> math::vec4;
/** Native screen-space warp attenuates oversized sprites before deriving UV displacement. */
auto pw_screen_warp_strength(const math::vec2& projected_size, float alpha) -> math::vec2;
} // namespace unravel
