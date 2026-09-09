#include "pw_map_effects.h"
#include "pw_effect_geometry.h"
#include "pw_effect_shader.h"
#include "pw_effect_model.h"

#include <engine/assets/asset_manager.h>
#include <engine/ecs/components/id_component.h>
#include <engine/ecs/components/transform_component.h>
#include <engine/ecs/ecs.h>
#include <engine/ecs/scene.h>
#include <engine/rendering/mesh.h>
#include <graphics/shader.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace unravel
{
auto make_pw_effect_ring(float radius, float height, float pitch, uint32_t sectors, bool centered)
    -> std::vector<std::array<float, 3>>
{
    if(!std::isfinite(radius) || !std::isfinite(height) || !std::isfinite(pitch) || radius < 0 || height < 0)
        throw std::runtime_error("invalid native ring dimensions");
    if(sectors == 0) sectors = 36; // A3DGFXRing::_default_sects.
    if(sectors < 3 || sectors > 65535) throw std::runtime_error("invalid native ring sector count");
    const float half_height = height * 0.5f;
    const float delta_radius = math::clamp(half_height * std::tan(pitch), -radius, radius);
    std::vector<std::array<float, 3>> result;
    result.reserve((sectors + 1) * 2);
    for(uint32_t index = 0; index < sectors; ++index)
    {
        const float angle = float(index) * math::two_pi<float>() / float(sectors);
        result.push_back({(radius + delta_radius) * std::cos(angle), centered ? -half_height : 0,
                          (radius + delta_radius) * std::sin(angle)});
        result.push_back({(radius - delta_radius) * std::cos(angle), centered ? half_height : height,
                          (radius - delta_radius) * std::sin(angle)});
    }
    result.push_back(result[0]);
    result.push_back(result[1]);
    return result;
}

auto sample_pw_effect_box(const std::array<float, 3>& size, bool surface, const std::array<float, 4>& random)
    -> std::array<float, 3>
{
    std::array<float, 3> point{random[1] * 2 - 1, random[2] * 2 - 1, random[3] * 2 - 1};
    for(const float extent : size)
        if(!std::isfinite(extent) || extent < 0) throw std::runtime_error("invalid native box dimensions");
    if(surface)
    {
        const float xy = size[0] * size[1], yz = size[1] * size[2], zx = size[2] * size[0];
        if(xy + yz + zx <= 0) throw std::runtime_error("native box surface has no area");
        // Preserve the native GenSurface face selection, including its second threshold.
        const size_t axis = random[0] < xy / (xy + yz + zx) ? 2 :
                            random[0] < yz / (xy + yz + zx) ? 0 : 1;
        point[axis] = random[axis + 1] < 0.5f ? -1.0f : 1.0f;
    }
    for(size_t axis = 0; axis < 3; ++axis) point[axis] *= size[axis] * 0.5f;
    return point;
}

namespace
{
using fields = std::vector<pw_effect_field>;
using entity_index = std::unordered_map<hpp::uuid, entt::handle>;

auto resolve_entity(const entity_index& index, const hpp::uuid& id) -> entt::handle
{
    const auto found = index.find(id);
    return found == index.end() ? entt::handle{} : found->second;
}

auto scalar(const fields& source, const std::string& key, size_t occurrence = 0) -> float
{
    const double number = read_pw_effect_numbers(source, key, occurrence, 1)[0];
    const float result = static_cast<float>(number);
    if(!std::isfinite(result)) throw std::runtime_error("authored float outside native range: " + key);
    return result;
}

auto optional_scalar(const fields& source, const std::string& key, float native_default, size_t occurrence = 0) -> float
{
    const auto found = std::find_if(source.begin(), source.end(), [&](const auto& field) { return field.name == key; });
    return found == source.end() ? native_default : scalar(source, key, occurrence);
}

auto vector3(const fields& source, const std::string& key, size_t occurrence = 0) -> math::vec3
{
    const auto value = read_pw_effect_numbers(source, key, occurrence, 3);
    return {static_cast<float>(value[0]), static_cast<float>(value[1]), static_cast<float>(value[2])};
}

auto text_field(const fields& source, const std::string& key) -> std::string
{
    for(const auto& field : source) if(field.name == key) return field.value;
    return {};
}

auto direction_frame(const math::vec3& forward, const math::vec3& authored_up) -> math::quat
{
    if(math::length(forward) < 0.000001f) throw std::runtime_error("zero native effect forward");
    const auto z = math::normalize(forward);
    auto up = authored_up;
    if(math::length(math::cross(up, z)) < 0.000001f)
        up = std::abs(z.y) < 0.99f ? math::vec3(0, 1, 0) : math::vec3(1, 0, 0);
    const auto x = math::normalize(math::cross(up, z));
    const auto y = math::normalize(math::cross(z, x));
    return math::normalize(math::quat_cast(math::mat3(x, y, z)));
}

auto native_perpendicular(const math::vec3& direction) -> math::vec3
{
    return math::normalize(std::abs(direction.y) > 0.999f ? math::cross(direction, math::vec3(1, 0, 0)) :
                                                         math::cross(math::vec3(0, 1, 0), direction));
}

auto native_direction_frame(const math::vec3& direction) -> math::quat
{
    const auto z = math::normalize(direction);
    math::vec3 x, y;
    if(std::abs(z.y) > 0.999f) { y = native_perpendicular(z); x = math::cross(y, z); }
    else { x = native_perpendicular(z); y = math::cross(z, x); }
    return math::normalize(math::quat_cast(math::mat3(x, y, z)));
}

auto native_noise(const pw_effect_controller& controller, float x, size_t channel = 0) -> float
{
    float result = 0;
    const int64_t count = static_cast<int64_t>(controller.noise_values.size());
    if(count == 0) throw std::runtime_error("native noise table was not prepared");
    for(const auto& octave : controller.noise_octaves)
    {
        const float position = octave[0] + x / octave[1];
        const int64_t first = static_cast<int64_t>(position);
        const float fraction = position - static_cast<float>(first);
        const size_t a = static_cast<size_t>((first % count + count) % count);
        const size_t b = (a + 1) % static_cast<size_t>(count);
        // APerlinNoiseBase defaults to turbulence (absolute octave values).
        result += octave[2] * std::abs(math::mix(controller.noise_values[a][channel],
                                                controller.noise_values[b][channel], fraction));
    }
    return result;
}

auto color_channels(uint32_t color) -> math::vec4
{
    return {float((color >> 16) & 255u), float((color >> 8) & 255u), float(color & 255u), float(color >> 24)};
}

auto authored_color(const fields& source, const std::string& key) -> math::vec4
{
    const auto value = read_pw_effect_numbers(source, key, 0, 1)[0];
    if(std::floor(value) != value || value < INT32_MIN || value > UINT32_MAX)
        throw std::runtime_error("invalid native ARGB field: " + key);
    return color_channels(static_cast<uint32_t>(static_cast<int64_t>(value)));
}

struct native_state
{
    math::vec3 position{};
    math::quat direction = math::identity<math::quat>();
    math::vec4 color{255.0f};
    float scale = 1;
    float rotation = 0;
    float scale_noise = 0;
    math::vec3 axis_offset{};
};

auto keypoint_state(const pw_effect_keypoint& keypoint) -> native_state
{
    native_state result;
    result.position = {keypoint.position[0], keypoint.position[1], keypoint.position[2]};
    result.direction = {keypoint.direction[3], keypoint.direction[0], keypoint.direction[1], keypoint.direction[2]};
    result.color = color_channels(keypoint.color_argb);
    result.scale = keypoint.scale;
    result.rotation = keypoint.rotation_2d;
    return result;
}

auto keypoint_rotation(const native_state& keypoint) -> math::quat
{ return keypoint.direction * math::angleAxis(keypoint.rotation, math::vec3(0, 0, 1)); }

void apply_controller(const pw_effect_controller& controller, native_state& value, float elapsed, float delta,
                      float duration_seconds)
{
    // A3DGFXKeyPointCtrlBase::Tick gates against the keypoint/particle elapsed time.
    if(elapsed < controller.start_seconds || (controller.end_seconds >= 0 && elapsed > controller.end_seconds)) return;
    const auto& source = controller.fields;
    switch(controller.type)
    {
        case 100:
        {
            const float velocity = scalar(source, "Vel");
            const float acceleration = scalar(source, "Acc");
            const auto offset = vector3(source, "Dir") * ((velocity + acceleration * (elapsed - delta * 0.5f)) * delta);
            value.position += offset;
            value.axis_offset += offset;
            break;
        }
        case 101:
            value.rotation += (scalar(source, "Vel") + scalar(source, "Acc") * (elapsed - delta * 0.5f)) * delta;
            break;
        case 102:
        {
            const float distance = (scalar(source, "Vel") + scalar(source, "Acc") * (elapsed - delta * 0.5f)) * delta;
            const auto axis = math::normalize(vector3(source, "Axis"));
            const auto rotation = math::angleAxis(distance, axis);
            const auto center = vector3(source, "Pos") + value.axis_offset;
            value.position = center + rotation * (value.position - center);
            value.direction = math::normalize(rotation * value.direction);
            break;
        }
        case 104:
        {
            const auto center = vector3(source, "Pos") + value.axis_offset;
            const auto direction = value.position - center;
            const float length = math::length(direction);
            const float distance = (scalar(source, "Vel") + scalar(source, "Acc") * (elapsed - delta * 0.5f)) * delta;
            if(length > 0) value.position = distance < -length ? center : value.position + direction * (distance / length);
            break;
        }
        case 105:
        {
            const auto change = read_pw_effect_numbers(source, "ColorDelta", 0, 4);
            for(int channel = 0; channel < 4; ++channel)
                value.color[channel] = std::clamp(value.color[channel] + static_cast<float>(change[channel]) * delta,
                                                   0.0f, 255.0f);
            break;
        }
        case 106:
        {
            const auto change = read_pw_effect_numbers(source, "ScaleChage", 0, 3);
            value.scale = std::clamp(value.scale + static_cast<float>(change[0]) * delta,
                                     static_cast<float>(change[1]), static_cast<float>(change[2]));
            break;
        }
        case 107:
            value.color = authored_color(source, "BaseColor");
            value.color.w = std::clamp(value.color.w + float(int(native_noise(controller, elapsed * 1000))), 0.0f, 255.0f);
            break;
        case 109:
            value.scale_noise = native_noise(controller, elapsed * 1000);
            break;
        case 110:
        {
            const auto& samples = controller.curve_samples;
            if(samples.size() < 2) break;
            const float portion = duration_seconds > 0 ? std::min(1.0f, elapsed / duration_seconds) : 1;
            size_t segment = 0;
            while(segment + 2 < samples.size() && portion > samples[segment + 1][3]) ++segment;
            const auto& first = samples[segment];
            const auto& next = samples[segment + 1];
            const float span = next[3] - first[3];
            const float fraction = span > 0 ? (portion - first[3]) / span : 0;
            const math::vec3 start(first[0], first[1], first[2]), end(next[0], next[1], next[2]);
            const auto position = math::mix(start, end, fraction);
            value.axis_offset += position - value.position;
            value.position = position;
            if(optional_scalar(source, "CalcDir", 0) != 0 && math::length(end - start) > 0)
            {
                const auto first_direction = native_direction_frame(end - start);
                const size_t following = std::min(segment + 2, samples.size() - 1);
                const auto& after = samples[following];
                const math::vec3 next_direction(after[0] - next[0], after[1] - next[1], after[2] - next[2]);
                value.direction = math::length(next_direction) > 0 ?
                    math::normalize(math::slerp(first_direction, native_direction_frame(next_direction), fraction)) : first_direction;
            }
            break;
        }
        case 108:
        case 111:
        {
            const int count = static_cast<int>(scalar(source, "Count"));
            if(count <= 0) break;
            float span = elapsed * 1000.0f;
            int segment = 0;
            float duration = scalar(source, "TimeSpan", 0);
            while(segment + 1 < count && span >= duration)
            {
                span -= duration;
                duration = scalar(source, "TimeSpan", ++segment);
            }
            const float portion = duration > 0 ? std::clamp(span / duration, 0.0f, 1.0f) : 1.0f;
            if(controller.type == 111)
                value.scale = math::mix(scalar(source, "Scale", segment), scalar(source, "Scale", segment + 1), portion);
            else
            {
                const auto first = read_pw_effect_numbers(source, "Color", segment, 1)[0];
                const auto second = read_pw_effect_numbers(source, "Color", segment + 1, 1)[0];
                const auto color = math::mix(color_channels(static_cast<uint32_t>(static_cast<int64_t>(first))),
                                             color_channels(static_cast<uint32_t>(static_cast<int64_t>(second))), portion);
                if(optional_scalar(source, "AlphaOnly", 0) != 0) value.color.w = color.w;
                else value.color = color;
            }
            break;
        }
        default:
            throw std::runtime_error("native GFX controller renderer unavailable: " + std::to_string(controller.type));
    }
}

struct native_program_state;
struct native_particle
{
    native_state state;
    math::vec3 velocity_direction{};
    float age = 0;
    float self_speed = 0;
    float acceleration_speed = 0;
    math::vec3 previous_position{};
    std::shared_ptr<native_program_state> child;
};

struct sprite_spec
{
    bool particle = false;
    bool bind = true;
    bool surface = false;
    bool three_dimensional = false;
    bool facing = false;
    bool grid = false;
    bool ring = false;
    bool trail = false;
    bool lightning = false;
    bool container = false;
    bool model = false;
    bool dummy = false;
    bool rotate_from_view = false;
    bool affected_by_scale = true;
    bool no_width_scale = false;
    bool no_height_scale = false;
    uint32_t capacity = 1;
    int quota = -1;
    float width = 1;
    float height = 1;
    float rate = 0;
    float lifetime = 0;
    float angle = 0;
    float speed = 0;
    float self_acceleration = 0;
    float acceleration = 0;
    math::vec3 acceleration_direction{};
    math::vec3 area_size{};
    float scale_min = 1;
    float scale_max = 1;
    float rotation_min = 0;
    float rotation_max = 0;
    math::vec4 color_min{255.0f};
    math::vec4 color_max{255.0f};
    math::vec2 pivot{0.5f};
    int source_blend = 5;
    int destination_blend = 6;
    uint32_t grid_width = 0;
    uint32_t grid_height = 0;
    std::vector<pw_effect_vertex> grid_vertices;
    std::vector<std::array<float, 3>> ring_vertices;
    math::vec3 trail_origin[2]{};
    float trail_lifetime = 0;
};

auto compile_sprite(const pw_effect_element& element) -> sprite_spec
{
    sprite_spec result;
    result.particle = element.type == 120 || element.type == 121 || element.type == 123 || element.type == 124;
    result.grid = element.type == 210;
    result.ring = element.type == 140;
    result.trail = element.type == 110;
    result.lightning = element.type == 150;
    result.container = element.type == 200;
    result.model = element.type == 160;
    if(!result.particle && !result.grid && !result.ring && !result.trail && !result.lightning && !result.container && !result.model &&
       element.type != 100 && element.type != 101 && element.type != 102)
        throw std::runtime_error("native GFX renderer unavailable for type " + std::to_string(element.type));
    const auto& base = element.base_fields;
    const auto& data = element.type_fields;
    result.dummy = optional_scalar(base, "IsDummy", 0) != 0;
    result.rotate_from_view = optional_scalar(data, "RotFromView", 0) != 0;
    result.source_blend = static_cast<int>(scalar(base, "SrcBlend"));
    result.destination_blend = static_cast<int>(scalar(base, "DestBlend"));
    if(result.source_blend < 1 || result.source_blend > 11 || result.destination_blend < 1 || result.destination_blend > 11)
        throw std::runtime_error("unsupported native GFX blend factor");
    if(result.particle || (!result.grid && !result.ring && !result.trail && !result.container && !result.model && !result.lightning))
    {
        result.width = scalar(data, result.particle ? "ParticleWidth" : "Width");
        result.height = scalar(data, result.particle ? "ParticleHeight" : "Height");
    }
    if(result.ring)
    {
        result.ring_vertices = make_pw_effect_ring(scalar(data, "Radius"), scalar(data, "Height"), scalar(data, "Pitch"),
            static_cast<uint32_t>(scalar(data, "Sects")), scalar(data, "OrgAtCenter") != 0);
        result.no_width_scale = scalar(data, "NoRadScale") != 0;
        result.no_height_scale = scalar(data, "NoHeiScale") != 0;
    }
    if(result.trail)
    {
        result.trail_origin[0] = vector3(data, "OrgPos1");
        result.trail_origin[1] = vector3(data, "OrgPos2");
        result.trail_lifetime = scalar(data, "SegLife") / 1000.0f;
        if(result.trail_lifetime <= 0 || scalar(data, "EnableMat") != 0 || scalar(data, "EnableOrgPos1") != 0 ||
           scalar(data, "EnableOrgPos2") != 0 || scalar(data, "Bind") != 0 || scalar(data, "Spline") != 0)
            throw std::runtime_error("native trail requires supported authored line mode and local origins");
    }
    if(result.grid)
    {
        result.grid_width = static_cast<uint32_t>(scalar(data, "wNumber"));
        result.grid_height = static_cast<uint32_t>(scalar(data, "hNumber"));
        if(result.grid_width < 2 || result.grid_height < 2 ||
           uint64_t(result.grid_width) * result.grid_height > data.size()) throw std::runtime_error("invalid authored grid dimensions");
        result.affected_by_scale = optional_scalar(data, "AffByScl", 1) != 0;
        if(optional_scalar(data, "keyNumber", 0) != 0) throw std::runtime_error("animated grid keys require native grid animation evaluation");
        size_t record = 0;
        while(record < data.size() && data[record].name != "") ++record;
        for(uint32_t index = 0; index < result.grid_width * result.grid_height; ++index)
        {
            if(record + 1 >= data.size() || data[record].name != "" || data[record + 1].name != "dwColor")
                throw std::runtime_error("malformed authored grid vertex");
            const auto p = read_pw_effect_numbers({data[record]}, "", 0, 3);
            size_t consumed = 0;
            const auto color = std::stoul(data[record + 1].value, &consumed, 16);
            if(consumed != data[record + 1].value.size() || color > UINT32_MAX) throw std::runtime_error("invalid grid ARGB");
            result.grid_vertices.push_back({{float(p[0]), float(p[1]), float(p[2])},
                {float(index % result.grid_width) / float(result.grid_width - 1), float(index / result.grid_width) / float(result.grid_height - 1)},
                color_channels(static_cast<uint32_t>(color)) / 255.0f});
            record += 2;
        }
    }
    result.pivot = {optional_scalar(data, "OrgPt", 0.5f, 0), optional_scalar(data, "OrgPt", 0.5f, 1)};
    if(!result.ring)
    {
        result.no_width_scale = optional_scalar(data, "NoScale", 0, 0) != 0;
        result.no_height_scale = optional_scalar(data, "NoScale", 0, 1) != 0;
    }
    if(result.particle)
    {
        result.rate = scalar(data, "EmissionRate");
        result.three_dimensional = scalar(data, "3DParticle") != 0;
        result.facing = scalar(data, "Facing") != 0;
        result.lifetime = scalar(data, "TTL");
        result.quota = static_cast<int>(scalar(data, "Quota"));
        if(result.rate < 0 || result.lifetime < 0 || double(result.rate) * result.lifetime >= UINT32_MAX)
            throw std::runtime_error("invalid native particle pool size");
        result.capacity = static_cast<uint32_t>(result.rate * result.lifetime) + 1;
        if(result.quota >= 0) result.capacity = std::min(result.capacity, static_cast<uint32_t>(result.quota));
        result.capacity = std::max(1u, result.capacity);
        result.angle = scalar(data, "Angle");
        result.speed = scalar(data, "Speed");
        result.self_acceleration = optional_scalar(data, "ParAcc", 0);
        result.acceleration = scalar(data, "Acc");
        result.acceleration_direction = vector3(data, "AccDir");
        result.bind = scalar(data, "IsBind") != 0;
        result.surface = scalar(data, "IsSurface") != 0;
        result.scale_min = scalar(data, "ScaleMin");
        result.scale_max = scalar(data, "ScaleMax");
        result.rotation_min = optional_scalar(data, "RotMin", 0);
        result.rotation_max = optional_scalar(data, "RotMax", 0);
        result.color_min = authored_color(data, "ColorMin");
        result.color_max = authored_color(data, "ColorMax");
        if(element.type != 120) result.area_size = vector3(data, "AreaSize");
    }
    for(const auto& point : element.keypoints)
        for(const auto& controller : point.controllers)
            if(controller.type != 100 && controller.type != 101 && controller.type != 102 && controller.type != 104 &&
               controller.type != 105 && controller.type != 106 && controller.type != 107 && controller.type != 108 &&
               controller.type != 109 && controller.type != 110 && controller.type != 111)
                throw std::runtime_error("native GFX controller renderer unavailable: " + std::to_string(controller.type));
    for(const auto& controller : element.particle_affectors)
        if(controller.type != 100 && controller.type != 101 && controller.type != 102 && controller.type != 104 &&
           controller.type != 105 && controller.type != 106 && controller.type != 107 && controller.type != 108 &&
           controller.type != 109 && controller.type != 110 && controller.type != 111)
            throw std::runtime_error("native particle affector renderer unavailable: " + std::to_string(controller.type));
    return result;
}

struct native_program;
struct native_element
{
    const pw_effect_element* source = nullptr;
    sprite_spec spec;
    hpp::uuid entity_id;
    std::vector<pw_effect_texture_state> textures;
    std::vector<pw_shader_constant> shader_constants;
    std::unique_ptr<native_program> child;
    std::unique_ptr<pw_effect_model_runtime> model;
    std::vector<hpp::uuid> model_entities;
    size_t bind = SIZE_MAX;
    size_t dummy = SIZE_MAX;
};
struct native_program
{
    const pw_effect_document* document = nullptr;
    std::vector<native_element> elements;
};
struct native_trail_segment
{
    math::vec3 first{}, second{};
    math::vec4 color{};
    float remaining = 0;
};
struct native_element_state
{
    native_state keypoint;
    std::vector<native_particle> particles;
    std::vector<native_trail_segment> trail;
    math::quat trail_last_rotation = math::identity<math::quat>();
    math::vec3 trail_last_position{};
    bool trail_started = false;
    std::shared_ptr<native_program_state> child;
    float age = 0;
    float local_time = 0;
    float texture_time = 0;
    float emission_remainder = 0;
    uint32_t emitted = 0;
    uint32_t cycle = 0;
    bool active = false;
    uint64_t lightning_epoch = UINT64_MAX;
    float lightning_offset = 0;
    std::mt19937 random;
};
struct native_program_state
{
    std::vector<native_element_state> elements;
};
struct native_frame
{
    math::vec3 position{};
    math::quat rotation = math::identity<math::quat>();
    float scale = 1;
    float alpha = 1;
    math::vec4 outer_color{1};
};

auto point_world(const native_frame& frame, const math::vec3& point) -> math::vec3
{ return frame.position + frame.rotation * (point * frame.scale); }

auto create_program(const pw_effect_document& document, const std::string& protocol_root) -> std::unique_ptr<native_program>
{
    auto result = std::make_unique<native_program>();
    result->document = &document;
    result->elements.reserve(document.elements.size());
    for(const auto& source : document.elements)
    {
        native_element element;
        element.source = &source;
        element.spec = compile_sprite(source);
        if(element.spec.lightning) sample_pw_effect_lightning_amplitude(source, document.version, 0);
        const bool has_shader_constants = std::any_of(source.base_fields.begin(), source.base_fields.end(),
            [](const auto& field) { return field.name == "PSConstCount"; });
        // Earlier native GFX versions have no pixel-shader section at all.
        if(has_shader_constants || !text_field(source.base_fields, "ShaderFile").empty())
            element.shader_constants = parse_pw_effect_shader_constants(source.base_fields);
        if(source.keypoints.size() != 1 || source.keypoints[0].interpolation != 0)
            throw std::runtime_error("native effect requires implemented authored keypoint interpolation");
        for(const auto& dependency : source.dependencies)
        {
            if(dependency.kind == "model")
            {
                if(!dependency.model) throw std::runtime_error("native model was not prepared on the worker");
                element.model = std::make_unique<pw_effect_model_runtime>();
                element.model->begin(dependency.model, protocol_root);
            }
            if(dependency.kind == "gfx")
            {
                if(!dependency.nested) throw std::runtime_error("nested GFX was not prepared on the worker");
                if(element.child) throw std::runtime_error("multiple nested GFX dependencies for one element");
                element.child = create_program(*dependency.nested, protocol_root);
            }
        }
        if(element.spec.container && !element.child) throw std::runtime_error("GFX container dependency missing");
        result->elements.push_back(std::move(element));
    }
    for(auto& element : result->elements)
    {
        const auto resolve = [&](const char* key) -> size_t
        {
            const auto name = text_field(element.source->base_fields, key);
            if(name.empty()) return SIZE_MAX;
            for(size_t index = 0; index < result->elements.size(); ++index)
                // Bind and dummy names use exact legacy bytes, not display aliases.
                if(text_field(result->elements[index].source->base_fields, "Name") == name) return index;
            throw std::runtime_error(std::string("GFX ") + key + " target absent");
        };
        element.bind = resolve("BindEle");
        element.dummy = resolve("DummyEle");
        if(element.dummy != SIZE_MAX && (!element.spec.particle || !result->elements[element.dummy].child))
            throw std::runtime_error("native particle dummy must reference a prepared GFX container");
    }
    return result;
}

auto create_state(const native_program& program, uint32_t seed) -> std::shared_ptr<native_program_state>
{
    auto state = std::make_shared<native_program_state>();
    state->elements.resize(program.elements.size());
    for(size_t index = 0; index < program.elements.size(); ++index)
    {
        const auto& definition = program.elements[index];
        auto& element = state->elements[index];
        element.keypoint = keypoint_state(definition.source->keypoints[0]);
        element.particles.reserve(definition.spec.capacity);
        element.random.seed(seed + static_cast<uint32_t>(index) * 1664525u);
        if(definition.child && !definition.spec.dummy) element.child = create_state(*definition.child, seed ^ uint32_t(index + 1));
    }
    return state;
}

void collect_elements(native_program& program, std::vector<native_element*>& output)
{
    for(auto& element : program.elements)
    {
        output.push_back(&element);
        if(element.child) collect_elements(*element.child, output);
    }
}

auto effect_uv(const pw_effect_element& element, float elapsed, math::vec2 uv) -> math::vec2
{
    const auto& fields = element.base_fields;
    const float rows = scalar(fields, "TexRow"), columns = scalar(fields, "TexCol");
    if(rows < 1 || columns < 1) throw std::runtime_error("invalid native texture sheet dimensions");
    if(optional_scalar(fields, "UReverse", 0) != 0) uv.x = 1 - uv.x;
    if(optional_scalar(fields, "VReverse", 0) != 0) uv.y = 1 - uv.y;
    math::vec2 offset{}, size{};
    if(optional_scalar(fields, "TileMode", 0) != 0)
    {
        size = {columns, rows};
        offset = {optional_scalar(fields, "TexSpeed", 0, 0) * elapsed,
                  optional_scalar(fields, "TexSpeed", 0, 1) * elapsed};
    }
    else
    {
        size = {1 / columns, 1 / rows};
        const float interval = scalar(fields, "TexInterval");
        const uint64_t frame = interval > 0 ? static_cast<uint64_t>(elapsed * 1000 / interval) : 0;
        offset = {float(frame % uint64_t(columns)) / columns,
                  float((frame / uint64_t(columns)) % uint64_t(rows)) / rows};
    }
    uv = uv * size + offset;
    if(optional_scalar(fields, "UVExchg", 0) != 0) std::swap(uv.x, uv.y);
    return uv;
}

void configure_geometry(pw_effect_geometry_component& geometry, const native_element& element, float elapsed)
{
    const auto& source = *element.source;
    if(!element.textures.empty()) geometry.texture = element.textures.front().handle;
    geometry.source_blend = element.spec.source_blend;
    geometry.destination_blend = element.spec.destination_blend;
    geometry.render_layer = int(optional_scalar(source.base_fields, "RenderLayer", 0));
    geometry.depth_test = optional_scalar(source.base_fields, "ZEnable", 1) != 0;
    if(optional_scalar(source.base_fields, "Warp", 0) != 0) geometry.shader = pw_effect_shader_mode::warp;
    const std::string shader = text_field(source.base_fields, "ShaderFile");
    if(!shader.empty())
    {
        const auto implementation = std::find_if(source.dependencies.begin(), source.dependencies.end(),
            [](const auto& dependency) { return dependency.kind == "shader" && dependency.implementation == "angelica-fluid-v1"; });
        if(implementation == source.dependencies.end()) throw std::runtime_error("native pixel shader descriptor is not verified");
        geometry.shader = pw_effect_shader_mode::fluid;
        if(element.textures.empty()) throw std::runtime_error("native fluid shader texture is absent");
        geometry.shader_texture = element.textures.back().handle;
    }
    for(const auto& constant : element.shader_constants)
    {
        if(constant.index == 0) geometry.flow_offset = evaluate_pw_effect_shader_constant(constant, uint64_t(elapsed * 1000));
        if(constant.index == 1) geometry.flow_blend = evaluate_pw_effect_shader_constant(constant, uint64_t(elapsed * 1000));
    }
}

auto random_float(std::mt19937& random, float minimum, float maximum) -> float
{ return math::mix(minimum, maximum, float(random()) / float(std::mt19937::max())); }

auto spawn_particle(native_element_state& state, const sprite_spec& spec, int type) -> native_particle
{
    native_particle result;
    const auto random = [&](float minimum, float maximum) { return random_float(state.random, minimum, maximum); };
    result.state.scale = random(spec.scale_min, spec.scale_max);
    result.state.rotation = random(spec.rotation_min, spec.rotation_max);
    for(int channel = 0; channel < 4; ++channel) result.state.color[channel] = random(spec.color_min[channel], spec.color_max[channel]);
    result.self_speed = spec.speed;
    result.velocity_direction = {0, 0, 1};
    if(spec.angle != 0)
    {
        const float azimuth = random(0, math::pi<float>() * 2), cone = random(0, spec.angle);
        result.velocity_direction = {std::cos(azimuth) * std::sin(cone), std::sin(azimuth) * std::sin(cone), std::cos(cone)};
    }
    if(type == 120) result.state.position = result.velocity_direction * 0.001f;
    else if(type == 121)
    {
        const auto point = sample_pw_effect_box({spec.area_size.x, spec.area_size.y, spec.area_size.z}, spec.surface,
                                                {random(0, 1), random(0, 1), random(0, 1), random(0, 1)});
        result.state.position = {point[0], point[1], point[2]};
    }
    else
    {
        math::vec3 point;
        if(spec.surface)
        {
            if(type == 124)
            {
                const float angle = random(0, math::pi<float>() * 2);
                point = {std::cos(angle), std::sin(angle), random(-1, 1)};
            }
            else
            {
                const float angle = random(0, math::pi<float>() * 2), azimuth = random(0, math::pi<float>() * 2);
                point = {std::sin(angle) * std::cos(azimuth), std::cos(angle), std::sin(angle) * std::sin(azimuth)};
                result.velocity_direction = -point;
            }
        }
        else
        {
            do { point = {random(-1, 1), random(-1, 1), random(-1, 1)}; }
            while(point.x * point.x + point.y * point.y + (type == 123 ? point.z * point.z : 0) > 1);
        }
        result.state.position = point * spec.area_size * 0.5f;
    }
    result.previous_position = result.state.position;
    return result;
}

void tick_element(const native_element& definition, native_element_state& state, float delta)
{
    const auto& source = *definition.source;
    const auto& point = source.keypoints[0];
    state.age += delta;
    const float time = state.age - source.start_ms / 1000.0f;
    const float duration = point.duration_ms / 1000.0f;
    const float delay = optional_scalar(source.base_fields, "RepeatDelay", 0) / 1000.0f;
    const int repeat = int(scalar(source.base_fields, "RepeatCount"));
    const bool infinite = point.duration_ms == UINT32_MAX;
    uint32_t cycle = !infinite && duration + delay > 0 && time >= 0 ? uint32_t(time / (duration + delay)) : 0;
    state.local_time = !infinite && duration + delay > 0 ? time - cycle * (duration + delay) : time;
    state.active = time >= 0 && (repeat < 0 || cycle < uint32_t(repeat)) &&
                   (infinite || state.local_time < duration);
    if(cycle != state.cycle)
    {
        // ResumeLoop only runs when another authored repeat will actually start.
        // Finished emitters/trails retain their live particles/segments until expiry.
        if(repeat < 0 || cycle < uint32_t(repeat))
        {
            state.keypoint = keypoint_state(point);
            state.emitted = 0;
            state.emission_remainder = 0;
            state.particles.clear();
            state.trail.clear();
            state.trail_started = false;
            if(optional_scalar(source.base_fields, "ResetLoopEnd", 0) != 0) state.texture_time = 0;
            if(definition.child && optional_scalar(source.type_fields, "LoopFlag", 0) != 0)
                state.child = create_state(*definition.child, state.random());
        }
        state.cycle = cycle;
    }
    if(!state.active) return;
    state.texture_time += delta;
    const float step = std::min(delta, std::max(0.0f, state.local_time));
    for(const auto& controller : point.controllers)
        apply_controller(controller, state.keypoint, state.local_time, step, duration);
}

auto current_keypoint(const native_program& program, const native_program_state& state, size_t index,
                      size_t depth = 0) -> native_state
{
    if(depth >= program.elements.size()) throw std::runtime_error("cyclic native GFX BindEle");
    auto value = state.elements[index].keypoint;
    value.scale += value.scale_noise;
    const size_t binding = program.elements[index].bind;
    if(binding != SIZE_MAX)
    {
        const auto bound = current_keypoint(program, state, binding, depth + 1);
        value.position = bound.position;
        value.direction = bound.direction;
    }
    return value;
}

void draw_program(native_program& program, native_program_state& state, const entity_index& entities,
                  const native_frame& parent, float delta);

void draw_particles(native_program& program, size_t index, native_element_state& state, const entity_index& entities,
                    const native_frame& parent, const native_state& keypoint, float delta,
                    pw_effect_geometry_component& geometry)
{
    auto& definition = program.elements[index];
    const auto& spec = definition.spec;
    const auto& source = *definition.source;
    if(!spec.bind) throw std::runtime_error("unbound native particle placement requires world spawn state");
    if(optional_scalar(source.type_fields, "IsDrag", 0) != 0 ||
       optional_scalar(source.type_fields, "IsAvgGen", 0) != 0 ||
       optional_scalar(source.type_fields, "IsUseHSVInterp", 0) != 0)
        throw std::runtime_error("native particle authored mode has no implemented evaluator");
    const auto rotation = parent.rotation * keypoint_rotation(keypoint);
    const float scale = parent.scale * keypoint.scale;
    state.particles.erase(std::remove_if(state.particles.begin(), state.particles.end(),
        [&](const auto& particle) { return particle.age + delta >= spec.lifetime; }), state.particles.end());
    if(state.active)
    {
        state.emission_remainder += spec.rate * delta;
        uint32_t emit = uint32_t(state.emission_remainder);
        state.emission_remainder -= float(emit);
        emit = std::min(emit, spec.capacity - uint32_t(state.particles.size()));
        if(spec.quota >= 0) emit = std::min(emit, uint32_t(spec.quota) - std::min(uint32_t(spec.quota), state.emitted));
        for(uint32_t count = 0; count < emit; ++count)
        {
            auto particle = spawn_particle(state, spec, source.type);
            if(spec.color_min != spec.color_max) particle.state.color.w *= keypoint.color.w / 255.0f;
            if(definition.dummy != SIZE_MAX)
                particle.child = create_state(*program.elements[definition.dummy].child, state.random());
            state.particles.push_back(std::move(particle));
            ++state.emitted;
        }
    }
    // Native order is Expire -> TriggerEmitter -> TriggerAffectors/ApplyMotion.
    for(auto& particle : state.particles)
    {
        particle.previous_position = particle.state.position;
        particle.age += delta;
        for(const auto& controller : source.particle_affectors)
            apply_controller(controller, particle.state, particle.age, delta, spec.lifetime);
        const float self_end = particle.self_speed + spec.self_acceleration * delta;
        const float acceleration_end = particle.acceleration_speed + spec.acceleration * delta;
        const auto offset = particle.velocity_direction * ((particle.self_speed + self_end) * delta * 0.5f) +
                            spec.acceleration_direction * ((particle.acceleration_speed + acceleration_end) * delta * 0.5f);
        particle.state.position += offset;
        particle.state.axis_offset += offset;
        particle.self_speed = self_end;
        particle.acceleration_speed = acceleration_end;
    }
    std::array<math::vec2, 4> particle_uv = spec.three_dimensional ?
        std::array<math::vec2, 4>{{{1, 1}, {1, 0}, {0, 0}, {0, 1}}} :
        std::array<math::vec2, 4>{{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
    for(auto& uv : particle_uv) uv = effect_uv(source, state.texture_time, uv);
    for(auto& particle : state.particles)
    {
        const auto position = point_world(parent, (keypoint.position + keypoint_rotation(keypoint) * particle.state.position) * keypoint.scale);
        const float particle_scale = particle.state.scale + particle.state.scale_noise;
        auto color = particle.state.color / 255.0f * parent.outer_color;
        color.w *= parent.alpha * keypoint.color.w / 255.0f;
        if(definition.dummy != SIZE_MAX)
        {
            auto& target = program.elements[definition.dummy];
            auto child_rotation = rotation * particle.state.direction * math::angleAxis(particle.state.rotation, math::vec3(0, 0, 1));
            if(spec.facing)
            {
                auto motion = particle.state.position - particle.previous_position;
                if(math::length(motion) < 0.000001f) motion = particle.velocity_direction;
                child_rotation = native_direction_frame(parent.rotation * (keypoint.direction * motion));
            }
            native_frame child_frame{position, child_rotation,
                particle_scale * (optional_scalar(target.source->type_fields, "DummyUseGScale", 1) ? parent.scale : 1),
                color.w, parent.outer_color};
            if(optional_scalar(target.source->type_fields, "OutColor", 0) != 0) child_frame.outer_color = color;
            child_frame.scale *= target.child->document->default_scale;
            child_frame.alpha *= target.child->document->default_alpha;
            draw_program(*target.child, *particle.child, entities, child_frame,
                delta * optional_scalar(target.source->type_fields, "PlaySpeed", 1) * target.child->document->default_speed);
            continue;
        }
        pw_effect_quad quad;
        quad.position = position;
        quad.camera_facing = !spec.three_dimensional;
        quad.warp_screen_space = !spec.three_dimensional;
        quad.velocity_facing = spec.three_dimensional && spec.facing;
        quad.right = rotation * (particle.state.direction * math::vec3(1, 0, 0));
        quad.up = rotation * (particle.state.direction * math::vec3(0, 1, 0));
        if(quad.velocity_facing)
        {
            auto motion = particle.previous_position - particle.state.position;
            if(math::length(motion) < 0.000001f) motion = particle.velocity_direction;
            quad.right = rotation * math::normalize(motion);
        }
        const float width = spec.no_width_scale ? spec.width : spec.width * particle_scale;
        const float height = spec.no_height_scale ? spec.height : spec.height * particle_scale;
        quad.size = {2 * width * (spec.three_dimensional || !spec.no_width_scale ? scale : 1),
                     2 * height * (spec.three_dimensional || !spec.no_height_scale ? scale : 1)};
        quad.pivot = spec.pivot;
        if(!spec.three_dimensional) quad.pivot.y = 1 - quad.pivot.y;
        quad.color = color;
        quad.rotation = quad.velocity_facing ? 0 : particle.state.rotation;
        quad.uv = particle_uv;
        geometry.quads.push_back(quad);
    }
}

void draw_ring(const native_element& definition, const native_state& keypoint, const native_frame& parent,
               float elapsed, pw_effect_geometry_component& geometry)
{
    const auto& spec = definition.spec;
    const float radius_scale = spec.no_width_scale ? 1 : keypoint.scale;
    const float height_scale = spec.no_height_scale ? 1 : keypoint.scale;
    // A3DGFXRing rotates Rad_2D about local Y, unlike decal/particle local Z.
    const auto rotation = keypoint.direction * math::angleAxis(keypoint.rotation, math::vec3(0, 1, 0));
    auto color = keypoint.color / 255.0f * parent.outer_color;
    color.w *= parent.alpha;
    std::vector<pw_effect_vertex> strip;
    strip.reserve(spec.ring_vertices.size());
    const size_t sectors = spec.ring_vertices.size() / 2 - 1;
    for(size_t index = 0; index < spec.ring_vertices.size(); ++index)
    {
        const auto& value = spec.ring_vertices[index];
        const math::vec3 local(value[0] * radius_scale, value[1] * height_scale, value[2] * radius_scale);
        strip.push_back({point_world(parent, keypoint.position + rotation * local),
            effect_uv(*definition.source, elapsed, {float(index / 2) / float(sectors), index % 2 ? 0.0f : 1.0f}), color});
    }
    for(size_t index = 0; index + 3 < strip.size(); index += 2)
        for(const size_t offset : {size_t(0), size_t(1), size_t(2), size_t(2), size_t(1), size_t(3)})
            geometry.triangles.push_back(strip[index + offset]);
}

void draw_trail(const native_element& definition, native_element_state& state, const native_state& keypoint,
                const native_frame& parent, float delta, pw_effect_geometry_component& geometry)
{
    const auto& spec = definition.spec;
    for(auto& segment : state.trail) segment.remaining -= delta;
    state.trail.erase(std::remove_if(state.trail.begin(), state.trail.end(),
        [](const auto& segment) { return segment.remaining <= 0; }), state.trail.end());
    if(state.active && delta > 0)
    {
        const auto rotation = parent.rotation * keypoint_rotation(keypoint);
        const auto position = point_world(parent, keypoint.position);
        const auto center = (spec.trail_origin[0] + spec.trail_origin[1]) * 0.5f;
        const auto half = (spec.trail_origin[1] - spec.trail_origin[0]) * (keypoint.scale * 0.5f);
        // Native LINE_MODE inserts extra segments at six-degree quaternion half-angle steps.
        const float angle = state.trail_started ? std::acos(math::clamp(std::abs(math::dot(state.trail_last_rotation, rotation)), 0.0f, 1.0f)) : 0;
        const int count = int(angle / math::radians(6.0f)) + 1;
        for(int index = 1; index <= count; ++index)
        {
            const float ratio = float(index) / float(count);
            const auto current_rotation = state.trail_started ? math::slerp(state.trail_last_rotation, rotation, ratio) : rotation;
            const auto current_position = state.trail_started ? math::mix(state.trail_last_position, position, ratio) : position;
            state.trail.push_back({current_position + current_rotation * ((center - half) * parent.scale),
                                   current_position + current_rotation * ((center + half) * parent.scale),
                                   keypoint.color / 255.0f, std::max(0.0f, spec.trail_lifetime - (1 - ratio) * delta)});
        }
        // A3DTrail's LineTrailList uses a 256-entry RotList, discarding the oldest entries.
        constexpr size_t native_segment_capacity = 256;
        if(state.trail.size() > native_segment_capacity)
            state.trail.erase(state.trail.begin(), state.trail.end() - native_segment_capacity);
        state.trail_last_position = position;
        state.trail_last_rotation = rotation;
        state.trail_started = true;
    }
    std::vector<pw_effect_vertex> strip;
    strip.reserve(state.trail.size() * 2);
    for(const auto& segment : state.trail)
    {
        const float fraction = segment.remaining / spec.trail_lifetime;
        auto color = segment.color * parent.outer_color;
        color.w *= fraction * parent.alpha;
        if(spec.source_blend == 2) // Native A3DBLEND_ONE premultiplies trail RGB by fading alpha.
            for(int channel = 0; channel < 3; ++channel) color[channel] *= segment.color.w * fraction * parent.alpha;
        strip.push_back({segment.first, effect_uv(*definition.source, state.texture_time, {0, fraction}), color});
        strip.push_back({segment.second, effect_uv(*definition.source, state.texture_time, {1, fraction}), color});
    }
    for(size_t index = 0; index + 3 < strip.size(); index += 2)
        for(const size_t offset : {size_t(0), size_t(1), size_t(2), size_t(2), size_t(1), size_t(3)})
            geometry.triangles.push_back(strip[index + offset]);
}

void draw_grid(const native_element& definition, const native_state& keypoint, const native_frame& parent,
               float elapsed, pw_effect_geometry_component& geometry)
{
    const auto& spec = definition.spec;
    const float scale = parent.scale * (spec.affected_by_scale ? keypoint.scale : 1);
    const auto color = keypoint.color / 255.0f * parent.outer_color;
    std::vector<pw_effect_vertex> vertices = spec.grid_vertices;
    for(auto& vertex : vertices)
    {
        vertex.position = parent.position + parent.rotation * ((keypoint.position + keypoint_rotation(keypoint) * vertex.position) *
                           (spec.affected_by_scale ? keypoint.scale : 1) * parent.scale);
        vertex.uv = effect_uv(*definition.source, elapsed, vertex.uv);
        vertex.color *= color;
        vertex.color.w *= parent.alpha;
    }
    geometry.rotate_from_view = spec.rotate_from_view;
    if(spec.rotate_from_view)
    {
        geometry.view_origin = point_world(parent, keypoint.position);
        geometry.view_axis = parent.rotation * (keypoint.direction * math::vec3(0, 0, 1));
        for(size_t index = 0; index < vertices.size(); ++index)
            vertices[index].position = spec.grid_vertices[index].position * scale;
    }
    for(uint32_t row = 0; row + 1 < spec.grid_height; ++row)
        for(uint32_t column = 0; column + 1 < spec.grid_width; ++column)
        {
            const uint32_t a = row * spec.grid_width + column, b = a + 1, c = a + spec.grid_width, d = c + 1;
            for(const uint32_t corner : {a, b, c, c, b, d}) geometry.triangles.push_back(vertices[corner]);
        }
}

auto curve_position(const pw_effect_controller& curve, float ratio) -> math::vec3
{
    const auto& samples = curve.curve_samples;
    if(samples.size() < 2) throw std::runtime_error("native lightning curve is empty");
    size_t index = 0;
    while(index + 2 < samples.size() && ratio > samples[index + 1][3]) ++index;
    const auto& a = samples[index];
    const auto& b = samples[index + 1];
    const float span = b[3] - a[3];
    return math::mix(math::vec3(a[0], a[1], a[2]), math::vec3(b[0], b[1], b[2]), span > 0 ? (ratio - a[3]) / span : 0);
}

void draw_lightning(const native_element& definition, int document_version, native_element_state& state, const native_state& keypoint,
                    const native_frame& parent, pw_effect_geometry_component& geometry)
{
    const auto& source = *definition.source;
    const auto& data = source.type_fields;
    const auto curve = std::find_if(source.keypoints[0].controllers.begin(), source.keypoints[0].controllers.end(),
        [](const auto& controller) { return controller.type == 110; });
    if(curve == source.keypoints[0].controllers.end()) throw std::runtime_error("native straight lightning evaluator required");
    if(optional_scalar(data, "WaveMoving", 0) != 0 || optional_scalar(data, "Pos1Enable", 0) != 0 ||
       optional_scalar(data, "Pos2Enable", 0) != 0) throw std::runtime_error("native externally driven lightning needs source positions");
    const uint32_t segments = uint32_t(scalar(data, "Segs"));
    if(segments < 2) throw std::runtime_error("native lightning needs at least two segments");
    const float interval = scalar(data, "Interval");
    const uint64_t epoch = interval > 0 ? uint64_t(state.local_time * 1000 / interval) : 0;
    if(epoch != state.lightning_epoch)
    {
        state.lightning_epoch = epoch;
        state.lightning_offset = float(state.random() % 1023);
    }
    std::vector<math::vec3> positions;
    positions.reserve(segments + 1);
    for(uint32_t index = 0; index <= segments; ++index) positions.push_back(curve_position(*curve, float(index) / segments));
    float length = 0;
    for(size_t index = 1; index < curve->curve_samples.size(); ++index)
    {
        const auto& a = curve->curve_samples[index - 1];
        const auto& b = curve->curve_samples[index];
        length += math::length(math::vec3(b[0] - a[0], b[1] - a[1], b[2] - a[2]));
    }
    const float wavelength = scalar(data, "WaveLen", 1);
    if(wavelength <= 0) throw std::runtime_error("invalid native lightning wavelength");
    const float amplitude = sample_pw_effect_lightning_amplitude(source, document_version, state.local_time);
    for(uint32_t index = 1; index < segments; ++index)
    {
        const auto direction = math::normalize(positions[index] - positions[index - 1]);
        const float coordinate = state.lightning_offset + float(index - 1) * length / wavelength / segments;
        const float angle = native_noise(source.geometry_noise, coordinate, 0) * math::pi<float>() * 2;
        float radial = native_noise(source.geometry_noise, coordinate, 1);
        const float ratio = float(index - 1) / segments;
        switch(int(scalar(data, "Filter")))
        {
            case 0: radial = 2 * (ratio > 0.5f ? 1 - ratio : ratio); break;
            case 1: break;
            case 2: radial *= std::sin(ratio * math::pi<float>()); break;
            case 3: radial *= ratio < 0.2f ? ratio * 5 : ratio > 0.8f ? (1 - ratio) * 5 : 1; break;
            default: throw std::runtime_error("invalid native lightning filter");
        }
        positions[index] += (math::angleAxis(angle, direction) * native_perpendicular(direction)) * radial * amplitude;
    }
    pw_effect_ribbon ribbon;
    ribbon.use_normal = optional_scalar(data, "UseNormal", 0) != 0;
    if(ribbon.use_normal) ribbon.normal = parent.rotation * vector3(data, "Normal");
    const float widths[] = {scalar(data, "Width", 0), scalar(data, "Width", 2), scalar(data, "Width", 1)};
    const float alphas[] = {scalar(data, "Alpha", 0), scalar(data, "Alpha", 2), scalar(data, "Alpha", 1)};
    for(uint32_t index = 0; index <= segments; ++index)
    {
        const float ratio = float(index) / segments;
        const int half = ratio < 0.5f ? 0 : 1;
        const float fraction = ratio * 2 - half;
        pw_effect_ribbon_point point;
        point.position = point_world(parent, positions[index]); // Native curve ignores the moving keypoint matrix.
        point.half_width = math::mix(widths[half], widths[half + 1], fraction) * parent.scale * keypoint.scale;
        point.color = keypoint.color / 255.0f * parent.outer_color;
        point.color.w *= parent.alpha * math::mix(alphas[half], alphas[half + 1], fraction);
        point.uv0 = effect_uv(source, state.texture_time, {ratio, 1});
        point.uv1 = effect_uv(source, state.texture_time, {ratio, 0});
        ribbon.points.push_back(point);
    }
    geometry.ribbons.push_back(std::move(ribbon));
}

void draw_model(const native_element& definition, const native_element_state& state, const native_state& keypoint,
                const native_frame& parent, const entity_index& entities)
{
    std::vector<pw_effect_model_batch> batches;
    const auto position = point_world(parent, keypoint.position);
    const auto rotation = parent.rotation * keypoint_rotation(keypoint);
    const float scale = parent.scale * keypoint.scale;
    const math::mat4 world = math::translate(math::mat4(1), position) * math::mat4_cast(rotation) *
                              math::scale(math::mat4(1), math::vec3(scale));
    auto color = keypoint.color / 255.0f * parent.outer_color;
    color.w *= parent.alpha;
    if(!definition.model->append_triangles(batches, state.local_time, int(scalar(definition.source->type_fields, "Loops")), world, color))
        throw std::runtime_error(definition.model->error());
    if(batches.size() != definition.model_entities.size()) throw std::runtime_error("native model batch inventory changed");
    for(size_t index = 0; index < batches.size(); ++index)
    {
        const auto entity = resolve_entity(entities, definition.model_entities[index]);
        if(!entity) throw std::runtime_error("native model entity missing after clone");
        auto& geometry = entity.get_or_emplace<pw_effect_geometry_component>();
        configure_geometry(geometry, definition, state.texture_time);
        geometry.texture = batches[index].texture;
        geometry.untextured = batches[index].untextured;
        geometry.triangles.insert(geometry.triangles.end(), batches[index].triangles.begin(), batches[index].triangles.end());
        geometry.depth_write = optional_scalar(definition.source->type_fields, "WriteZ", 0) != 0;
        if(optional_scalar(definition.source->type_fields, "AlphaCmp", 0) != 0)
            throw std::runtime_error("native model alpha comparison needs authored render-state reference");
    }
}

void draw_program(native_program& program, native_program_state& state, const entity_index& entities,
                  const native_frame& parent, float delta)
{
    for(size_t index = 0; index < program.elements.size(); ++index)
        if(!program.elements[index].spec.dummy) tick_element(program.elements[index], state.elements[index], delta);
    for(size_t index = 0; index < program.elements.size(); ++index)
    {
        auto& definition = program.elements[index];
        auto& runtime = state.elements[index];
        if(definition.spec.dummy) continue;
        if(!runtime.active && runtime.particles.empty() && runtime.trail.empty()) continue;
        if(definition.bind != SIZE_MAX && !state.elements[definition.bind].active) continue;
        const auto& source = *definition.source;
        const auto keypoint = current_keypoint(program, state, index);
        if(definition.child)
        {
            native_frame child_frame{point_world(parent, keypoint.position), parent.rotation * keypoint_rotation(keypoint),
                parent.scale * keypoint.scale * definition.child->document->default_scale,
                parent.alpha * keypoint.color.w / 255.0f * definition.child->document->default_alpha, parent.outer_color};
            if(optional_scalar(source.type_fields, "OutColor", 0) != 0) child_frame.outer_color = keypoint.color / 255.0f;
            draw_program(*definition.child, *runtime.child, entities, child_frame,
                delta * optional_scalar(source.type_fields, "PlaySpeed", 1) * definition.child->document->default_speed);
            continue;
        }
        const auto entity = resolve_entity(entities, definition.entity_id);
        if(!entity) throw std::runtime_error("map-owned effect entity absent after scene replacement");
        auto& geometry = entity.get_or_emplace<pw_effect_geometry_component>();
        configure_geometry(geometry, definition, runtime.texture_time);
        if(definition.spec.particle) draw_particles(program, index, runtime, entities, parent, keypoint, delta, geometry);
        else if(definition.spec.grid) draw_grid(definition, keypoint, parent, runtime.texture_time, geometry);
        else if(definition.spec.ring) draw_ring(definition, keypoint, parent, runtime.texture_time, geometry);
        else if(definition.spec.trail) draw_trail(definition, runtime, keypoint, parent, delta, geometry);
        else if(definition.spec.model) draw_model(definition, runtime, keypoint, parent, entities);
        else if(definition.spec.lightning) draw_lightning(definition, program.document->version, runtime, keypoint, parent, geometry);
        else
        {
            pw_effect_quad quad;
            quad.position = point_world(parent, keypoint.position);
            quad.right = parent.rotation * (keypoint.direction * math::vec3(1, 0, 0));
            quad.up = parent.rotation * (keypoint.direction * math::vec3(0, 1, 0));
            quad.camera_facing = source.type == 101 || source.type == 102;
            quad.warp_screen_space = quad.camera_facing;
            quad.velocity_facing = definition.spec.rotate_from_view;
            if(quad.velocity_facing) quad.right = parent.rotation * (keypoint.direction * math::vec3(0, 0, 1));
            quad.size = {definition.spec.width * parent.scale * (definition.spec.no_width_scale ? 1 : keypoint.scale),
                         definition.spec.height * parent.scale * (definition.spec.no_height_scale ? 1 : keypoint.scale)};
            quad.pivot = definition.spec.pivot;
            if(quad.camera_facing) quad.pivot.y = 1 - quad.pivot.y;
            quad.rotation = keypoint.rotation;
            quad.color = keypoint.color / 255.0f * parent.outer_color;
            quad.color.w *= parent.alpha;
            for(auto& uv : quad.uv) uv = effect_uv(source, runtime.texture_time, uv);
            geometry.quads.push_back(quad);
        }
    }
}
} // namespace

auto sample_pw_effect_lightning_amplitude(const pw_effect_element& element, int document_version, float elapsed_seconds)
    -> float
{
    const auto& data = element.type_fields;
    // A3DGFXLightning::Load: pre-102 files contain a second Amplitude after
    // Width/Alpha. The first Amplitude belongs to the preceding noise table.
    if(document_version < 102) return scalar(data, "Amplitude", 1);

    const float destinations = scalar(data, "DestNum");
    if(destinations < 0 || destinations > 5 || std::floor(destinations) != destinations)
        throw std::runtime_error("invalid native lightning destination count");
    const size_t count = static_cast<size_t>(destinations);
    const float start = scalar(data, "StartTime");
    std::array<float, 6> values{};
    std::array<float, 5> durations{};
    // Validate the whole authored track during program creation, including
    // destinations that are not reached by the first rendered frame.
    for(size_t index = 0; index <= count; ++index) values[index] = scalar(data, "DestVal", index);
    for(size_t index = 0; index < count; ++index)
    {
        durations[index] = scalar(data, "TransTime", index);
        if(durations[index] < 0) throw std::runtime_error("negative native lightning transition duration");
    }
    float time = elapsed_seconds * 1000 - start;
    if(time <= 0 || count == 0) return values[0];
    for(size_t segment = 0; segment < count; ++segment)
    {
        const float duration = durations[segment];
        if(time > duration) { time -= duration; continue; }
        return math::mix(values[segment], values[segment + 1], duration > 0 ? time / duration : 1);
    }
    return values[count];
}

struct pw_map_effects_runtime::implementation
{
    pw_effect_preparation prepared;
    std::string root;
    uint64_t generation = 0;
    std::string failure;
    std::vector<std::string> ready;
    asset_handle<gfx::shader> vertex_shader;
    asset_handle<gfx::shader> fragment_shader;
    std::vector<std::unique_ptr<native_program>> programs;
    std::vector<std::shared_ptr<native_program_state>> states;
    std::vector<std::vector<native_element*>> elements;
    size_t instance_index = 0;
    size_t element_index = 0;
    // Per-frame simulation policy: only instances near an observer simulate, within a budget.
    pw_effect_update_policy policy;
    std::vector<std::array<float, 3>> observers;
    size_t update_cursor = 0;
    pw_effect_update_plan last_plan;
    std::vector<uint8_t> frozen;       // per instance: geometry already cleared while far
    std::vector<float> pending_delta;  // per instance: time not yet simulated (budget deferral)
    entity_index entity_cache;         // uuid -> handle, rebuilt only when a lookup goes stale
    const entt::registry* cached_registry = nullptr;
};

pw_map_effects_runtime::pw_map_effects_runtime() : impl_(std::make_unique<implementation>()) {}
pw_map_effects_runtime::~pw_map_effects_runtime() = default;
pw_map_effects_runtime::pw_map_effects_runtime(pw_map_effects_runtime&&) noexcept = default;
auto pw_map_effects_runtime::operator=(pw_map_effects_runtime&&) noexcept -> pw_map_effects_runtime& = default;

void pw_map_effects_runtime::begin(pw_effect_preparation prepared, std::string content_root, uint64_t generation)
{
    impl_->prepared = std::move(prepared);
    impl_->root = std::move(content_root);
    impl_->generation = generation;
    if(!impl_->prepared.valid) impl_->failure = impl_->prepared.error;
    try
    {
        for(const auto& instance : impl_->prepared.instances)
        {
            auto program = create_program(instance.document, impl_->root);
            impl_->states.push_back(create_state(*program, static_cast<uint32_t>(std::hash<std::string>{}(instance.source_id))));
            impl_->elements.emplace_back();
            collect_elements(*program, impl_->elements.back());
            impl_->programs.push_back(std::move(program));
        }
    }
    catch(const std::exception& error) { impl_->failure = error.what(); }
}

auto pw_map_effects_runtime::stage(rtti::context& context, entt::handle owner,
                                  std::vector<entt::handle>& created_entities,
                                  std::vector<std::string>& generated_mesh_keys) -> pw_effect_stage
{
    (void)generated_mesh_keys;
    if(!impl_->failure.empty()) return pw_effect_stage::failed;
    try
    {
        auto& manager = context.get_cached<asset_manager>();
        auto& current_scene = context.get_cached<ecs>().get_scene();
        if(!impl_->prepared.instances.empty())
        {
            impl_->vertex_shader = manager.get_asset<gfx::shader>("engine:/data/shaders/pw/vs_pw_effect.sc", load_flags::standard);
            impl_->fragment_shader = manager.get_asset<gfx::shader>("engine:/data/shaders/pw/fs_pw_effect.sc", load_flags::standard);
            impl_->vertex_shader.submit();
            impl_->fragment_shader.submit();
            const auto vertex = impl_->vertex_shader.get_if_ready();
            const auto fragment = impl_->fragment_shader.get_if_ready();
            if(!vertex || !fragment || !vertex->is_valid() || !fragment->is_valid()) return pw_effect_stage::waiting;
        }
        constexpr size_t elements_per_frame = 16;
        size_t created = 0;
        while(impl_->instance_index < impl_->prepared.instances.size() && created < elements_per_frame)
        {
            const auto& instance = impl_->prepared.instances[impl_->instance_index];
            if(impl_->element_index == impl_->elements[impl_->instance_index].size())
            {
                impl_->ready.push_back(instance.source_id);
                ++impl_->instance_index;
                impl_->element_index = 0;
                continue;
            }
            auto& runtime = *impl_->elements[impl_->instance_index][impl_->element_index];
            const auto& element = *runtime.source;
            const auto resource = poll_pw_effect_textures(manager, impl_->root, element, runtime.textures, impl_->failure);
            if(resource == pw_effect_resource_status::failed) return pw_effect_stage::failed;
            if(resource == pw_effect_resource_status::waiting) return pw_effect_stage::waiting;
            if(!runtime.spec.container && !runtime.spec.model && runtime.dummy == SIZE_MAX && runtime.textures.empty())
                throw std::runtime_error("native drawable requires a resolved source texture");
            if(runtime.spec.model)
            {
                if(!runtime.model) throw std::runtime_error("native model required dependency missing");
                const auto status = runtime.model->poll(manager);
                if(status == pw_effect_resource_status::failed) throw std::runtime_error(runtime.model->error());
                if(status == pw_effect_resource_status::waiting) return pw_effect_stage::waiting;
                if(runtime.model->batch_count() == 0) throw std::runtime_error("native model has no usable batches");
            }
            const std::string name = "PW Effect " + instance.source_id + "/" + std::to_string(element.index);
            auto entity = scene::create_entity(*current_scene.registry, name);
            created_entities.push_back(entity);
            entity.get<transform_component>().set_parent(owner, false);
            auto& geometry = entity.emplace<pw_effect_geometry_component>();
            configure_geometry(geometry, runtime, 0);
            runtime.entity_id = entity.get<id_component>().id;
            if(runtime.spec.model)
            {
                runtime.model_entities.push_back(runtime.entity_id);
                for(size_t batch = 1; batch < runtime.model->batch_count(); ++batch)
                {
                    auto part = scene::create_entity(*current_scene.registry, name + "/" + std::to_string(batch));
                    part.get<transform_component>().set_parent(owner, false);
                    part.emplace<pw_effect_geometry_component>();
                    created_entities.push_back(part);
                    runtime.model_entities.push_back(part.get<id_component>().id);
                }
            }
            ++impl_->element_index;
            ++created;
        }
        return impl_->instance_index == impl_->prepared.instances.size() ? pw_effect_stage::ready : pw_effect_stage::waiting;
    }
    catch(const std::exception& error)
    {
        impl_->failure = error.what();
        return pw_effect_stage::failed;
    }
}

void pw_map_effects_runtime::update(rtti::context& context, float delta_seconds)
{
    if(!impl_->failure.empty() || !std::isfinite(delta_seconds) || delta_seconds < 0) return;
    auto& current_scene = context.get_cached<ecs>().get_scene();
    try
    {
        const size_t count = std::min(impl_->programs.size(), impl_->prepared.instances.size());
        impl_->frozen.resize(count, 0);
        impl_->pending_delta.resize(count, 0.0f);

        // The uuid index is rebuilt at most once per frame, and only when a lookup finds a dead handle
        // or the registry changed. Building it every frame over the whole scene was a per-frame O(N).
        auto& entities = impl_->entity_cache;
        bool rebuilt = false;
        const auto rebuild_index = [&]
        {
            entities.clear();
            const auto view = current_scene.registry->view<id_component>();
            entities.reserve(view.size());
            for(const auto entity : view)
                entities.emplace(view.get<id_component>(entity).id, entt::handle{*current_scene.registry, entity});
            impl_->cached_registry = current_scene.registry.get();
            rebuilt = true;
        };
        if(impl_->cached_registry != current_scene.registry.get()) rebuild_index();
        const auto resolve = [&](const hpp::uuid& id) -> entt::handle
        {
            auto entity = resolve_entity(entities, id);
            if(entity && entity.valid()) return entity;
            if(rebuilt) return entt::handle{};
            rebuild_index();
            entity = resolve_entity(entities, id);
            return entity && entity.valid() ? entity : entt::handle{};
        };
        const auto clear_instance = [&](size_t index, bool configure)
        {
            for(const auto* element : impl_->elements[index])
            {
                const auto entity = resolve(element->entity_id);
                if(!entity) continue;
                auto& geometry = entity.get_or_emplace<pw_effect_geometry_component>();
                geometry.triangles.clear();
                geometry.quads.clear();
                geometry.ribbons.clear();
                if(configure) configure_geometry(geometry, *element, 0);
                for(const auto& id : element->model_entities)
                {
                    const auto part = resolve(id);
                    if(part) part.get_or_emplace<pw_effect_geometry_component>().triangles.clear();
                }
            }
        };

        impl_->last_plan = plan_pw_effect_updates(impl_->prepared.instances, impl_->observers, impl_->policy,
                                                  impl_->update_cursor);
        std::vector<uint8_t> updated(count, 0);
        for(const size_t index : impl_->last_plan.freeze)
        {
            if(index >= count) continue;
            updated[index] = 1;
            impl_->pending_delta[index] = 0.0f;
            if(impl_->frozen[index]) continue;
            clear_instance(index, false);
            impl_->frozen[index] = 1;
        }
        for(const size_t index : impl_->last_plan.update)
        {
            if(index >= count) continue;
            updated[index] = 1;
            clear_instance(index, true);
            impl_->frozen[index] = 0;
            const float step = std::min(impl_->pending_delta[index], 1.0f) + delta_seconds;
            impl_->pending_delta[index] = 0.0f;
            const auto& instance = impl_->prepared.instances[index];
            const float factor = impl_->prepared.day_night_factor;
            if(instance.valid_time != 2 && !((instance.valid_time == 0 && factor < 0.5f) ||
                                             (instance.valid_time == 1 && factor > 0.5f))) continue;
            native_frame frame;
            frame.position = {instance.position[0], instance.position[1], instance.position[2]};
            frame.rotation = direction_frame({instance.forward[0], instance.forward[1], instance.forward[2]},
                                               {instance.up[0], instance.up[1], instance.up[2]});
            frame.scale = instance.scale * instance.document.default_scale;
            frame.alpha = instance.alpha * instance.document.default_alpha;
            draw_program(*impl_->programs[index], *impl_->states[index], entities, frame,
                         step * instance.speed * instance.document.default_speed);
        }
        // Near instances the budget postponed keep last frame's geometry and accumulate time.
        for(size_t index = 0; index < count; ++index)
            if(!updated[index]) impl_->pending_delta[index] += delta_seconds;
    }
    catch(const std::exception& error) { impl_->failure = error.what(); }
}

void pw_map_effects_runtime::set_observers(std::vector<std::array<float, 3>> observers)
{
    impl_->observers = std::move(observers);
}

void pw_map_effects_runtime::set_update_policy(const pw_effect_update_policy& policy) { impl_->policy = policy; }

auto pw_map_effects_runtime::last_update_plan() const -> const pw_effect_update_plan& { return impl_->last_plan; }

void pw_map_effects_runtime::destroy(rtti::context& context)
{
    auto& current_scene = context.get_cached<ecs>().get_scene();
    for(const auto& instance_elements : impl_->elements)
        for(const auto* element : instance_elements)
        {
            const auto entity = current_scene.find_entity_by_uuid(element->entity_id);
            if(entity) scene::destroy_entity(entity);
            for(const auto& id : element->model_entities)
            {
                if(id == element->entity_id) continue;
                const auto part = current_scene.find_entity_by_uuid(id);
                if(part) scene::destroy_entity(part);
            }
        }
    impl_->elements.clear();
    impl_->programs.clear();
    impl_->states.clear();
    impl_->ready.clear();
}

auto pw_map_effects_runtime::error() const -> const std::string& { return impl_->failure; }
auto pw_map_effects_runtime::ready_ids() const -> const std::vector<std::string>& { return impl_->ready; }
} // namespace unravel
