#include "pw_map_effects.h"
#include "pw_effect_model.h"

#include <engine/assets/asset_manager.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace unravel
{
namespace
{
using json = nlohmann::json;

auto trim(const std::string& value) -> std::string
{
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last = value.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : value.substr(first, last - first + 1);
}

auto hex_digit(char value) -> int
{
    if(value >= '0' && value <= '9') return value - '0';
    if(value >= 'a' && value <= 'f') return value - 'a' + 10;
    throw std::runtime_error("authored bytesHex must contain lowercase hexadecimal bytes");
}

auto decode_records(const json& records) -> std::vector<pw_effect_field>
{
    if(!records.is_array()) throw std::runtime_error("authored records must be an array");
    std::vector<pw_effect_field> result;
    for(const auto& record : records)
    {
        const std::string hex = record.at("bytesHex").get<std::string>();
        if(hex.size() % 2 != 0) throw std::runtime_error("authored bytesHex is truncated");
        std::string line;
        line.reserve(hex.size() / 2);
        for(size_t index = 0; index < hex.size(); index += 2)
            line.push_back(static_cast<char>((hex_digit(hex[index]) << 4) | hex_digit(hex[index + 1])));
        if(line.find('\0') != std::string::npos || line.find('\n') != std::string::npos)
            throw std::runtime_error("invalid embedded authored record delimiter");
        const auto separator = line.find(':');
        result.push_back(separator == std::string::npos ? pw_effect_field{"", trim(line)} :
                         pw_effect_field{trim(line.substr(0, separator)), trim(line.substr(separator + 1))});
        // Numeric conversion belongs to preparation, not thousands of per-frame particle evaluations.
        std::string numeric = result.back().value;
        std::replace(numeric.begin(), numeric.end(), ',', ' ');
        std::istringstream values(numeric);
        double value = 0;
        while(values >> value) result.back().numeric_values.push_back(value);
        if(!values.eof()) result.back().numeric_values.clear();
    }
    return result;
}

auto numbers(const std::string& text, size_t count) -> std::vector<double>
{
    std::string normalized = text;
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::istringstream stream(normalized);
    std::vector<double> result;
    double value = 0;
    while(stream >> value)
    {
        if(!std::isfinite(value)) throw std::runtime_error("non-finite authored number");
        result.push_back(value);
    }
    if(!stream.eof() || result.size() != count)
        throw std::runtime_error("malformed authored numeric field: " + text);
    return result;
}

auto read(const std::vector<pw_effect_field>& fields, size_t& index, const std::string& name, size_t count = 1)
    -> std::vector<double>
{
    if(index >= fields.size() || fields[index].name != name)
        throw std::runtime_error("expected authored field " + name);
    return numbers(fields[index++].value, count);
}

auto integer(double value, double minimum, double maximum) -> int64_t
{
    if(std::floor(value) != value || value < minimum || value > maximum)
        throw std::runtime_error("authored integer outside its native range");
    return static_cast<int64_t>(value);
}

auto read_uint(const std::vector<pw_effect_field>& fields, size_t& index, const std::string& name) -> uint32_t
{
    return static_cast<uint32_t>(integer(read(fields, index, name)[0], 0, UINT32_MAX));
}

void prepare_controller(pw_effect_controller& controller)
{
    const auto number = [&](const char* name, size_t occurrence = 0)
    { return read_pw_effect_numbers(controller.fields, name, occurrence, 1)[0]; };
    if(controller.type == 107 || controller.type == 109 || controller.type == 112)
    {
        // APerlinNoise1D: Park-Miller random buffer, three-channel cyclic smoothing,
        // normalized octave amplitudes and linear interpolation. Native seeds use
        // wall time; a stable record seed makes map reloads reproducible.
        uint32_t seed = 2166136261u;
        for(const auto& field : controller.fields)
            for(const unsigned char byte : field.name + field.value) seed = (seed ^ byte) * 16777619u;
        seed = seed % 2147483646u + 1;
        const auto random = [&]() -> uint32_t
        { seed = static_cast<uint32_t>((uint64_t(seed) * 16807u) % 2147483647u); return seed; };
        const auto count = static_cast<size_t>(integer(number("BufLen"), 1, 1048576));
        const int octaves = static_cast<int>(integer(number("OctaveNum"), 1, 16));
        int wavelength = static_cast<int>(integer(number("WaveLen"), 1, INT32_MAX));
        const float amplitude = std::abs(static_cast<float>(number("Amplitude")));
        const float persistence = std::abs(static_cast<float>(number("Persistence")));
        controller.noise_values.resize(count);
        std::vector<float> raw(count);
        for(size_t channel = 0; channel < 3; ++channel)
        {
            for(auto& value : raw) value = (int(random() % 2001) - 1000) / 1000.0f;
            for(size_t index = 0; index < count; ++index)
                controller.noise_values[index][channel] = raw[(index + count - 1) % count] * 0.25f +
                    raw[index] * 0.5f + raw[(index + 1) % count] * 0.25f;
        }
        float total = 0, weight = 1;
        for(int octave = 0; octave < octaves; ++octave)
        {
            controller.noise_octaves.push_back({float(random() % 1023), float(wavelength), weight});
            total += weight;
            weight *= persistence;
            wavelength /= 2;
            if(wavelength <= 0) break;
        }
        for(auto& octave : controller.noise_octaves) octave[2] *= amplitude / total;
    }
    if(controller.type == 110)
    {
        const size_t count = static_cast<size_t>(integer(number("Count"), 0, 1048576));
        if(count < 3) return; // Native GenPath has the same minimum.
        std::vector<std::array<float, 3>> points(count), corners(count * 3 - 2);
        for(size_t index = 0; index < count; ++index)
        {
            const auto point = read_pw_effect_numbers(controller.fields, "Pos", index, 3);
            for(size_t axis = 0; axis < 3; ++axis) points[index][axis] = static_cast<float>(point[axis]);
        }
        corners.front() = points.front();
        corners.back() = points.back();
        for(size_t index = 1; index + 1 < count; ++index)
            for(size_t axis = 0; axis < 3; ++axis)
            {
                const float difference = (points[index + 1][axis] - points[index - 1][axis]) / 6.0f;
                corners[index * 3 - 1][axis] = points[index][axis] - difference;
                corners[index * 3][axis] = points[index][axis];
                corners[index * 3 + 1][axis] = points[index][axis] + difference;
            }
        for(size_t axis = 0; axis < 3; ++axis)
        {
            corners[1][axis] = corners[2][axis] + (points[0][axis] - points[1][axis]) / 3.0f;
            corners[corners.size() - 2][axis] = corners[corners.size() - 3][axis] +
                (points[count - 1][axis] - points[count - 2][axis]) / 3.0f;
        }
        for(size_t segment = 0; segment + 1 < count; ++segment)
            for(int step = 0; step < 6; ++step)
            {
                const float t = step / 6.0f, u = 1 - t;
                std::array<float, 4> sample{};
                for(size_t axis = 0; axis < 3; ++axis)
                    sample[axis] = corners[segment * 3][axis] * u * u * u +
                        corners[segment * 3 + 1][axis] * 3 * u * u * t +
                        corners[segment * 3 + 2][axis] * 3 * u * t * t +
                        corners[segment * 3 + 3][axis] * t * t * t;
                controller.curve_samples.push_back(sample);
            }
        controller.curve_samples.push_back({points.back()[0], points.back()[1], points.back()[2], 0});
        for(size_t index = 1; index < controller.curve_samples.size(); ++index)
        {
            auto& point = controller.curve_samples[index];
            const auto& previous = controller.curve_samples[index - 1];
            float squared = 0;
            for(size_t axis = 0; axis < 3; ++axis) squared += (point[axis] - previous[axis]) * (point[axis] - previous[axis]);
            point[3] = previous[3] + std::sqrt(squared);
        }
        const float length = controller.curve_samples.back()[3];
        if(length <= 0) controller.curve_samples.clear();
        else for(auto& point : controller.curve_samples) point[3] /= length;
    }
}

auto read_controller(const std::vector<pw_effect_field>& fields, size_t& index, int version) -> pw_effect_controller
{
    pw_effect_controller result;
    result.type = static_cast<int>(integer(read(fields, index, "CtrlType")[0], 100, 112));
    if(version >= 21)
    {
        result.start_seconds = read(fields, index, "StartTime")[0];
        result.end_seconds = read(fields, index, "EndTime")[0];
    }
    while(index < fields.size() && fields[index].name != "CtrlType" && fields[index].name != "InterpolateMode")
        result.fields.push_back(fields[index++]);
    prepare_controller(result);
    return result;
}

auto read_keypoint(const std::vector<pw_effect_field>& fields, size_t& index, int version) -> pw_effect_keypoint
{
    pw_effect_keypoint result;
    result.interpolation = static_cast<int>(integer(read(fields, index, "InterpolateMode")[0], 0, 2));
    const auto duration = integer(read(fields, index, "TimeSpan")[0], INT32_MIN, UINT32_MAX);
    result.duration_ms = static_cast<uint32_t>(duration);
    const auto position = read(fields, index, "Position", 3);
    for(size_t axis = 0; axis < 3; ++axis) result.position[axis] = static_cast<float>(position[axis]);
    result.color_argb = static_cast<uint32_t>(integer(read(fields, index, "Color")[0], INT32_MIN, UINT32_MAX));
    result.scale = static_cast<float>(read(fields, index, "Scale")[0]);
    const auto direction = read(fields, index, "Direction", 4);
    double length = 0;
    for(size_t axis = 0; axis < 4; ++axis) length += direction[axis] * direction[axis];
    if(length <= std::numeric_limits<float>::epsilon()) throw std::runtime_error("zero authored quaternion");
    for(size_t axis = 0; axis < 4; ++axis) result.direction[axis] = static_cast<float>(direction[axis] / std::sqrt(length));
    result.rotation_2d = static_cast<float>(read(fields, index, "Rad_2D")[0]);
    const auto count = read_uint(fields, index, "CtrlMethodCount");
    if(count > fields.size()) throw std::runtime_error("keypoint controller count exceeds authored records");
    for(uint32_t controller = 0; controller < count; ++controller)
        result.controllers.push_back(read_controller(fields, index, version));
    return result;
}

auto is_native_type(int type) -> bool
{
    switch(type)
    {
        case 100: case 101: case 102: case 110: case 120: case 121: case 122: case 123:
        case 124: case 125: case 130: case 140: case 150: case 151: case 152: case 160:
        case 170: case 180: case 190: case 200: case 210: case 211: case 220: case 221:
        case 230: case 240: return true;
        default: return false;
    }
}

auto read_element(const json& source, size_t element_index, int version) -> pw_effect_element
{
    pw_effect_element result;
    result.index = element_index;
    result.type = source.at("typeId").get<int>();
    result.name = source.at("name").get<std::string>();
    if(!is_native_type(result.type)) throw std::runtime_error("unknown native GFX type " + std::to_string(result.type));
    const auto& authored = source.at("authored");
    if(authored.at("recordEncoding") != "legacy-bytes-hex") throw std::runtime_error("unsupported GFX record encoding");
    const auto fields = decode_records(authored.at("records"));
    const size_t type_offset = authored.at("typePayloadRecordOffset").get<size_t>();
    const size_t count_offset = authored.at("keyPointCountRecordOffset").get<size_t>();
    if(type_offset == 0 || count_offset <= type_offset || count_offset >= fields.size())
        throw std::runtime_error("invalid authored GFX section offsets");
    size_t index = 0;
    if(read_uint(fields, index, "GFXELEMENTID") != static_cast<uint32_t>(result.type))
        throw std::runtime_error("authored GFX type contradicts metadata");
    result.base_fields.assign(fields.begin(), fields.begin() + type_offset);
    // Native A3DGFXKeyPointSet::Load reads StartTime immediately before KEYPOINTCOUNT.
    index = count_offset - 1;
    result.start_ms = read_uint(fields, index, "StartTime");
    const auto count = read_uint(fields, index, "KEYPOINTCOUNT");
    if(count != source.at("keyPointCount").get<uint32_t>() || count > fields.size())
        throw std::runtime_error("keypoint count contradicts metadata or available records");
    for(uint32_t keypoint = 0; keypoint < count; ++keypoint)
        result.keypoints.push_back(read_keypoint(fields, index, version));
    if(index != fields.size()) throw std::runtime_error("unconsumed keypoint/controller records");
    result.type_fields.assign(fields.begin() + type_offset, fields.begin() + count_offset - 1);
    if(result.type == 150)
    {
        result.geometry_noise.type = 112;
        result.geometry_noise.fields.assign(result.type_fields.begin(), result.type_fields.begin() +
            std::min(size_t(5), result.type_fields.size()));
        prepare_controller(result.geometry_noise);
    }
    if(result.type >= 120 && result.type <= 125)
    {
        const auto affector = std::find_if(result.type_fields.begin(), result.type_fields.end(),
            [](const pw_effect_field& field) { return field.name == "AffectorCount"; });
        if(affector == result.type_fields.end()) throw std::runtime_error("particle AffectorCount absent");
        size_t position = static_cast<size_t>(affector - result.type_fields.begin());
        const auto affector_count = read_uint(result.type_fields, position, "AffectorCount");
        if(affector_count > result.type_fields.size()) throw std::runtime_error("particle affector count exceeds records");
        for(uint32_t number = 0; number < affector_count; ++number)
            result.particle_affectors.push_back(read_controller(result.type_fields, position, version));
        if(position != result.type_fields.size()) throw std::runtime_error("unconsumed particle affector records");
        result.type_fields.erase(affector, result.type_fields.end());
    }
    return result;
}

auto safe_relative(const std::string& path) -> bool
{
    if(path.empty() || path.find(':') != std::string::npos || path.find('\\') != std::string::npos) return false;
    const fs::path relative(path);
    if(relative.is_absolute()) return false;
    for(const auto& part : relative) if(part == ".." || part == ".") return false;
    return true;
}

void read_nested_documents(const fs::path& content_root, pw_effect_document& document,
                           std::unordered_map<std::string, std::shared_ptr<pw_effect_document>>& cache,
                           std::unordered_set<std::string>& active)
{
    for(auto& element : document.elements)
        for(auto& dependency : element.dependencies)
        {
            if(dependency.kind == "model")
            {
                dependency.model = prepare_pw_effect_model(content_root, dependency.model_descriptor);
                if(!dependency.model || !dependency.model->valid)
                    throw std::runtime_error(dependency.model ? dependency.model->error : "native model preparation failed");
            }
            if(dependency.kind == "shader")
            {
                if(!safe_relative(dependency.output)) throw std::runtime_error("required GFX shader descriptor absent");
                std::ifstream input(content_root / dependency.output, std::ios::binary);
                if(!input) throw std::runtime_error("GFX shader descriptor unreadable");
                const json shader = json::parse(input);
                if(shader.at("format") != "EDS_GFX_SHADER" || shader.at("schemaVersion") != 1 ||
                   shader.at("implementation") != "angelica-fluid-v1" || dependency.implementation != "angelica-fluid-v1")
                    throw std::runtime_error("unsupported required native GFX shader");
            }
            if(dependency.kind != "gfx") continue;
            if(!safe_relative(dependency.output)) throw std::runtime_error("required nested GFX output absent");
            if(active.count(dependency.output)) throw std::runtime_error("cyclic nested GFX output");
            const auto existing = cache.find(dependency.output);
            if(existing != cache.end()) { dependency.nested = existing->second; continue; }
            std::ifstream input(content_root / dependency.output, std::ios::binary);
            if(!input) throw std::runtime_error("nested GFX output unreadable: " + dependency.output);
            auto nested = std::make_shared<pw_effect_document>(parse_pw_effect_document(json::parse(input)));
            if(!nested->valid) throw std::runtime_error("nested GFX: " + nested->error);
            active.insert(dependency.output);
            read_nested_documents(content_root, *nested, cache, active);
            active.erase(dependency.output);
            cache.emplace(dependency.output, nested);
            dependency.nested = std::move(nested);
        }
}
} // namespace

auto read_pw_effect_numbers(const std::vector<pw_effect_field>& fields, const std::string& name,
                            size_t occurrence, size_t count) -> std::vector<double>
{
    for(const auto& field : fields)
    {
        if(field.name == name)
        {
            if(occurrence == 0)
            {
                if(field.numeric_values.size() == count)
                {
                    for(const double value : field.numeric_values)
                        if(!std::isfinite(value)) throw std::runtime_error("non-finite authored number");
                    return field.numeric_values;
                }
                return numbers(field.value, count);
            }
            --occurrence;
        }
    }
    throw std::runtime_error("required authored field absent: " + name);
}

auto parse_pw_effect_document(const json& document) -> pw_effect_document
{
    pw_effect_document result;
    try
    {
        if(document.at("format") != "EDS_EFFECT" || document.at("schemaVersion") != 1 ||
           document.at("gfxFormat") != "text" || !document.at("sourceReadable").get<bool>() ||
           !document.at("complete").get<bool>()) throw std::runtime_error("incomplete or unsupported GFX document");
        result.source_id = document.at("sourceId").get<std::string>();
        result.version = document.at("version").get<int>();
        if(result.version < 1 || result.version > 103) throw std::runtime_error("unsupported Angelica GFX version");
        result.default_scale = document.at("defaultScale").get<float>();
        result.default_speed = document.at("defaultPlaySpeed").get<float>();
        result.default_alpha = document.at("defaultAlpha").get<float>();
        if(!std::isfinite(result.default_scale) || !std::isfinite(result.default_speed) ||
           !std::isfinite(result.default_alpha)) throw std::runtime_error("non-finite GFX document defaults");
        result.header_fields = decode_records(document.at("authoredHeader"));
        const auto& elements = document.at("elements");
        if(!elements.is_array() || elements.size() != document.at("declaredElementCount").get<size_t>())
            throw std::runtime_error("GFX element count mismatch");
        for(size_t index = 0; index < elements.size(); ++index)
            result.elements.push_back(read_element(elements[index], index, result.version));
        for(const auto& source : document.at("dependencies"))
        {
            const auto index = source.at("elementIndex").get<size_t>();
            if(index >= result.elements.size() || source.at("elementType").get<int>() != result.elements[index].type)
                throw std::runtime_error("dependency element identity mismatch");
            pw_effect_dependency dependency;
            dependency.kind = source.at("kind").get<std::string>();
            dependency.source_path = source.at("sourcePath").get<std::string>();
            dependency.required = source.at("required").get<bool>();
            dependency.implementation = source.value("implementation", std::string());
            if(dependency.kind == "model") dependency.model_descriptor = source;
            dependency.output = source.value(dependency.kind == "texture" ? "textureRef" :
                                             dependency.kind == "model" ? "modelRef" :
                                             dependency.kind == "shader" ? "shaderRef" : "effectRef", std::string());
            if(!dependency.output.empty() && !safe_relative(dependency.output))
                throw std::runtime_error("unsafe GFX dependency output path");
            result.elements[index].dependencies.push_back(std::move(dependency));
        }
        result.valid = true;
    }
    catch(const std::exception& error)
    {
        result.error = error.what();
    }
    return result;
}

auto prepare_pw_map_effects(const fs::path& content_root, const json& scene) -> pw_effect_preparation
{
    pw_effect_preparation result;
    try
    {
        std::unordered_map<std::string, json> identities;
        std::unordered_map<std::string, std::shared_ptr<pw_effect_document>> nested_cache;
        std::unordered_set<std::string> active_nested;
        for(const auto& node : scene.at("nodes"))
        {
            if(node.value("type", 0) != 14) continue;
            ++result.raw_references;
            const std::string id = node.at("sourceId").get<std::string>();
            if(id.size() != 15 || id.compare(0, 7, "effect:") != 0)
                throw std::runtime_error("invalid effect source identity");
            for(size_t index = 7; index < id.size(); ++index) hex_digit(id[index]);
            const auto& parameters = node.at("effectParameters");
            if(!parameters.at("present").get<bool>()) throw std::runtime_error("authored effect parameters absent: " + id);
            const json identity = {{"sourcePath", node.at("externalPath")}, {"position", node.at("pos")},
                {"forward", node.at("dir")}, {"up", node.at("up")}, {"parameters", parameters}};
            const auto existing = identities.find(id);
            if(existing != identities.end())
            {
                if(existing->second != identity) throw std::runtime_error("contradictory duplicate effect identity: " + id);
                ++result.duplicate_references;
                continue;
            }
            identities.emplace(id, identity);
            pw_effect_instance instance;
            instance.source_id = id;
            instance.effect_ref = node.at("openFormat").at("effectRef").at("effect").get<std::string>();
            if(!safe_relative(instance.effect_ref)) throw std::runtime_error("unsafe effect output path: " + id);
            instance.position = node.at("pos").get<std::array<float, 3>>();
            instance.forward = node.at("dir").get<std::array<float, 3>>();
            instance.up = node.at("up").get<std::array<float, 3>>();
            instance.scale = parameters.at("scale").get<float>();
            instance.speed = parameters.at("playSpeed").get<float>();
            instance.alpha = parameters.at("alpha").get<float>();
            instance.valid_time = parameters.at("validTime").get<int>();
            for(const float value : {instance.scale, instance.speed, instance.alpha, instance.position[0],
                    instance.position[1], instance.position[2], instance.forward[0], instance.forward[1],
                    instance.forward[2], instance.up[0], instance.up[1], instance.up[2]})
                if(!std::isfinite(value)) throw std::runtime_error("non-finite effect placement: " + id);
            std::ifstream input(content_root / instance.effect_ref, std::ios::binary);
            if(!input) throw std::runtime_error("effect output unreadable: " + instance.effect_ref);
            instance.document = parse_pw_effect_document(json::parse(input));
            if(!instance.document.valid) throw std::runtime_error(id + ": " + instance.document.error);
            if(instance.document.source_id != id) throw std::runtime_error("effect document source identity mismatch: " + id);
            read_nested_documents(content_root, instance.document, nested_cache, active_nested);
            result.instances.push_back(std::move(instance));
        }
        result.valid = true;
    }
    catch(const std::exception& error)
    {
        result.error = error.what();
    }
    return result;
}

auto poll_pw_effect_textures(asset_manager& manager, const std::string& content_root,
                            const pw_effect_element& element, std::vector<pw_effect_texture_state>& state,
                            std::string& error) -> pw_effect_resource_status
{
    error.clear();
    if(content_root.empty())
    {
        error = "GFX content root is empty";
        return pw_effect_resource_status::failed;
    }
    bool waiting = false;
    for(const auto& dependency : element.dependencies)
    {
        if(dependency.kind != "texture") continue;
        if(!safe_relative(dependency.output))
        {
            error = "GFX texture has no valid converted output: " + dependency.source_path;
            return pw_effect_resource_status::failed;
        }
        const std::string key = content_root + (content_root.back() == '/' ? "" : "/") + dependency.output;
        auto slot = std::find_if(state.begin(), state.end(), [&](const auto& value) { return value.key == key; });
        if(slot == state.end())
        {
            state.push_back({key, {}});
            slot = state.end() - 1;
        }
        slot->handle = manager.get_asset<gfx::texture>(key, load_flags::standard);
        slot->handle.submit();
        const auto texture = slot->handle.get_if_ready();
        if(!texture || !texture->is_valid() || texture->info.width == 0 || texture->info.height == 0) waiting = true;
    }
    return waiting ? pw_effect_resource_status::waiting : pw_effect_resource_status::ready;
}

auto plan_pw_effect_updates(const std::vector<pw_effect_instance>& instances,
                            const std::vector<std::array<float, 3>>& observers,
                            const pw_effect_update_policy& policy, size_t& cursor) -> pw_effect_update_plan
{
    pw_effect_update_plan plan;
    const size_t count = instances.size();
    if(count == 0)
    {
        cursor = 0;
        return plan;
    }
    const bool cull = policy.update_radius > 0.0f && !observers.empty();
    const float radius_sq = policy.update_radius * policy.update_radius;
    std::vector<size_t> near_set;
    near_set.reserve(count);
    for(size_t index = 0; index < count; ++index)
    {
        bool is_near = !cull;
        if(cull)
        {
            const auto& p = instances[index].position;
            for(const auto& o : observers)
            {
                const float dx = p[0] - o[0];
                const float dy = p[1] - o[1];
                const float dz = p[2] - o[2];
                if(dx * dx + dy * dy + dz * dz <= radius_sq)
                {
                    is_near = true;
                    break;
                }
            }
        }
        if(is_near) near_set.push_back(index);
        else plan.freeze.push_back(index);
    }
    if(near_set.empty())
    {
        cursor = 0;
        return plan;
    }
    const size_t budget = policy.max_updates_per_frame == 0
                              ? near_set.size()
                              : std::min<size_t>(near_set.size(), policy.max_updates_per_frame);
    cursor %= near_set.size();
    plan.update.reserve(budget);
    for(size_t k = 0; k < budget; ++k) plan.update.push_back(near_set[(cursor + k) % near_set.size()]);
    plan.deferred = near_set.size() - budget;
    cursor = budget < near_set.size() ? (cursor + budget) % near_set.size() : 0;
    return plan;
}
} // namespace unravel
