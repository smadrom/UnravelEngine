#include "../tests.h"
#include <engine/pw/pw_effect_shader.h>
#include <engine/pw/pw_effect_vertex_upload.h>

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace unravel
{
namespace
{
auto run_pw_effect_shader_suite(rtti::context&) -> int
{
    int failures = 0;
    int checks = 0;
    auto check = [&](bool condition, const char* message)
    {
        ++checks;
        if(!condition) { ++failures; std::printf("FAIL: %s\n", message); }
    };
    auto close = [](const math::vec4& left, const math::vec4& right)
    {
        return math::length(left - right) < 0.00001f;
    };
    // The actual a61 fluid track moves C0 from (1,1,1,0) to zero every eight seconds.
    const std::vector<pw_effect_field> fields = {
        {"PSConstCount", "2"}, {"PSConstIndex", "0"}, {"PSConstValue", "1, 1, 1, 0"},
        {"PSLoopCount", "-1"}, {"PSTargetCount", "1"}, {"PSInterval", "8000"}, {"PSConstValue", "0, 0, 0, 0"},
        {"PSConstIndex", "1"}, {"PSConstValue", "1, 0, 0, 0"}, {"PSLoopCount", "1"}, {"PSTargetCount", "0"}};
    const auto constants = parse_pw_effect_shader_constants(fields);
    check(constants.size() == 2 && constants[0].index == 0 && constants[1].index == 1, "ordered constants retain register binding");
    check(close(evaluate_pw_effect_shader_constant(constants[0], 2000), {0.75f, 0.75f, 0.75f, 0}), "a61 fluid samples the native linear quarter point");
    check(close(evaluate_pw_effect_shader_constant(constants[0], 8000), {1, 1, 1, 0}), "infinite track restarts at the exact endpoint");
    check(close(evaluate_pw_effect_shader_constant(constants[1], 100000), {1, 0, 0, 0}), "static blend constant remains authored");
    auto finite = constants[0];
    finite.loops = 2;
    check(close(evaluate_pw_effect_shader_constant(finite, 12000), {0.5f, 0.5f, 0.5f, 0}), "finite track interpolates its second cycle");
    check(close(evaluate_pw_effect_shader_constant(finite, 16000), {}), "finite track retains final value after the last cycle");
    pw_shader_constant held;
    held.initial = {2, 3, 4, 5};
    held.loops = 1;
    held.targets = {{-1000, {6, 7, 8, 9}}, {1000, {10, 11, 12, 13}}};
    check(close(evaluate_pw_effect_shader_constant(held, 1000), held.initial), "negative duration holds initial value including boundary");
    check(close(evaluate_pw_effect_shader_constant(held, 1500), {8, 9, 10, 11}), "following ramp starts from the held target");
    held.targets = {{0, {1, -1, 2, 0}}};
    check(close(evaluate_pw_effect_shader_constant(held, 2500), {4.5f, 0.5f, 9, 5}), "zero duration integrates authored per-second velocity");
    auto invalid = fields;
    invalid[7].value = "0";
    bool rejected = false;
    try { parse_pw_effect_shader_constants(invalid); } catch(const std::runtime_error&) { rejected = true; }
    check(rejected, "duplicate shader register is rejected");
    invalid = fields;
    invalid[5].value = "-2147483648";
    rejected = false;
    try { parse_pw_effect_shader_constants(invalid); } catch(const std::runtime_error&) { rejected = true; }
    check(rejected, "interval cannot overflow its native magnitude");
    const auto small_warp = pw_screen_warp_strength({0.1f, 0.2f}, 0.5f);
    check(std::abs(small_warp.x - 0.042f) < 0.000001f && std::abs(small_warp.y - 0.084f) < 0.000001f,
          "screen warp uses source attenuation and alpha in both UV axes");
    check(pw_screen_warp_strength({0.49f, 0.1f}, 1) == math::vec2{}, "near-fullscreen warp obeys the source attenuation cutoff");
    check(pw_screen_warp_strength({0.1f, 0.2f}, 0) == math::vec2{}, "transparent source produces no displacement");
    const auto& layout = pw_effect_gpu_vertex_layout();
    check(layout.getStride() == sizeof(pw_effect_gpu_vertex) && layout.getStride() == 44,
          "native effect GPU stride is independent of SIMD-aligned CPU vertices");
    check(layout.getOffset(bgfx::Attrib::Position) == 0 && layout.getOffset(bgfx::Attrib::TexCoord0) == 12 &&
          layout.getOffset(bgfx::Attrib::Color0) == 20 && layout.getOffset(bgfx::Attrib::TexCoord1) == 36,
          "native effect attribute offsets match the uploaded byte stream");
    const std::array<pw_effect_vertex, 2> cpu_vertices{{
        {{101, -202, 303}, {0.125f, 0.75f}, {0.1f, 0.2f, 0.3f, 0.4f}, {-0.25f, 0.5f}},
        {{-404, 505, -606}, {0.625f, 0.875f}, {0.6f, 0.7f, 0.8f, 0.9f}, {0.75f, -1.0f}}}};
    std::array<unsigned char, 2 * sizeof(pw_effect_gpu_vertex) + 16> guarded_bytes;
    guarded_bytes.fill(0xa5);
    copy_pw_effect_vertices(guarded_bytes.data() + 8, cpu_vertices.data(), cpu_vertices.size());
    const float expected[][11] = {
        {101, -202, 303, 0.125f, 0.75f, 0.1f, 0.2f, 0.3f, 0.4f, -0.25f, 0.5f},
        {-404, 505, -606, 0.625f, 0.875f, 0.6f, 0.7f, 0.8f, 0.9f, 0.75f, -1.0f}};
    for(size_t index = 0; index < cpu_vertices.size(); ++index)
    {
        float actual[11]{};
        std::memcpy(actual, guarded_bytes.data() + 8 + index * layout.getStride(), sizeof(actual));
        bool matches = true;
        for(size_t field = 0; field < 11; ++field) matches = matches && actual[field] == expected[index][field];
        check(matches, "consecutive native effect vertices preserve positions, UVs, colors and warp values at GPU stride");
    }
    bool guards_intact = true;
    for(size_t index = 0; index < 8; ++index)
        guards_intact = guards_intact && guarded_bytes[index] == 0xa5 && guarded_bytes[guarded_bytes.size() - 1 - index] == 0xa5;
    check(guards_intact, "native effect upload does not write past the GPU allocation");
    std::printf("PW effect shader constants: %d checks, %d failures\n", checks, failures);
    return failures;
}
} // namespace
} // namespace unravel

using namespace unravel;
REGISTER_TEST_SUITE("PW effect shader constants / warp", run_pw_effect_shader_suite)
