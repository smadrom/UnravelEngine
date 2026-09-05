#include "../tests.h"
#include <engine/pw/pw_effect_shader.h>

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
    const auto small = pw_screen_warp_strength({0.1f, 0.2f}, 0.5f);
    check(std::abs(small.x - 0.042f) < 0.000001f && std::abs(small.y - 0.084f) < 0.000001f,
          "screen warp uses source attenuation and alpha in both UV axes");
    check(pw_screen_warp_strength({0.49f, 0.1f}, 1) == math::vec2{}, "near-fullscreen warp obeys the source attenuation cutoff");
    check(pw_screen_warp_strength({0.1f, 0.2f}, 0) == math::vec2{}, "transparent source produces no displacement");
    std::printf("PW effect shader constants: %d checks, %d failures\n", checks, failures);
    return failures;
}
} // namespace
} // namespace unravel

using namespace unravel;
REGISTER_TEST_SUITE("PW effect shader constants / warp", run_pw_effect_shader_suite)
