#include "pw_effect_shader.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace unravel
{
auto parse_pw_effect_shader_constants(const std::vector<pw_effect_field>& fields) -> std::vector<pw_shader_constant>
{
    const auto begin = std::find_if(fields.begin(), fields.end(), [](const auto& field) { return field.name == "PSConstCount"; });
    if(begin == fields.end()) throw std::runtime_error("authored shader has no PSConstCount");
    size_t cursor = static_cast<size_t>(std::distance(fields.begin(), begin));
    auto numbers = [&](const std::string& name, size_t count)
    {
        if(cursor >= fields.size() || fields[cursor].name != name)
            throw std::runtime_error("invalid ordered shader constant field: " + name);
        return read_pw_effect_numbers({fields[cursor++]}, name, 0, count);
    };
    auto integer = [&](const std::string& name) -> int32_t
    {
        const double value = numbers(name, 1)[0];
        if(std::floor(value) != value || value < INT32_MIN || value > INT32_MAX)
            throw std::runtime_error("invalid shader constant integer: " + name);
        return static_cast<int32_t>(value);
    };
    auto color = [&]()
    {
        const auto values = numbers("PSConstValue", 4);
        math::vec4 value{};
        for(int channel = 0; channel < 4; ++channel)
        {
            value[channel] = static_cast<float>(values[channel]);
            if(!std::isfinite(value[channel])) throw std::runtime_error("shader constant outside float range");
        }
        return value;
    };
    const int32_t count = integer("PSConstCount");
    if(count < 0 || count > 32) throw std::runtime_error("invalid shader constant count");
    std::set<uint32_t> indices;
    std::vector<pw_shader_constant> result;
    for(int32_t index = 0; index < count; ++index)
    {
        pw_shader_constant constant;
        const int32_t slot = integer("PSConstIndex");
        if(slot < 0 || slot >= 32 || !indices.insert(static_cast<uint32_t>(slot)).second)
            throw std::runtime_error("invalid or duplicate shader constant slot");
        constant.index = static_cast<uint32_t>(slot);
        constant.initial = color();
        constant.loops = integer("PSLoopCount");
        const int32_t targets = integer("PSTargetCount");
        if(targets < 0 || targets > 4096) throw std::runtime_error("invalid shader target count");
        for(int32_t target = 0; target < targets; ++target)
        {
            const int32_t interval = integer("PSInterval");
            if(interval == INT32_MIN) throw std::runtime_error("shader interval magnitude outside native range");
            constant.targets.push_back({interval, color()});
        }
        result.push_back(std::move(constant));
    }
    return result;
}

auto evaluate_pw_effect_shader_constant(const pw_shader_constant& constant, uint64_t elapsed_ms) -> math::vec4
{
    if(constant.targets.empty()) return constant.initial;
    uint64_t total_ms = 0;
    for(const auto& target : constant.targets)
    {
        if(target.interval_ms == 0) { total_ms = 0; break; }
        total_ms += static_cast<uint64_t>(std::abs(static_cast<int64_t>(target.interval_ms)));
    }
    uint64_t local_ms = elapsed_ms;
    if(total_ms != 0)
    {
        if(constant.loops >= 0 && elapsed_ms / total_ms >= static_cast<uint64_t>(constant.loops))
            return constant.targets.back().value;
        local_ms %= total_ms;
    }
    uint64_t endpoint = 0;
    math::vec4 previous = constant.initial;
    for(const auto& target : constant.targets)
    {
        if(target.interval_ms == 0) return previous + target.value * (static_cast<float>(local_ms) * 0.001f);
        const uint64_t interval = static_cast<uint64_t>(std::abs(static_cast<int64_t>(target.interval_ms)));
        endpoint += interval;
        if(local_ms <= endpoint)
        {
            if(target.interval_ms < 0) return previous;
            return math::mix(previous, target.value, 1.0f - static_cast<float>(endpoint - local_ms) / static_cast<float>(interval));
        }
        previous = target.value;
    }
    return constant.targets.back().value;
}

auto pw_screen_warp_strength(const math::vec2& projected_size, float alpha) -> math::vec2
{
    const math::vec2 size = math::abs(projected_size);
    const float attenuation = std::clamp(std::min(1.0f - size.x * size.x * 4.0f, 1.0f - size.y * size.y * 4.0f), 0.0f, 1.0f);
    if(attenuation < 0.1f) return {};
    return math::clamp(size * (alpha * attenuation), math::vec2(0), math::vec2(0.5f));
}
} // namespace unravel
