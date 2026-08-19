#include "mcp_system.h"

#include "mcp_commands.h"

#include "json.hpp"

#include <engine/animation/animation.h>
#include <engine/animation/ecs/components/animation_component.h>
#include <engine/assets/asset_manager.h>
#include <engine/defaults/defaults.h>
#include <engine/ecs/components/tag_component.h>
#include <engine/ecs/components/transform_component.h>
#include <engine/ecs/ecs.h>
#include <engine/ecs/scene.h>
#include <engine/events.h>
#include <engine/rendering/ecs/components/auto_exposure_component.h>
#include <engine/rendering/ecs/components/camera_component.h>
#include <engine/rendering/ecs/components/light_component.h>
#include <engine/rendering/ecs/components/model_component.h>
#include <engine/rendering/ecs/components/particle_emitter_component.h>
#include <engine/rendering/ecs/components/reflection_probe_component.h>
#include <engine/rendering/ecs/components/tonemapping_component.h>
#include <engine/rendering/ecs/systems/rendering_system.h>
#include <engine/rendering/material.h>
#include <engine/rendering/mesh.h>
#include <engine/rendering/model.h>
#include <filesystem/filesystem.h>
#include <graphics/graphics.h>
#include <graphics/render_pass.h>
#include <graphics/texture.h>
#include <graphics/utils/bgfx_utils.h>
#include <graphics/vertex_decl.h>
#include <logging/logging.h>

#include <bimg/encode.h>
#include <bx/file.h>

#include <editor/hub/hub.h>
#include <editor/hub/panels/scene_panel/scene_panel.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace unravel
{
namespace
{
constexpr auto kMcpFrameDt = delta_t(0.016667f);
constexpr uint32_t kLoginMaxAssetWaitFrames = 900;
constexpr float kDuplicatePositionEpsilon = 0.01f;
constexpr const char* kLoginCharacterEntityName = "Login Character";
// Dressed char-select Blademaster: body + Guardian armor (AddSkinFile) + sword (AddChildModel),
// baked into model_1c939f0c.gltf by ECModelViewer. Same 武侠男 skeleton as the bare body, so the
// existing idle clip (model_57f7b4eb_anim_74691562) is reused unchanged.
constexpr const char* kLoginCharacterMeshRef = "characters/player/model_1c939f0c.gltf";
constexpr const char* kLoginCharacterIdleClipRef = "characters/player/model_57f7b4eb_anim_74691562.anim";
// Character stand + char-select camera derived from the client config configs/scenectrl.ini
// (read via AngelicaIDE PCK reader). The converter now emits LEFT-handed open format (kEmitLeftHanded),
// matching this LH runtime, so scenectrl.ini coordinates are used directly (no Z flip).
// Stand = [NewChar] Pos0 (profession 0 = Blademaster), camera = [Camera] idx 14 (LOGIN_SCENE_CREATE
// for profession 0) + s_camPosDelta[0][0]=(0,0.2,0); FOV = DEFCAMERA_FOV (56 deg).
constexpr float kLoginCharacterX = 191.983002f;
constexpr float kLoginCharacterZ = 286.619995f;
constexpr float kLoginCharacterFallbackY = 228.391006f;
constexpr float kLoginCharacterGroundOffset = 0.03f;
constexpr float kLoginCharacterCameraFov = 56.0f;
constexpr float kLoginCharacterCameraNear = 0.05f;
constexpr float kLoginCharacterCameraFar = 1600.0f;

using json = nlohmann::json;
constexpr int kLoginSceneNewCharMax = 64;

struct partial_login_scene_camera
{
    login_scene_camera camera;
    bool pos_x = false;
    bool pos_y = false;
    bool pos_z = false;
};

struct partial_login_scene_vec3
{
    math::vec3 value{};
    bool x = false;
    bool y = false;
    bool z = false;
};

auto is_valid_map_slug(const std::string& slug) -> bool
{
    if(slug.empty())
    {
        return false;
    }
    return std::all_of(slug.begin(), slug.end(), [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

auto normalize_content_root(std::string root, const std::string& expected_map_slug = {}) -> std::string
{
    if(root.empty())
    {
        const auto map_slug = expected_map_slug.empty() ? std::string("login") : expected_map_slug;
        root = "app:/data/" + map_slug;
    }

    while(!root.empty() && (root.back() == '/' || root.back() == '\\'))
    {
        root.pop_back();
    }

    if(root.size() > 1 && root.back() == ':')
    {
        const auto protocol_slug = root.substr(0, root.size() - 1);
        if(!is_valid_map_slug(protocol_slug))
        {
            throw std::runtime_error("load_login: invalid content-root protocol '" + root + "'");
        }
        if(!expected_map_slug.empty() && protocol_slug != expected_map_slug)
        {
            throw std::runtime_error("load_login: content-root protocol '" + protocol_slug +
                                     "' does not match map '" + expected_map_slug + "'");
        }

        const auto protocol_data_root = protocol_slug + ":/data/" + protocol_slug;
        if(fs::has_known_protocol(protocol_data_root))
        {
            return protocol_data_root;
        }

        const auto app_data_root = std::string("app:/data/") + protocol_slug;
        fs::error_code ec;
        if(fs::has_known_protocol(app_data_root) &&
           fs::is_directory(fs::resolve_protocol(app_data_root), ec) && !ec)
        {
            return app_data_root;
        }
    }

    if(!fs::has_known_protocol(root))
    {
        const auto protocol_root = fs::convert_to_protocol(root).generic_string();
        if(fs::has_known_protocol(protocol_root))
        {
            root = protocol_root;
        }
    }

    if(!fs::has_known_protocol(root))
    {
        fs::error_code ec;
        if(fs::is_directory(root, ec))
        {
            const auto canonical_root = fs::weakly_canonical(root, ec);
            const auto data_dir = canonical_root.parent_path();
            const auto project_root = data_dir.parent_path();
            if(data_dir.filename() == "data" && fs::is_directory(project_root, ec))
            {
                const auto inferred_map_slug = canonical_root.filename().string();
                if(!expected_map_slug.empty() && inferred_map_slug != expected_map_slug)
                {
                    throw std::runtime_error("load_login: content-root directory '" + inferred_map_slug +
                                             "' does not match map '" + expected_map_slug + "'");
                }
            }
            throw std::runtime_error(
                "load_login: content_root is outside a registered project; open its project or use a known asset protocol");
        }

        throw std::runtime_error(
            "load_login: content_root must use a known asset protocol or existing directory, got '" + root + "'");
    }

    return root;
}

auto map_title_from_slug(const std::string& slug) -> std::string
{
    std::string title = slug.empty() ? std::string("Map") : slug;
    title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
    return title;
}

auto make_generated_map_asset_key(const std::string& map_slug, uint64_t generation, const std::string& suffix)
    -> std::string
{
    return "app:/generated/pw_map_" + map_slug + "_" + std::to_string(generation) + "_" + suffix;
}

auto make_asset_key(const std::string& content_root, std::string relative) -> std::string
{
    std::replace(relative.begin(), relative.end(), '\\', '/');
    while(!relative.empty() && relative.front() == '/')
    {
        relative.erase(relative.begin());
    }
    return normalize_content_root(content_root) + "/" + relative;
}

auto read_json_asset(const std::string& asset_key) -> json
{
    const auto path = fs::resolve_protocol(asset_key);
    std::ifstream file(path);
    if(!file)
    {
        throw std::runtime_error("load_login: failed to open '" + asset_key + "'");
    }

    auto doc = json::parse(file, nullptr, false);
    if(doc.is_discarded())
    {
        throw std::runtime_error("load_login: failed to parse '" + asset_key + "'");
    }
    return doc;
}

auto read_map_lights_document(const std::string& content_root, const std::string& map_slug) -> json
{
    auto lights_doc = read_json_asset(make_asset_key(content_root, "lights/" + map_slug + ".eds.lights.json"));
    if(!lights_doc.contains("lights") || !lights_doc["lights"].is_array())
    {
        throw std::runtime_error("load_login: map '" + map_slug + "' lights document has no lights[]");
    }
    return lights_doc;
}

auto trim_login_scene_text(std::string text) -> std::string
{
    auto begin = size_t{0};
    while(begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
    {
        ++begin;
    }
    auto end = text.size();
    while(end > begin && std::isspace(static_cast<unsigned char>(text[end - 1u])))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

auto parse_login_scene_float(const std::string& text, float& out_value) -> bool
{
    char* end = nullptr;
    const char* begin = text.c_str();
    out_value = std::strtof(begin, &end);
    if(end == begin)
    {
        return false;
    }
    while(*end != '\0' && std::isspace(static_cast<unsigned char>(*end)))
    {
        ++end;
    }
    return *end == '\0';
}

auto parse_login_scene_key_index(const std::string& key, const char* prefix, int max_count, int& out_index) -> bool
{
    const auto prefix_text = std::string(prefix);
    if(key.size() <= prefix_text.size() || key.compare(0u, prefix_text.size(), prefix_text) != 0)
    {
        return false;
    }
    size_t index = 0;
    for(size_t i = prefix_text.size(); i < key.size(); ++i)
    {
        const auto ch = static_cast<unsigned char>(key[i]);
        if(!std::isdigit(ch))
        {
            return false;
        }
        index = index * 10u + static_cast<size_t>(key[i] - '0');
        if(index >= static_cast<size_t>(max_count))
        {
            return false;
        }
    }
    out_index = static_cast<int>(index);
    return true;
}

auto get_login_scene_vec3_slot(std::vector<partial_login_scene_vec3>& values, int index) -> partial_login_scene_vec3*
{
    if(index < 0 || index >= kLoginSceneNewCharMax)
    {
        return nullptr;
    }
    const auto slot = static_cast<size_t>(index);
    if(slot >= values.size())
    {
        values.resize(slot + 1u);
    }
    return &values[slot];
}

void parse_login_scene_camera_value(std::array<partial_login_scene_camera, kLoginSceneCameraCount>& cameras,
                                    const std::string& key,
                                    float value)
{
    int index = 0;
    if(parse_login_scene_key_index(key, "PosX", kLoginSceneCameraCount, index))
    {
        cameras[static_cast<size_t>(index)].camera.pos.x = value;
        cameras[static_cast<size_t>(index)].pos_x = true;
        return;
    }
    if(parse_login_scene_key_index(key, "PosY", kLoginSceneCameraCount, index))
    {
        cameras[static_cast<size_t>(index)].camera.pos.y = value;
        cameras[static_cast<size_t>(index)].pos_y = true;
        return;
    }
    if(parse_login_scene_key_index(key, "PosZ", kLoginSceneCameraCount, index))
    {
        cameras[static_cast<size_t>(index)].camera.pos.z = value;
        cameras[static_cast<size_t>(index)].pos_z = true;
        return;
    }
    if(parse_login_scene_key_index(key, "DirX", kLoginSceneCameraCount, index))
    {
        cameras[static_cast<size_t>(index)].camera.dir.x = value;
        return;
    }
    if(parse_login_scene_key_index(key, "DirY", kLoginSceneCameraCount, index))
    {
        cameras[static_cast<size_t>(index)].camera.dir.y = value;
        return;
    }
    if(parse_login_scene_key_index(key, "DirZ", kLoginSceneCameraCount, index))
    {
        cameras[static_cast<size_t>(index)].camera.dir.z = value;
        return;
    }
    if(parse_login_scene_key_index(key, "UpX", kLoginSceneCameraCount, index))
    {
        cameras[static_cast<size_t>(index)].camera.up.x = value;
        return;
    }
    if(parse_login_scene_key_index(key, "UpY", kLoginSceneCameraCount, index))
    {
        cameras[static_cast<size_t>(index)].camera.up.y = value;
        return;
    }
    if(parse_login_scene_key_index(key, "UpZ", kLoginSceneCameraCount, index))
    {
        cameras[static_cast<size_t>(index)].camera.up.z = value;
        return;
    }
}

void parse_login_scene_new_char_value(std::vector<partial_login_scene_vec3>& positions,
                                      const std::string& key,
                                      float value)
{
    int index = 0;
    if(parse_login_scene_key_index(key, "PosX", kLoginSceneNewCharMax, index))
    {
        auto* slot = get_login_scene_vec3_slot(positions, index);
        if(slot)
        {
            slot->value.x = value;
            slot->x = true;
        }
        return;
    }
    if(parse_login_scene_key_index(key, "PosY", kLoginSceneNewCharMax, index))
    {
        auto* slot = get_login_scene_vec3_slot(positions, index);
        if(slot)
        {
            slot->value.y = value;
            slot->y = true;
        }
        return;
    }
    if(parse_login_scene_key_index(key, "PosZ", kLoginSceneNewCharMax, index))
    {
        auto* slot = get_login_scene_vec3_slot(positions, index);
        if(slot)
        {
            slot->value.z = value;
            slot->z = true;
        }
        return;
    }
}

void parse_login_scene_center_value(partial_login_scene_vec3& center, const std::string& key, float value)
{
    if(key == "PosX0")
    {
        center.value.x = value;
        center.x = true;
        return;
    }
    if(key == "PosY0")
    {
        center.value.y = value;
        center.y = true;
        return;
    }
    if(key == "PosZ0")
    {
        center.value.z = value;
        center.z = true;
        return;
    }
}

void finalize_login_scene_cameras(login_scene_config& config,
                                  const std::array<partial_login_scene_camera, kLoginSceneCameraCount>& cameras)
{
    for(size_t i = 0; i < cameras.size(); ++i)
    {
        config.cameras[i] = cameras[i].camera;
        config.cameras[i].valid = cameras[i].pos_x && cameras[i].pos_y && cameras[i].pos_z;
    }
}

void finalize_login_scene_positions(login_scene_config& config, const std::vector<partial_login_scene_vec3>& positions)
{
    for(const auto& position : positions)
    {
        if(!position.x || !position.y || !position.z)
        {
            return;
        }
        config.new_char_positions.push_back(position.value);
    }
}

auto parse_login_scene_config(const std::string& content_root) -> login_scene_config
{
    login_scene_config config;
    std::ifstream file;
    try
    {
        const auto path = fs::resolve_protocol(make_asset_key(content_root, "scenectrl.ini"));
        file.open(path);
    }
    catch(const std::exception&)
    {
        return config;
    }
    if(!file)
    {
        return config;
    }
    config.loaded = true;
    std::array<partial_login_scene_camera, kLoginSceneCameraCount> cameras{};
    std::vector<partial_login_scene_vec3> new_char_positions;
    partial_login_scene_vec3 new_char_center;
    std::string section;
    std::string line;
    while(std::getline(file, line))
    {
        const auto comment = line.find_first_of(";#");
        if(comment != std::string::npos)
        {
            line.erase(comment);
        }
        line = trim_login_scene_text(line);
        if(line.empty())
        {
            continue;
        }
        if(line.front() == '[' && line.back() == ']')
        {
            section = trim_login_scene_text(line.substr(1u, line.size() - 2u));
            continue;
        }
        const auto equals = line.find('=');
        if(equals == std::string::npos)
        {
            continue;
        }
        const auto key = trim_login_scene_text(line.substr(0u, equals));
        const auto value_text = trim_login_scene_text(line.substr(equals + 1u));
        float value = 0.0f;
        if(!parse_login_scene_float(value_text, value))
        {
            continue;
        }
        if(section == "Camera")
        {
            parse_login_scene_camera_value(cameras, key, value);
            continue;
        }
        if(section == "NewChar")
        {
            parse_login_scene_new_char_value(new_char_positions, key, value);
            continue;
        }
        if(section == "NewCharCenter")
        {
            parse_login_scene_center_value(new_char_center, key, value);
            continue;
        }
    }
    finalize_login_scene_cameras(config, cameras);
    finalize_login_scene_positions(config, new_char_positions);
    if(new_char_center.x && new_char_center.y && new_char_center.z)
    {
        config.new_char_center = new_char_center.value;
    }
    return config;
}

auto read_material_texture_ref(const std::string& content_root, const std::string& material_ref) -> std::string
{
    const auto material_doc = read_json_asset(make_asset_key(content_root, material_ref));
    std::string texture_ref = material_doc.value("textureRef", std::string{});
    if(texture_ref.empty() && material_doc.contains("params") && material_doc["params"].is_object())
    {
        texture_ref = material_doc["params"].value("diffuseTextureRef", std::string{});
    }
    if(texture_ref.empty())
    {
        throw std::runtime_error("load_login: material '" + material_ref + "' has no textureRef");
    }
    return texture_ref;
}

auto read_boolish_member(const json& item, const char* key, bool fallback = false) -> bool
{
    if(!item.contains(key))
    {
        return fallback;
    }

    const auto& value = item[key];
    if(value.is_boolean())
    {
        return value.get<bool>();
    }
    if(value.is_number_integer())
    {
        return value.get<int>() != 0;
    }
    if(value.is_string())
    {
        const auto text = value.get<std::string>();
        return text == "true" || text == "1" || text == "TRUE" || text == "True";
    }

    return fallback;
}

struct login_material_info
{
    std::vector<std::string> textures;
    bool alpha_blend = false;
    bool alpha_test = false;
    bool two_sided = false;
};

auto read_login_material_info(const std::string& content_root, const std::string& material_ref) -> login_material_info
{
    const auto material_doc = read_json_asset(make_asset_key(content_root, material_ref));
    std::string fallback_ref = material_doc.value("textureRef", std::string{});
    if(fallback_ref.empty() && material_doc.contains("params") && material_doc["params"].is_object())
    {
        fallback_ref = material_doc["params"].value("diffuseTextureRef", std::string{});
    }

    login_material_info info;
    if(material_doc.contains("params") && material_doc["params"].is_object())
    {
        const auto& params = material_doc["params"];
        info.alpha_blend = read_boolish_member(params, "alphaBlend", false);
        info.alpha_test = read_boolish_member(params, "alphaTest", false);
        info.two_sided = read_boolish_member(params, "twoSided", false);
    }

    std::vector<std::string>& refs = info.textures;
    if(material_doc.contains("materialSlots") && material_doc["materialSlots"].is_array())
    {
        for(const auto& slot : material_doc["materialSlots"])
        {
            if(!slot.is_object())
            {
                continue;
            }

            const int index = slot.value("index", static_cast<int>(refs.size()));
            if(index < 0)
            {
                continue;
            }

            std::string texture_ref = slot.value("diffuseTextureRef", std::string{});
            if(texture_ref.empty())
            {
                texture_ref = fallback_ref;
            }
            if(texture_ref.empty())
            {
                continue;
            }

            info.alpha_test = info.alpha_test || read_boolish_member(slot, "alphaTest", false);
            info.two_sided = info.two_sided || read_boolish_member(slot, "twoSided", false);

            if(static_cast<size_t>(index) >= refs.size())
            {
                refs.resize(static_cast<size_t>(index) + 1u);
            }
            refs[static_cast<size_t>(index)] = std::move(texture_ref);
        }
    }

    if(refs.empty())
    {
        if(fallback_ref.empty())
        {
            throw std::runtime_error("load_login: material '" + material_ref + "' has no textureRef");
        }
        refs.emplace_back(fallback_ref);
    }

    for(auto& ref : refs)
    {
        if(ref.empty())
        {
            ref = fallback_ref;
        }
    }

    return info;
}

auto read_material_texture_refs(const std::string& content_root, const std::string& material_ref) -> std::vector<std::string>
{
    return read_login_material_info(content_root, material_ref).textures;
}

void add_unique_terrain_albedo_ref(std::vector<std::string>& refs, std::string ref)
{
    if(ref.empty())
    {
        return;
    }
    if(std::find(refs.begin(), refs.end(), ref) == refs.end())
    {
        refs.emplace_back(std::move(ref));
    }
}

auto read_login_terrain_albedo_refs(const std::string& content_root, const std::string& map_slug) -> std::vector<std::string>
{
    std::vector<std::string> refs;
    try
    {
        const auto layers_doc = read_json_asset(make_asset_key(content_root, "terrain/" + map_slug + "/layers.json"));
        if(layers_doc.contains("bakedAlbedo") && layers_doc["bakedAlbedo"].is_object())
        {
            const auto& baked_albedo = layers_doc["bakedAlbedo"];
            if(baked_albedo.contains("path") && baked_albedo["path"].is_string())
            {
                const auto baked_albedo_ref = baked_albedo["path"].get<std::string>();
                if(!baked_albedo_ref.empty())
                {
                    APPLOG_INFO("load_login terrain albedo baked preference: ref='{}'", baked_albedo_ref);
                    add_unique_terrain_albedo_ref(refs, baked_albedo_ref);
                }
            }
        }

        if(!layers_doc.contains("layers") || !layers_doc["layers"].is_array())
        {
            return refs;
        }

        for(const auto& layer : layers_doc["layers"])
        {
            if(!layer.is_object() || !layer.contains("albedo") || !layer["albedo"].is_object())
            {
                continue;
            }

            const auto& albedo = layer["albedo"];
            if(!albedo.contains("path") || !albedo["path"].is_string())
            {
                continue;
            }

            const auto albedo_ref = albedo["path"].get<std::string>();
            if(!albedo_ref.empty())
            {
                add_unique_terrain_albedo_ref(refs, albedo_ref);
            }
        }
    }
    catch(const std::exception&)
    {
    }

    return refs;
}

auto read_vec3_member(const json& item, const char* key) -> math::vec3
{
    if(!item.contains(key) || !item[key].is_array() || item[key].size() < 3)
    {
        throw std::runtime_error(std::string("load_login: building has no valid '") + key + "'");
    }

    return {item[key][0].get<float>(), item[key][1].get<float>(), item[key][2].get<float>()};
}

auto read_vec3_array(const json& item, const char* label) -> math::vec3
{
    if(!item.is_array() || item.size() < 3)
    {
        throw std::runtime_error(std::string("load_login: foliage has no valid '") + label + "'");
    }

    return {item[0].get<float>(), item[1].get<float>(), item[2].get<float>()};
}

auto read_login_light_vec3_member(const json& item, const char* key) -> math::vec3
{
    if(!item.contains(key) || !item[key].is_array() || item[key].size() < 3)
    {
        throw std::runtime_error(std::string("load_login: light has no valid '") + key + "'");
    }

    return {item[key][0].get<float>(), item[key][1].get<float>(), item[key][2].get<float>()};
}

auto read_login_light_color(const json& item) -> math::color
{
    const auto color = read_login_light_vec3_member(item, "colorRGB");
    return {color.x, color.y, color.z, 1.0f};
}

auto normalize_login_light_direction(const math::vec3& direction) -> math::vec3
{
    if(math::dot(direction, direction) <= 0.000001f)
    {
        throw std::runtime_error("load_login: directional light has zero direction");
    }

    return math::normalize(direction);
}

void validate_login_light_vec3(const math::vec3& value, const char* key)
{
    if(!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
    {
        throw std::runtime_error(std::string("load_login: light '") + key + "' contains a non-finite value");
    }
}

void read_optional_login_light_float(const json& item, const char* key, float& value, bool& present)
{
    if(!item.contains(key))
    {
        return;
    }
    value = item[key].get<float>();
    if(!std::isfinite(value))
    {
        throw std::runtime_error(std::string("load_login: light '") + key + "' is not finite");
    }
    present = true;
}

auto parse_map_lights(const std::string& content_root, const std::string& map_slug)
    -> std::vector<mcp_system::login_light>
{
    const auto lights_doc = read_map_lights_document(content_root, map_slug);
    const auto& items = lights_doc["lights"];
    std::vector<mcp_system::login_light> result;
    result.reserve(items.size());

    for(size_t i = 0; i < items.size(); ++i)
    {
        const auto& item = items[i];
        if(!item.is_object())
        {
            continue;
        }

        try
        {
            const auto type = item.value("type", std::string{});
            mcp_system::login_light parsed;
            parsed.source_index = static_cast<uint32_t>(i);

            if(type == "directional")
            {
                parsed.type = mcp_system::login_light::kind::directional;
                parsed.direction = read_login_light_vec3_member(item, "direction");
                validate_login_light_vec3(parsed.direction, "direction");
                parsed.direction = normalize_login_light_direction(parsed.direction);
            }
            else if(type == "point")
            {
                parsed.type = mcp_system::login_light::kind::point;
                parsed.position = read_login_light_vec3_member(item, "position");
                validate_login_light_vec3(parsed.position, "position");
                read_optional_login_light_float(item, "range", parsed.range, parsed.has_range);
            }
            else
            {
                continue;
            }

            parsed.color = read_login_light_color(item);
            validate_login_light_vec3(
                {parsed.color.value.r, parsed.color.value.g, parsed.color.value.b}, "colorRGB");
            read_optional_login_light_float(item, "intensity", parsed.intensity, parsed.has_intensity);
            result.emplace_back(parsed);
        }
        catch(const std::exception& e)
        {
            throw std::runtime_error("load_login: invalid light[" + std::to_string(i) + "] for map '" + map_slug +
                                     "': " + e.what());
        }
    }

    return result;
}

auto map_source_position_to_unravel(const math::vec3& source_pos) -> math::vec3
{
    return source_pos;
}

auto map_source_forward_to_unravel(const math::vec3& source_dir) -> math::vec3
{
    math::vec3 forward{source_dir.x, source_dir.y, source_dir.z};
    if(math::dot(forward, forward) <= 0.000001f)
    {
        return {0.0f, 0.0f, 1.0f};
    }

    return math::normalize(forward);
}

auto map_source_up_to_unravel(const math::vec3& source_up) -> math::vec3
{
    math::vec3 up{source_up.x, source_up.y, source_up.z};
    if(math::dot(up, up) <= 0.000001f)
    {
        return {0.0f, 1.0f, 0.0f};
    }

    return math::normalize(up);
}

auto decode_r16_heightmap(const std::string& heightmap_key, uint32_t& width, uint32_t& height) -> std::vector<float>
{
    const auto resolved = fs::resolve_protocol(heightmap_key).string();
    auto* image = imageLoad(bx::FilePath(resolved.c_str()), bgfx::TextureFormat::Count);
    if(image == nullptr || image->m_data == nullptr)
    {
        throw std::runtime_error("load_login: failed to decode '" + heightmap_key + "'");
    }

    width = image->m_width;
    height = image->m_height;
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<float> heights(count);

    switch(image->m_format)
    {
        case bimg::TextureFormat::R16:
        {
            const auto* src = static_cast<const uint16_t*>(image->m_data);
            for(size_t i = 0; i < count; ++i)
            {
                heights[i] = static_cast<float>(src[i]) / 65535.0f;
            }
            break;
        }
        case bimg::TextureFormat::RGBA16:
        {
            const auto* src = static_cast<const uint16_t*>(image->m_data);
            for(size_t i = 0; i < count; ++i)
            {
                heights[i] = static_cast<float>(src[i * 4]) / 65535.0f;
            }
            break;
        }
        case bimg::TextureFormat::R8:
        {
            const auto* src = static_cast<const uint8_t*>(image->m_data);
            for(size_t i = 0; i < count; ++i)
            {
                heights[i] = static_cast<float>(src[i]) / 255.0f;
            }
            break;
        }
        case bimg::TextureFormat::RGBA8:
        {
            const auto* src = static_cast<const uint8_t*>(image->m_data);
            for(size_t i = 0; i < count; ++i)
            {
                heights[i] = static_cast<float>(src[i * 4]) / 255.0f;
            }
            break;
        }
        default:
            bimg::imageFree(image);
            throw std::runtime_error("load_login: unsupported heightmap format for '" + heightmap_key + "'");
    }

    bimg::imageFree(image);
    return heights;
}

auto load_login_terrain_heightfield(const std::string& content_root, const std::string& map_slug) -> terrain_heightfield
{
    const auto layers_doc = read_json_asset(make_asset_key(content_root, "terrain/" + map_slug + "/layers.json"));
    const auto heightmap_doc = layers_doc.contains("heightmap") && layers_doc["heightmap"].is_object()
                                   ? layers_doc["heightmap"]
                                   : json::object();

    const auto heightmap_ref = heightmap_doc.value("path", std::string("terrain/" + map_slug + "/height.r16.png"));

    terrain_heightfield terrain;
    terrain.height_min = heightmap_doc.value("heightMin", 134.59677124023438f);
    terrain.height_max = heightmap_doc.value("heightMax", 424.2374267578125f);
    terrain.world_width = 1024.0f;
    terrain.world_depth = 1024.0f;
    if(layers_doc.contains("world") && layers_doc["world"].is_object())
    {
        const auto& world_doc = layers_doc["world"];
        const float world_width = world_doc.value("widthM", 1024.0f);
        const float world_depth = world_doc.value("depthM", 1024.0f);
        const bool has_world_left = world_doc.contains("leftM");
        const bool has_world_top = world_doc.contains("topM");
        if(has_world_left != has_world_top)
        {
            throw std::runtime_error("load_login: terrain world rect requires both leftM and topM");
        }
        terrain.world_width = world_width;
        terrain.world_depth = world_depth;
        if(has_world_left)
        {
            const float world_left = world_doc.at("leftM").get<float>();
            const float world_top = world_doc.at("topM").get<float>();
            if(!std::isfinite(world_left) || !std::isfinite(world_top) ||
               !std::isfinite(world_left + world_width) || !std::isfinite(world_top - world_depth))
            {
                throw std::runtime_error("load_login: terrain world rect is not finite");
            }
            terrain.world_left = world_left;
            terrain.world_top = world_top;
            terrain.has_world_origin = true;
        }
    }
    if(!std::isfinite(terrain.height_min) || !std::isfinite(terrain.height_max) ||
       terrain.height_max < terrain.height_min || !std::isfinite(terrain.world_width) ||
       !std::isfinite(terrain.world_depth) || terrain.world_width <= 0.0f || terrain.world_depth <= 0.0f)
    {
        throw std::runtime_error("load_login: terrain height or world dimensions are invalid");
    }
    terrain.heights = decode_r16_heightmap(make_asset_key(content_root, heightmap_ref), terrain.width, terrain.height);
    if(!terrain.is_valid())
    {
        throw std::runtime_error("load_login: heightmap is too small or invalid");
    }

    return terrain;
}

struct login_buildings_parse_result
{
    std::vector<mcp_system::login_building> buildings;
    uint32_t duplicate_skipped = 0;
};

struct login_foliage_parse_result
{
    std::vector<mcp_system::login_foliage> foliage;
    uint32_t skipped = 0;
};

struct login_water_parse_result
{
    std::vector<mcp_system::login_water> water;
    uint32_t skipped = 0;
};

struct terrain_rgb
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

auto mix_channel(uint8_t a, uint8_t b, float t) -> uint8_t
{
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::round(static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * clamped));
}

auto mix_color(terrain_rgb a, terrain_rgb b, float t) -> terrain_rgb
{
    return {mix_channel(a.r, b.r, t), mix_channel(a.g, b.g, t), mix_channel(a.b, b.b, t)};
}

auto terrain_height_color(float h) -> terrain_rgb
{
    h = std::clamp(h, 0.0f, 1.0f);

    constexpr terrain_rgb low{78, 93, 55};
    constexpr terrain_rgb grass{103, 122, 63};
    constexpr terrain_rgb earth{132, 104, 70};
    constexpr terrain_rgb rock{137, 132, 119};
    constexpr terrain_rgb high{176, 173, 158};

    if(h < 0.28f)
    {
        return mix_color(low, grass, h / 0.28f);
    }
    if(h < 0.55f)
    {
        return mix_color(grass, earth, (h - 0.28f) / 0.27f);
    }
    if(h < 0.82f)
    {
        return mix_color(earth, rock, (h - 0.55f) / 0.27f);
    }
    return mix_color(rock, high, (h - 0.82f) / 0.18f);
}

auto create_login_terrain_debug_albedo(rtti::context& ctx,
                                       const std::string& map_slug,
                                       uint64_t generation,
                                       const terrain_heightfield& terrain,
                                       std::vector<std::string>& generated_texture_keys)
    -> asset_handle<gfx::texture>
{
    if(!terrain.is_valid())
    {
        return {};
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(terrain.width) * static_cast<size_t>(terrain.height) * 4u, 255u);
    const auto sample_normalized = [&terrain](uint32_t x, uint32_t y) -> float
    {
        x = std::min(x, terrain.width - 1u);
        y = std::min(y, terrain.height - 1u);
        return terrain.heights[static_cast<size_t>(y) * static_cast<size_t>(terrain.width) + static_cast<size_t>(x)];
    };

    for(uint32_t y = 0; y < terrain.height; ++y)
    {
        for(uint32_t x = 0; x < terrain.width; ++x)
        {
            const float h = sample_normalized(x, y);
            const float hx0 = sample_normalized(x > 0 ? x - 1u : x, y);
            const float hx1 = sample_normalized(std::min(x + 1u, terrain.width - 1u), y);
            const float hy0 = sample_normalized(x, y > 0 ? y - 1u : y);
            const float hy1 = sample_normalized(x, std::min(y + 1u, terrain.height - 1u));
            const float slope = std::clamp(std::sqrt((hx1 - hx0) * (hx1 - hx0) + (hy1 - hy0) * (hy1 - hy0)) * 18.0f,
                                           0.0f,
                                           1.0f);

            auto color = terrain_height_color(h);
            color = mix_color(color, terrain_rgb{128, 128, 118}, slope * 0.55f);
            const float shade = 0.82f + h * 0.20f;

            const size_t offset = (static_cast<size_t>(y) * static_cast<size_t>(terrain.width) + static_cast<size_t>(x)) * 4u;
            pixels[offset + 0u] = static_cast<uint8_t>(std::clamp(std::round(static_cast<float>(color.r) * shade), 0.0f, 255.0f));
            pixels[offset + 1u] = static_cast<uint8_t>(std::clamp(std::round(static_cast<float>(color.g) * shade), 0.0f, 255.0f));
            pixels[offset + 2u] = static_cast<uint8_t>(std::clamp(std::round(static_cast<float>(color.b) * shade), 0.0f, 255.0f));
            pixels[offset + 3u] = 255u;
        }
    }

    const auto* mem = gfx::copy(pixels.data(), static_cast<uint32_t>(pixels.size()));
    auto texture = std::make_shared<gfx::texture>(static_cast<uint16_t>(terrain.width),
                                                  static_cast<uint16_t>(terrain.height),
                                                  false,
                                                  1,
                                                  gfx::texture_format::RGBA8,
                                                  BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
                                                  mem);
    if(!texture || !texture->is_valid())
    {
        return {};
    }

    auto& am = ctx.get_cached<asset_manager>();
    const auto key = make_generated_map_asset_key(map_slug, generation, "terrain_debug_albedo");
    generated_texture_keys.emplace_back(key);
    return am.get_asset_from_instance<gfx::texture>(key, texture);
}

auto load_login_terrain_albedo(rtti::context& ctx, const std::string& content_root, const std::string& map_slug)
    -> asset_handle<gfx::texture>
{
    try
    {
        const auto albedo_refs = read_login_terrain_albedo_refs(content_root, map_slug);
        if(albedo_refs.empty())
        {
            return {};
        }

        auto& am = ctx.get_cached<asset_manager>();
        for(const auto& albedo_ref : albedo_refs)
        {
            const auto texture_key = fs::has_known_protocol(albedo_ref) ? albedo_ref : make_asset_key(content_root, albedo_ref);
            const auto texture_path = fs::resolve_protocol(texture_key);
            fs::error_code exists_ec;
            if(!fs::exists(texture_path, exists_ec) || exists_ec)
            {
                APPLOG_INFO("load_login terrain albedo missing: ref='{}' key='{}' path='{}'",
                            albedo_ref,
                            texture_key,
                            texture_path.generic_string());
                continue;
            }

            auto texture_handle = am.get_asset<gfx::texture>(texture_key, load_flags::standard);
            texture_handle.submit();

            auto texture_instance = texture_handle.is_valid() ? texture_handle.get(true) : std::shared_ptr<gfx::texture>{};
            const bool texture_loaded = static_cast<bool>(texture_instance);
            const bool native_valid = texture_instance && texture_instance->is_valid();
            const auto native_idx = native_valid ? texture_instance->native_handle().idx : bgfx::kInvalidHandle;
            const auto width = texture_instance ? texture_instance->info.width : 0;
            const auto height = texture_instance ? texture_instance->info.height : 0;
            const auto format = texture_instance ? static_cast<int>(texture_instance->info.format) : -1;
            const bool texture_usable = native_valid && width > 0 && height > 0;

            APPLOG_INFO("load_login terrain albedo requested: ref='{}' key='{}' handle_valid={} ready={} loaded={} "
                        "native_valid={} native_idx={} width={} height={} format={} usable={}",
                        albedo_ref,
                        texture_key,
                        texture_handle.is_valid(),
                        texture_handle.is_ready(),
                        texture_loaded,
                        native_valid,
                        native_idx,
                        width,
                        height,
                        format,
                        texture_usable);

            if(texture_usable)
            {
                return texture_handle;
            }
        }
        return {};
    }
    catch(const std::exception&)
    {
    }

    return {};
}

auto positions_match(const math::vec3& lhs, const math::vec3& rhs) -> bool
{
    const auto delta = lhs - rhs;
    return math::dot(delta, delta) <= kDuplicatePositionEpsilon * kDuplicatePositionEpsilon;
}

auto make_login_buildings(const std::string& content_root, const std::string& map_slug, const terrain_heightfield& terrain)
    -> login_buildings_parse_result
{
    const auto scene_doc = read_json_asset(make_asset_key(content_root, "maps/" + map_slug + "/scene.eds.json"));
    if(!scene_doc.contains("buildings") || !scene_doc["buildings"].is_array())
    {
        throw std::runtime_error("load_login: scene.eds.json has no buildings[]");
    }

    login_buildings_parse_result result;
    for(const auto& item : scene_doc["buildings"])
    {
        if(!item.contains("openFormat") || !item["openFormat"].is_object())
        {
            continue;
        }

        const auto& open_format = item["openFormat"];
        if(!open_format.value("convertedToOpenFormat", false))
        {
            continue;
        }

        const auto model_ref = open_format.value("model", std::string{});
        const auto material_ref = open_format.value("material", std::string{});
        if(model_ref.empty() || material_ref.empty())
        {
            continue;
        }

        const auto source_position = read_vec3_member(item, "pos");
        const auto mapped_position = map_source_position_to_unravel(source_position);
        if(std::any_of(result.buildings.begin(),
                       result.buildings.end(),
                       [&mapped_position](const mcp_system::login_building& accepted) {
                           return positions_match(accepted.position, mapped_position);
                       }))
        {
            ++result.duplicate_skipped;
            continue;
        }

        mcp_system::login_building building;
        building.name = item.value("name", std::string("Login Building ") + std::to_string(result.buildings.size()));
        building.model = model_ref;
        const auto material_info = read_login_material_info(content_root, material_ref);
        building.textures = material_info.textures;
        building.texture = building.textures.empty() ? std::string{} : building.textures.front();
        building.alpha_blend = material_info.alpha_blend;
        building.alpha_test = material_info.alpha_test;
        building.position = mapped_position;
        building.forward = map_source_forward_to_unravel(read_vec3_member(item, "dir"));
        building.up = map_source_up_to_unravel(read_vec3_member(item, "up"));
        building.source_position_y = source_position.y;
        building.terrain_sample_valid =
            terrain.sample_terrain_height(building.position.x, building.position.z, building.terrain_surface_y);
        result.buildings.emplace_back(std::move(building));
    }

    APPLOG_INFO("load_login parsed buildings: placed={} skipped={}",
                result.buildings.size(),
                result.duplicate_skipped);

    return result;
}

auto login_scene_payload_asset_key(const std::string& content_root, const std::string& map_slug, std::string payload_ref)
    -> std::string
{
    std::replace(payload_ref.begin(), payload_ref.end(), '\\', '/');
    while(!payload_ref.empty() && payload_ref.front() == '/')
    {
        payload_ref.erase(payload_ref.begin());
    }

    if(fs::has_known_protocol(payload_ref))
    {
        return payload_ref;
    }
    if(payload_ref.rfind("maps/", 0) == 0)
    {
        return make_asset_key(content_root, payload_ref);
    }
    return make_asset_key(content_root, "maps/" + map_slug + "/" + payload_ref);
}

auto make_login_water(const std::string& content_root, const std::string& map_slug) -> login_water_parse_result
{
    const auto scene_doc = read_json_asset(make_asset_key(content_root, "maps/" + map_slug + "/scene.eds.json"));

    login_water_parse_result result;
    if(!scene_doc.contains("nodes") || !scene_doc["nodes"].is_array())
    {
        APPLOG_WARNING("load_login: scene.eds.json has no nodes[]; water skipped");
        return result;
    }

    std::vector<std::string> accepted_keys;
    for(const auto& item : scene_doc["nodes"])
    {
        if(!item.is_object() || item.value("kind", std::string{}) != "Water")
        {
            continue;
        }

        try
        {
            if(!item.contains("openFormat") || !item["openFormat"].is_object())
            {
                ++result.skipped;
                continue;
            }

            const auto& open_format = item["openFormat"];
            if(!open_format.value("convertedToOpenFormat", false))
            {
                ++result.skipped;
                continue;
            }

            std::string payload_ref = open_format.value("surface", std::string{});
            if(payload_ref.empty())
            {
                payload_ref = item.value("payload", std::string{});
            }
            if(payload_ref.empty())
            {
                ++result.skipped;
                continue;
            }

            const auto payload_doc = read_json_asset(login_scene_payload_asset_key(content_root, map_slug, payload_ref));
            if(!payload_doc.contains("waterSurface") || !payload_doc["waterSurface"].is_object())
            {
                ++result.skipped;
                continue;
            }

            const auto& water_surface = payload_doc["waterSurface"];
            std::string dedupe_key = payload_ref;
            if(water_surface.contains("area") && water_surface["area"].is_object())
            {
                const auto& area = water_surface["area"];
                dedupe_key = std::to_string(area.value("areaId", 0u)) + ":" +
                             std::to_string(area.value("subTerrain", -1)) + ":" +
                             std::to_string(area.value("dataIndex", -1)) + ":" +
                             area.value("gridFlagsHex", std::string{});
            }

            if(std::find(accepted_keys.begin(), accepted_keys.end(), dedupe_key) != accepted_keys.end())
            {
                ++result.skipped;
                continue;
            }
            accepted_keys.emplace_back(std::move(dedupe_key));

            mcp_system::login_water water;
            water.name = item.value("name", std::string("Water ") + std::to_string(result.water.size()));
            water.payload = payload_ref;
            if(water_surface.contains("mesh") && water_surface["mesh"].is_object())
            {
                water.visible_cells = water_surface["mesh"].value("visibleCells", 0u);
            }
            result.water.emplace_back(std::move(water));
        }
        catch(const std::exception& e)
        {
            ++result.skipped;
            APPLOG_WARNING("load_login water skipped: name='{}' reason='{}'",
                           item.value("name", std::string("<unnamed>")),
                           e.what());
        }
    }

    APPLOG_INFO("load_login parsed water: placed={} skipped={}", result.water.size(), result.skipped);

    return result;
}

auto make_login_foliage(const std::string& content_root, const std::string& map_slug, const terrain_heightfield& terrain)
    -> login_foliage_parse_result
{
    const auto scene_doc = read_json_asset(make_asset_key(content_root, "maps/" + map_slug + "/scene.eds.json"));

    login_foliage_parse_result result;
    if(!scene_doc.contains("nodes") || !scene_doc["nodes"].is_array())
    {
        APPLOG_WARNING("load_login: scene.eds.json has no nodes[]; foliage skipped");
        return result;
    }

    for(const auto& item : scene_doc["nodes"])
    {
        if(!item.is_object() || item.value("kind", std::string{}) != "Tree")
        {
            continue;
        }

        try
        {
            if(!item.contains("openFormat") || !item["openFormat"].is_object())
            {
                ++result.skipped;
                continue;
            }

            const auto& open_format = item["openFormat"];
            if(!open_format.value("convertedToOpenFormat", false))
            {
                ++result.skipped;
                continue;
            }

            const auto payload_ref = item.value("payload", std::string{});
            if(payload_ref.empty())
            {
                ++result.skipped;
                continue;
            }

            const auto payload_doc = read_json_asset(login_scene_payload_asset_key(content_root, map_slug, payload_ref));
            if(!payload_doc.contains("foliageInstance") || !payload_doc["foliageInstance"].is_object())
            {
                ++result.skipped;
                continue;
            }

            const auto& foliage_instance = payload_doc["foliageInstance"];
            const json* type_ref = nullptr;
            if(foliage_instance.contains("typeRef") && foliage_instance["typeRef"].is_object())
            {
                type_ref = &foliage_instance["typeRef"];
            }
            else if(open_format.contains("typeRef") && open_format["typeRef"].is_object())
            {
                type_ref = &open_format["typeRef"];
            }

            if(type_ref == nullptr)
            {
                ++result.skipped;
                continue;
            }

            const auto model_ref = type_ref->value("model", std::string{});
            const auto material_ref = type_ref->value("material", std::string{});
            if(model_ref.empty() || material_ref.empty())
            {
                ++result.skipped;
                continue;
            }

            auto source_position = read_vec3_member(item, "pos");
            if(foliage_instance.contains("transform") && foliage_instance["transform"].is_object())
            {
                const auto& transform = foliage_instance["transform"];
                if(transform.contains("position"))
                {
                    source_position = read_vec3_array(transform["position"], "transform.position");
                }
            }

            mcp_system::login_foliage foliage;
            foliage.name = item.value("name", std::string("Tree ") + std::to_string(result.foliage.size()));
            foliage.model = model_ref;
            const auto material_info = read_login_material_info(content_root, material_ref);
            foliage.textures = material_info.textures;
            foliage.texture = foliage.textures.empty() ? std::string{} : foliage.textures.front();
            foliage.alpha_blend = material_info.alpha_blend;
            foliage.alpha_test = material_info.alpha_test || material_info.two_sided;
            foliage.position = map_source_position_to_unravel(source_position);
            foliage.source_position_y = source_position.y;
            foliage.tree_type = foliage_instance.value("treeType", item.value("treeType", -1));
            foliage.terrain_sample_valid =
                terrain.sample_terrain_height(foliage.position.x, foliage.position.z, foliage.terrain_surface_y);

            result.foliage.emplace_back(std::move(foliage));
        }
        catch(const std::exception& e)
        {
            ++result.skipped;
            APPLOG_WARNING("load_login foliage skipped: name='{}' reason='{}'",
                           item.value("name", std::string("<unnamed>")),
                           e.what());
        }
    }

    APPLOG_INFO("load_login parsed foliage: placed={} skipped={}", result.foliage.size(), result.skipped);

    return result;
}

auto build_login_terrain_skirt_mesh_data(const terrain_heightfield& terrain, mesh::load_data& data) -> bool
{
    if(!terrain.is_valid())
    {
        return false;
    }

    const uint32_t segments_x = terrain.width - 1u;
    const uint32_t segments_z = terrain.height - 1u;
    const uint32_t edge_segments = (segments_x + segments_z) * 2u;
    if(edge_segments == 0u)
    {
        return false;
    }

    data = mesh::load_data{};
    data.vertex_format = gfx::mesh_vertex::get_layout();
    data.vertex_count = edge_segments * 4u;
    data.triangle_count = edge_segments * 2u;
    data.vertex_data.resize(static_cast<size_t>(data.vertex_count) * data.vertex_format.getStride());
    data.triangle_data.resize(data.triangle_count);
    data.material_count = 1;

    const float height_range = terrain.height_range();
    const float base_y = 0.0f;

    auto sample_height = [&terrain, height_range](uint32_t x, uint32_t z) -> float
    {
        x = std::min(x, terrain.width - 1u);
        z = std::min(z, terrain.height - 1u);
        return terrain.heights[static_cast<size_t>(z) * static_cast<size_t>(terrain.width) + static_cast<size_t>(x)] *
               height_range;
    };

    auto local_position = [&terrain, segments_x, segments_z, sample_height](uint32_t x, uint32_t z) -> math::vec3
    {
        const float u = static_cast<float>(x) / static_cast<float>(segments_x);
        const float v = static_cast<float>(z) / static_cast<float>(segments_z);
        return {
            (u - 0.5f) * terrain.world_width,
            sample_height(x, z),
            (0.5f - v) * terrain.world_depth};
    };

    auto pack_vertex = [&data](uint32_t index,
                               const math::vec3& position,
                               const math::vec3& normal,
                               const math::vec3& tangent,
                               const math::vec3& bitangent,
                               float u,
                               float v)
    {
        const float packed_position[4] = {position.x, position.y, position.z, 0.0f};
        const float packed_normal[4] = {normal.x, normal.y, normal.z, 0.0f};
        const float packed_tangent[4] = {tangent.x, tangent.y, tangent.z, 0.0f};
        const float packed_bitangent[4] = {bitangent.x, bitangent.y, bitangent.z, 0.0f};
        const float packed_texcoord[4] = {u, v, 0.0f, 0.0f};

        gfx::vertex_pack(packed_position, false, gfx::attribute::Position, data.vertex_format, data.vertex_data.data(), index);
        gfx::vertex_pack(packed_normal, true, gfx::attribute::Normal, data.vertex_format, data.vertex_data.data(), index);
        gfx::vertex_pack(packed_tangent, true, gfx::attribute::Tangent, data.vertex_format, data.vertex_data.data(), index);
        gfx::vertex_pack(packed_bitangent, true, gfx::attribute::Bitangent, data.vertex_format, data.vertex_data.data(), index);
        gfx::vertex_pack(packed_texcoord, true, gfx::attribute::TexCoord0, data.vertex_format, data.vertex_data.data(), index);
    };

    math::bbox bounds;
    uint32_t vertex_index = 0;
    uint32_t triangle_index = 0;
    const math::vec3 bitangent{0.0f, 1.0f, 0.0f};

    auto append_segment = [&](uint32_t x0,
                              uint32_t z0,
                              uint32_t x1,
                              uint32_t z1,
                              const math::vec3& normal,
                              const math::vec3& tangent)
    {
        auto top0 = local_position(x0, z0);
        auto top1 = local_position(x1, z1);
        auto bottom1 = top1;
        auto bottom0 = top0;
        bottom1.y = base_y;
        bottom0.y = base_y;

        const float u0 = static_cast<float>(x0) / static_cast<float>(segments_x);
        const float v0 = static_cast<float>(z0) / static_cast<float>(segments_z);
        const float u1 = static_cast<float>(x1) / static_cast<float>(segments_x);
        const float v1 = static_cast<float>(z1) / static_cast<float>(segments_z);

        const uint32_t i0 = vertex_index++;
        const uint32_t i1 = vertex_index++;
        const uint32_t i2 = vertex_index++;
        const uint32_t i3 = vertex_index++;

        pack_vertex(i0, top0, normal, tangent, bitangent, u0, v0);
        pack_vertex(i1, top1, normal, tangent, bitangent, u1, v1);
        pack_vertex(i2, bottom1, normal, tangent, bitangent, u1, v1);
        pack_vertex(i3, bottom0, normal, tangent, bitangent, u0, v0);

        bounds.add_point(top0);
        bounds.add_point(top1);
        bounds.add_point(bottom1);
        bounds.add_point(bottom0);

        auto& tri0 = data.triangle_data[triangle_index++];
        tri0.data_group_id = 0;
        tri0.indices = {i0, i1, i2};

        auto& tri1 = data.triangle_data[triangle_index++];
        tri1.data_group_id = 0;
        tri1.indices = {i0, i2, i3};
    };

    for(uint32_t x = 0; x < segments_x; ++x)
    {
        append_segment(x, 0u, x + 1u, 0u, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f});
    }
    for(uint32_t z = 0; z < segments_z; ++z)
    {
        append_segment(segments_x, z, segments_x, z + 1u, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
    }
    for(uint32_t x = 0; x < segments_x; ++x)
    {
        append_segment(x, segments_z, x + 1u, segments_z, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f});
    }
    for(uint32_t z = 0; z < segments_z; ++z)
    {
        append_segment(0u, z, 0u, z + 1u, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
    }

    if(vertex_index != data.vertex_count || triangle_index != data.triangle_count)
    {
        return false;
    }

    mesh::submesh skirt_submesh;
    skirt_submesh.data_group_id = 0;
    skirt_submesh.vertex_start = 0;
    skirt_submesh.vertex_count = data.vertex_count;
    skirt_submesh.face_start = 0;
    skirt_submesh.face_count = data.triangle_count;
    skirt_submesh.bbox = bounds;
    data.submeshes.emplace_back(skirt_submesh);
    data.bbox = bounds;
    return true;
}

void create_login_terrain(rtti::context& ctx,
                          const std::string& content_root,
                          const std::string& map_slug,
                          uint64_t generation,
                          const terrain_heightfield& terrain,
                          std::vector<entt::handle>& created_entities,
                          std::vector<std::string>& generated_mesh_keys,
                          std::vector<std::string>& generated_texture_keys)
{
    auto terrain_mesh = std::make_shared<mesh>();
    const bool created = terrain_mesh->create_heightfield(gfx::mesh_vertex::get_layout(),
                                                          terrain.heights,
                                                          terrain.width - 1,
                                                          terrain.height - 1,
                                                          terrain.world_width * 0.5f,
                                                          terrain.world_depth * 0.5f,
                                                          terrain.heightfield_mesh_scale(),
                                                          mesh_create_origin::center,
                                                          true);
    if(!created)
    {
        throw std::runtime_error("load_login: failed to create terrain heightfield");
    }

    auto& am = ctx.get_cached<asset_manager>();
    const auto terrain_key = make_generated_map_asset_key(map_slug, generation, "terrain");
    generated_mesh_keys.emplace_back(terrain_key);
    auto terrain_handle = am.get_asset_from_instance<mesh>(terrain_key, terrain_mesh);

    mesh::load_data terrain_skirt_data;
    if(!build_login_terrain_skirt_mesh_data(terrain, terrain_skirt_data))
    {
        throw std::runtime_error("load_login: failed to create terrain skirt data");
    }

    auto terrain_skirt_mesh = std::make_shared<mesh>();
    if(!terrain_skirt_mesh->load_mesh(std::move(terrain_skirt_data)))
    {
        throw std::runtime_error("load_login: failed to create terrain skirt mesh");
    }
    const auto terrain_skirt_key = make_generated_map_asset_key(map_slug, generation, "terrain_skirt");
    generated_mesh_keys.emplace_back(terrain_skirt_key);
    auto terrain_skirt_handle = am.get_asset_from_instance<mesh>(terrain_skirt_key, terrain_skirt_mesh);

    auto material_instance = std::make_shared<pbr_material>();
    material_instance->set_base_color({1.0f, 1.0f, 1.0f, 1.0f});
    material_instance->set_metalness(0.0f);
    material_instance->set_roughness(0.85f);
    material_instance->set_cull_type(cull_type::none);

    auto terrain_albedo = load_login_terrain_albedo(ctx, content_root, map_slug);
    if(terrain_albedo.is_valid())
    {
        material_instance->set_color_map(terrain_albedo);
        APPLOG_INFO("load_login terrain albedo selected: imported");
    }
    else
    {
        terrain_albedo =
            create_login_terrain_debug_albedo(ctx, map_slug, generation, terrain, generated_texture_keys);
        if(terrain_albedo.is_valid())
        {
            material_instance->set_color_map(terrain_albedo);
            APPLOG_INFO("load_login terrain albedo selected: generated debug fallback");
        }
    }

    model terrain_model;
    terrain_model.set_lod(terrain_handle, 0);
    if(auto terrain = terrain_handle.get())
    {
        const auto submeshes = terrain->get_submeshes_count(0);
        for(uint32_t i = 0; i < submeshes; ++i)
        {
            terrain_model.set_material_instance(material_instance, i);
        }
    }

    const auto map_title = map_title_from_slug(map_slug);
    auto& scn = ctx.get_cached<ecs>().get_scene();
    // Heightfield meshes are centered locally. Region exports place that center in their absolute world rect;
    // legacy maps remain centered on the scene origin.
    const math::vec3 terrain_position = terrain.has_world_origin
                                            ? math::vec3{terrain.world_left + terrain.world_width * 0.5f,
                                                         terrain.heightfield_entity_y(),
                                                         terrain.world_top - terrain.world_depth * 0.5f}
                                            : math::vec3{0.0f, terrain.heightfield_entity_y(), 0.0f};
    auto entity = scene::create_entity(*scn.registry, map_title + " Terrain");
    created_entities.emplace_back(entity);
    entity.get<transform_component>().set_position_local(terrain_position);
    entity.emplace<model_component>().set_model(terrain_model);

    model terrain_skirt_model;
    terrain_skirt_model.set_lod(terrain_skirt_handle, 0);
    if(auto terrain_skirt = terrain_skirt_handle.get())
    {
        const auto submeshes = terrain_skirt->get_submeshes_count(0);
        for(uint32_t i = 0; i < submeshes; ++i)
        {
            terrain_skirt_model.set_material_instance(material_instance, i);
        }
    }

    auto skirt_entity = scene::create_entity(*scn.registry, map_title + " Terrain Skirt");
    created_entities.emplace_back(skirt_entity);
    skirt_entity.get<transform_component>().set_position_local(terrain_position);
    skirt_entity.emplace<model_component>().set_model(terrain_skirt_model);
}

auto color_from_argb(uint32_t argb) -> math::color
{
    const float r = static_cast<float>((argb >> 16u) & 0xffu) / 255.0f;
    const float g = static_cast<float>((argb >> 8u) & 0xffu) / 255.0f;
    const float b = static_cast<float>(argb & 0xffu) / 255.0f;
    return {r, g, b, 1.0f};
}

auto login_water_color_from_argb(uint32_t argb) -> math::color
{
    const float r = static_cast<float>((argb >> 16u) & 0xffu) / 255.0f;
    const float g = static_cast<float>((argb >> 8u) & 0xffu) / 255.0f;
    const float b = static_cast<float>(argb & 0xffu) / 255.0f;
    return {
        std::clamp(r * 0.35f + 0.08f * 0.65f, 0.0f, 1.0f),
        std::clamp(g * 0.45f + 0.22f * 0.55f, 0.0f, 1.0f),
        std::clamp(b * 0.45f + 0.26f * 0.55f, 0.0f, 1.0f),
        1.0f};
}

auto build_login_water_mesh_data(const json& water_payload, mesh::load_data& data) -> bool
{
    if(!water_payload.contains("waterSurface") || !water_payload["waterSurface"].is_object())
    {
        return false;
    }

    const auto& water_surface = water_payload["waterSurface"];
    if(!water_surface.contains("mesh") || !water_surface["mesh"].is_object())
    {
        return false;
    }

    const auto& mesh_doc = water_surface["mesh"];
    if(!mesh_doc.contains("vertices") || !mesh_doc["vertices"].is_array() ||
       !mesh_doc.contains("indices") || !mesh_doc["indices"].is_array())
    {
        return false;
    }

    const auto& vertices = mesh_doc["vertices"];
    const auto& indices = mesh_doc["indices"];
    if(vertices.empty() || indices.size() < 3u || (indices.size() % 3u) != 0u)
    {
        return false;
    }

    data = mesh::load_data{};
    data.vertex_format = gfx::mesh_vertex::get_layout();
    data.vertex_count = static_cast<uint32_t>(vertices.size());
    data.triangle_count = static_cast<uint32_t>(indices.size() / 3u);
    data.vertex_data.resize(static_cast<size_t>(data.vertex_count) * data.vertex_format.getStride());
    data.triangle_data.resize(data.triangle_count);
    data.material_count = 1;

    math::bbox bounds;
    for(size_t i = 0; i < vertices.size(); ++i)
    {
        const auto& vertex = vertices[i];
        if(!vertex.is_array() || vertex.size() < 3u)
        {
            return false;
        }

        const float position[4] = {
            vertex[0].get<float>(),
            vertex[1].get<float>(),
            vertex[2].get<float>(),
            0.0f};
        const float normal[4] = {0.0f, 1.0f, 0.0f, 0.0f};
        const float tangent[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        const float bitangent[4] = {0.0f, 0.0f, -1.0f, 0.0f};
        const float texcoord[4] = {(position[0] + 512.0f) / 1024.0f, (position[2] + 512.0f) / 1024.0f, 0.0f, 0.0f};

        gfx::vertex_pack(position, false, gfx::attribute::Position, data.vertex_format, data.vertex_data.data(), static_cast<uint32_t>(i));
        gfx::vertex_pack(normal, true, gfx::attribute::Normal, data.vertex_format, data.vertex_data.data(), static_cast<uint32_t>(i));
        gfx::vertex_pack(tangent, true, gfx::attribute::Tangent, data.vertex_format, data.vertex_data.data(), static_cast<uint32_t>(i));
        gfx::vertex_pack(bitangent, true, gfx::attribute::Bitangent, data.vertex_format, data.vertex_data.data(), static_cast<uint32_t>(i));
        gfx::vertex_pack(texcoord, true, gfx::attribute::TexCoord0, data.vertex_format, data.vertex_data.data(), static_cast<uint32_t>(i));

        bounds.add_point({position[0], position[1], position[2]});
    }

    for(size_t i = 0, tri_index = 0; i + 2u < indices.size(); i += 3u, ++tri_index)
    {
        const auto i0 = indices[i + 0u].get<uint32_t>();
        const auto i1 = indices[i + 1u].get<uint32_t>();
        const auto i2 = indices[i + 2u].get<uint32_t>();
        if(i0 >= data.vertex_count || i1 >= data.vertex_count || i2 >= data.vertex_count)
        {
            return false;
        }

        auto& tri = data.triangle_data[tri_index];
        tri.data_group_id = 0;
        tri.indices = {i0, i1, i2};
    }

    mesh::submesh water_submesh;
    water_submesh.data_group_id = 0;
    water_submesh.vertex_start = 0;
    water_submesh.vertex_count = data.vertex_count;
    water_submesh.face_start = 0;
    water_submesh.face_count = data.triangle_count;
    water_submesh.bbox = bounds;
    data.submeshes.emplace_back(water_submesh);
    data.bbox = bounds;
    return true;
}

auto create_login_water_surface(rtti::context& ctx,
                                const std::string& content_root,
                                const std::string& map_slug,
                                uint64_t generation,
                                const mcp_system::login_water& water,
                                uint32_t index,
                                std::vector<entt::handle>& created_entities,
                                std::vector<std::string>& generated_mesh_keys) -> bool
{
    const auto payload_doc = read_json_asset(login_scene_payload_asset_key(content_root, map_slug, water.payload));
    mesh::load_data data;
    if(!build_login_water_mesh_data(payload_doc, data))
    {
        return false;
    }

    auto water_mesh = std::make_shared<mesh>();
    if(!water_mesh->load_mesh(std::move(data)))
    {
        return false;
    }

    auto& am = ctx.get_cached<asset_manager>();
    const auto water_key =
        make_generated_map_asset_key(map_slug, generation, "water_" + std::to_string(index));
    generated_mesh_keys.emplace_back(water_key);
    auto water_handle = am.get_asset_from_instance<mesh>(water_key, water_mesh);

    uint32_t source_argb = 0xff1f5d72u;
    if(payload_doc.contains("waterSurface") && payload_doc["waterSurface"].is_object())
    {
        const auto& water_surface = payload_doc["waterSurface"];
        if(water_surface.contains("area") && water_surface["area"].is_object())
        {
            source_argb = water_surface["area"].value("sourceColorArgb", source_argb);
        }
    }

    auto material_instance = std::make_shared<pbr_material>();
    const auto source_color = login_water_color_from_argb(source_argb);
    const float source_luminance = source_color.value.r * 0.2126f +
                                   source_color.value.g * 0.7152f +
                                   source_color.value.b * 0.0722f;
    const math::color water_base_color{
        std::clamp(source_luminance * 0.08f + source_color.value.r * 0.10f + 0.015f, 0.0f, 0.07f),
        std::clamp(source_luminance * 0.10f + source_color.value.g * 0.16f + 0.055f, 0.0f, 0.16f),
        std::clamp(source_luminance * 0.10f + source_color.value.b * 0.15f + 0.070f, 0.0f, 0.18f),
        1.0f};
    material_instance->set_base_color(water_base_color);
    material_instance->set_metalness(0.04f);
    material_instance->set_roughness(0.08f);
    material_instance->set_cull_type(cull_type::none);

    model water_model;
    water_model.set_lod(water_handle, 0);
    water_model.set_material_instance(material_instance, 0);

    auto& scn = ctx.get_cached<ecs>().get_scene();
    auto entity = scene::create_entity(*scn.registry, water.name.empty() ? std::string("Water ") + std::to_string(index) : water.name);
    created_entities.emplace_back(entity);
    auto& model_comp = entity.emplace<model_component>();
    model_comp.set_model(water_model);
    model_comp.set_casts_shadow(false);

    APPLOG_INFO("load_login water created: name='{}' payload='{}' vertices={} triangles={} visible_cells={}",
                water.name,
                water.payload,
                water_mesh->get_vertex_count(),
                water_mesh->get_face_count(),
                water.visible_cells);
    return true;
}

auto find_scene_entity_named(scene& scn, const std::string& name) -> entt::handle
{
    auto view = scn.registry->view<tag_component>();
    for(auto e : view)
    {
        if(view.get<tag_component>(e).name == name)
        {
            return scn.create_handle(e);
        }
    }

    return {};
}

struct login_effect_note_params
{
    float scale = 1.0f;
    float play_speed = 1.0f;
    float alpha = 1.0f;
};

struct login_effect_element
{
    std::string name;
    std::string type_name;
    std::string texture_ref;
};

auto read_login_effect_note_float(const std::string& note, const char* key, float fallback) -> float
{
    const std::string marker = std::string(key) + "=";
    const auto marker_pos = note.find(marker);
    if(marker_pos == std::string::npos)
    {
        return fallback;
    }

    const char* begin = note.c_str() + marker_pos + marker.size();
    char* end = nullptr;
    const float value = std::strtof(begin, &end);
    if(end == begin || !std::isfinite(value))
    {
        return fallback;
    }

    return value;
}

auto read_login_effect_note_params(const json& item) -> login_effect_note_params
{
    login_effect_note_params params;
    const auto note = item.value("note", std::string{});
    params.scale = read_login_effect_note_float(note, "scale", params.scale);
    params.play_speed = read_login_effect_note_float(note, "speed", params.play_speed);
    params.alpha = read_login_effect_note_float(note, "alpha", params.alpha);
    return params;
}

void add_login_effect_ref_candidate(std::vector<std::string>& candidates, std::string ref)
{
    std::replace(ref.begin(), ref.end(), '\\', '/');
    while(!ref.empty() && ref.front() == '/')
    {
        ref.erase(ref.begin());
    }
    if(ref.empty() || std::find(candidates.begin(), candidates.end(), ref) != candidates.end())
    {
        return;
    }

    candidates.emplace_back(std::move(ref));
}

auto login_effect_ref_candidates(const json& item) -> std::vector<std::string>
{
    std::vector<std::string> candidates;

    if(item.contains("openFormat") && item["openFormat"].is_object())
    {
        const auto& open_format = item["openFormat"];
        if(open_format.contains("effectRef") && open_format["effectRef"].is_object())
        {
            add_login_effect_ref_candidate(candidates, open_format["effectRef"].value("effect", std::string{}));
        }
    }

    const auto payload_ref = item.value("payload", std::string{});
    add_login_effect_ref_candidate(candidates, payload_ref);

    std::string normalized_payload = payload_ref;
    std::replace(normalized_payload.begin(), normalized_payload.end(), '\\', '/');
    if(normalized_payload.rfind("effects/", 0) == 0)
    {
        add_login_effect_ref_candidate(candidates, "fx/" + normalized_payload.substr(std::string("effects/").size()));
    }

    return candidates;
}

auto make_login_relative_asset_key(const std::string& content_root, const std::string& ref) -> std::string
{
    if(fs::has_known_protocol(ref))
    {
        return ref;
    }

    return make_asset_key(content_root, ref);
}

auto read_login_effect_doc(const std::string& content_root, const json& item, std::string& selected_ref) -> json
{
    const auto candidates = login_effect_ref_candidates(item);
    std::string last_error;

    for(const auto& candidate : candidates)
    {
        try
        {
            const auto asset_key = make_login_relative_asset_key(content_root, candidate);
            auto doc = read_json_asset(asset_key);
            selected_ref = candidate;
            return doc;
        }
        catch(const std::exception& e)
        {
            last_error = e.what();
        }
    }

    throw std::runtime_error("load_login: no readable effect payload for '" +
                             item.value("name", std::string("<unnamed>")) + "'; last_error='" + last_error + "'");
}

auto find_login_effect_texture_ref(const json& effect_doc, const std::string& element_name) -> std::string
{
    std::string first_texture_ref;
    if(!effect_doc.contains("dependencies") || !effect_doc["dependencies"].is_array())
    {
        return {};
    }

    for(const auto& dependency : effect_doc["dependencies"])
    {
        if(!dependency.is_object() || dependency.value("kind", std::string{}) != "texture")
        {
            continue;
        }

        const auto texture_ref = dependency.value("textureRef", std::string{});
        if(texture_ref.empty())
        {
            continue;
        }

        if(first_texture_ref.empty())
        {
            first_texture_ref = texture_ref;
        }

        if(!element_name.empty() && dependency.value("elementName", std::string{}) == element_name)
        {
            return texture_ref;
        }
    }

    return first_texture_ref;
}

auto read_login_effect_elements(const json& effect_doc) -> std::vector<login_effect_element>
{
    std::vector<login_effect_element> elements;

    if(effect_doc.contains("elements") && effect_doc["elements"].is_array())
    {
        for(const auto& element_doc : effect_doc["elements"])
        {
            if(!element_doc.is_object())
            {
                continue;
            }

            login_effect_element element;
            element.name = element_doc.value("name", std::string{});
            element.type_name = element_doc.value("typeName", std::string{});
            element.texture_ref = find_login_effect_texture_ref(effect_doc, element.name);
            if(!element.texture_ref.empty())
            {
                elements.emplace_back(std::move(element));
            }
        }
    }

    if(elements.empty() && effect_doc.contains("dependencies") && effect_doc["dependencies"].is_array())
    {
        for(const auto& dependency : effect_doc["dependencies"])
        {
            if(!dependency.is_object() || dependency.value("kind", std::string{}) != "texture")
            {
                continue;
            }

            login_effect_element element;
            element.name = dependency.value("elementName", std::string{});
            element.type_name = "texture";
            element.texture_ref = dependency.value("textureRef", std::string{});
            if(!element.texture_ref.empty())
            {
                elements.emplace_back(std::move(element));
            }
        }
    }

    return elements;
}

auto login_effect_shape_for_type(const std::string& type_name) -> ps_soa::emitter_shape
{
    if(type_name == "particleBox")
    {
        return ps_soa::emitter_shape::box;
    }
    if(type_name == "decal3d" || type_name == "decalBillboard")
    {
        return ps_soa::emitter_shape::rect;
    }

    return ps_soa::emitter_shape::sphere;
}

auto login_effect_direction_for_type(const std::string& type_name) -> ps_soa::emitter_direction
{
    if(type_name == "particlePoint" || type_name == "particleBox" || type_name == "particleEllipsoid")
    {
        return ps_soa::emitter_direction::outward;
    }

    return ps_soa::emitter_direction::up;
}

auto login_effect_sprite_scale_for_type(const std::string& type_name) -> float
{
    // Tuned down toward the D9 reference: the prior multipliers (2.0-4.0) produced oversized
    // billboards that, with additive blending, washed out the scene (the SELCHAR view especially).
    if(type_name == "decal3d" || type_name == "decalBillboard")
    {
        return 2.0f;
    }
    if(type_name == "particleBox")
    {
        return 1.1f;
    }
    if(type_name == "particlePoint")
    {
        return 1.3f;
    }

    return 1.3f;
}

auto login_effect_extent_scale_for_type(const std::string& type_name) -> float
{
    if(type_name == "decal3d" || type_name == "decalBillboard")
    {
        return 0.35f;
    }
    if(type_name == "particleBox")
    {
        return 1.75f;
    }

    return 1.0f;
}

auto is_login_effect_type_mapped(const std::string& type_name) -> bool
{
    return type_name == "particleBox" || type_name == "particleEllipsoid" || type_name == "particlePoint" ||
           type_name == "decal3d" || type_name == "decalBillboard" || type_name == "texture";
}

void configure_login_effect_emitter(particle_emitter_component& emitter,
                                    const login_effect_element& element,
                                    const asset_handle<gfx::texture>& texture_handle,
                                    float scale,
                                    float alpha,
                                    float play_speed)
{
    const float safe_scale = std::max(scale, 0.05f);
    const float safe_speed = std::max(play_speed, 0.05f);
    const float sprite_size =
        std::clamp(safe_scale * login_effect_sprite_scale_for_type(element.type_name), 0.2f, 10.0f);
    const float emitter_extent =
        std::clamp(safe_scale * login_effect_extent_scale_for_type(element.type_name), 0.1f, 24.0f);

    const float lifetime = std::clamp(1.4f / safe_speed, 0.25f, 6.0f);
    const float emission_rate = std::clamp(18.0f * safe_speed, 4.0f, 60.0f);
    const auto max_particles = static_cast<uint32_t>(
        std::clamp(static_cast<float>(std::ceil(emission_rate * lifetime * 2.0f)), 16.0f, 110.0f));

    emitter.set_max_particles(max_particles);
    emitter.set_shape(login_effect_shape_for_type(element.type_name));
    emitter.set_direction(login_effect_direction_for_type(element.type_name));
    emitter.set_spawn_location(ps_soa::spawn_location::inside);
    emitter.set_simulation_space(ps_soa::simulation_space::world);
    emitter.set_texture(texture_handle);
    emitter.set_texture_mode(ps_soa::texture_mode::multi_channel);
    emitter.set_render_mode(ps_soa::render_mode::billboard);
    emitter.set_blend_mode(ps_soa::blend_mode::additive);
    emitter.set_loop(true);
    emitter.set_lifetime(std::chrono::duration<float>(lifetime));
    emitter.set_emission_lifetime(std::chrono::duration<float>(std::clamp(2.0f / safe_speed, 0.3f, 8.0f)));
    emitter.set_emission_rate(emission_rate);
    emitter.set_opacity(std::clamp(alpha, 0.0f, 0.7f));
    emitter.set_color_intensity(0.85f);
    emitter.set_gravity_scale(0.0f);
    emitter.set_velocity_damping(0.25f);
    emitter.set_temporal_motion(1.0f);
    emitter.set_emission_shape_scale(math::vec3(emitter_extent));
    emitter.set_initial_scale_3d(math::vec3(1.0f));

    math::gradient<frange_t> scale_gradient;
    scale_gradient.add_point(frange_t(sprite_size * 0.35f, sprite_size * 0.75f), 0.0f);
    scale_gradient.add_point(frange_t(sprite_size * 0.85f, sprite_size * 1.35f), 1.0f);
    emitter.set_scale_gradient(scale_gradient);

    math::gradient<math::color> color_gradient;
    color_gradient.add_point(math::color(1.0f, 1.0f, 1.0f, 0.0f), 0.0f);
    color_gradient.add_point(math::color(1.0f, 1.0f, 1.0f, 1.0f), 0.15f);
    color_gradient.add_point(math::color(1.0f, 1.0f, 1.0f, 0.85f), 0.72f);
    color_gradient.add_point(math::color(1.0f, 1.0f, 1.0f, 0.0f), 1.0f);
    emitter.set_color_gradient(color_gradient);

    math::gradient<frange_t> velocity_gradient;
    velocity_gradient.add_point(frange_t(0.15f * safe_scale, 0.7f * safe_scale), 0.0f);
    velocity_gradient.add_point(frange_t(0.35f * safe_scale, 1.8f * safe_scale), 1.0f);
    emitter.set_velocity_gradient(velocity_gradient);
    emitter.play();
}

void create_login_effects(rtti::context& ctx,
                          const std::string& content_root,
                          const std::string& map_slug,
                          std::vector<entt::handle>& created_entities)
{
    const auto scene_doc = read_json_asset(make_asset_key(content_root, "maps/" + map_slug + "/scene.eds.json"));
    if(!scene_doc.contains("nodes") || !scene_doc["nodes"].is_array())
    {
        APPLOG_WARNING("load_login: scene.eds.json has no nodes[]; effects skipped");
        return;
    }

    auto& am = ctx.get_cached<asset_manager>();
    auto& scn = ctx.get_cached<ecs>().get_scene();

    size_t effect_index = 0;
    size_t created = 0;
    size_t skipped = 0;
    size_t unmapped = 0;

    for(const auto& item : scene_doc["nodes"])
    {
        if(!item.is_object() || item.value("kind", std::string{}) != "Effect")
        {
            continue;
        }

        const size_t authored_effect_index = effect_index++;

        try
        {
            if(item.contains("openFormat") && item["openFormat"].is_object() &&
               !item["openFormat"].value("convertedToOpenFormat", false))
            {
                ++skipped;
                APPLOG_WARNING("load_login effect skipped: name='{}' reason='not converted to open format'",
                               item.value("name", std::string("<unnamed>")));
                continue;
            }

            std::string effect_ref;
            const auto effect_doc = read_login_effect_doc(content_root, item, effect_ref);
            auto elements = read_login_effect_elements(effect_doc);
            if(elements.empty())
            {
                ++skipped;
                APPLOG_WARNING("load_login effect skipped: name='{}' ref='{}' reason='no texture dependencies'",
                               item.value("name", std::string("<unnamed>")),
                               effect_ref);
                continue;
            }

            const auto note_params = read_login_effect_note_params(item);
            const float scale = effect_doc.value("defaultScale", 1.0f) * note_params.scale;
            const float alpha = effect_doc.value("defaultAlpha", 1.0f) * note_params.alpha;
            const float play_speed = effect_doc.value("defaultPlaySpeed", 1.0f) * note_params.play_speed;
            const auto position = map_source_position_to_unravel(read_vec3_member(item, "pos"));

            size_t element_index = 0;
            size_t created_for_effect = 0;
            for(const auto& element : elements)
            {
                if(!is_login_effect_type_mapped(element.type_name))
                {
                    ++unmapped;
                    APPLOG_WARNING("load_login effect element mapped as generic billboard: effect='{}' element='{}' "
                                   "type='{}'",
                                   effect_ref,
                                   element.name,
                                   element.type_name);
                }

                const auto texture_key = make_login_relative_asset_key(content_root, element.texture_ref);
                auto texture_handle = am.get_asset<gfx::texture>(texture_key, load_flags::standard);
                texture_handle.submit();
                if(!texture_handle.is_valid())
                {
                    ++unmapped;
                    APPLOG_WARNING("load_login effect element skipped: effect='{}' element='{}' texture='{}' "
                                   "reason='texture asset handle invalid'",
                                   effect_ref,
                                   element.name,
                                   texture_key);
                    ++element_index;
                    continue;
                }

                std::string entity_name = "Login Effect " + std::to_string(authored_effect_index);
                if(created_for_effect > 0)
                {
                    entity_name += " Element " + std::to_string(element_index);
                }

                auto entity = scene::create_entity(*scn.registry, entity_name);
                created_entities.emplace_back(entity);
                auto& transform = entity.get<transform_component>();
                transform.set_position_local(position);
                if(item.contains("dir") && item.contains("up"))
                {
                    const auto forward = map_source_forward_to_unravel(read_vec3_member(item, "dir"));
                    const auto up = map_source_up_to_unravel(read_vec3_member(item, "up"));
                    transform.look_at(position + forward, up);
                }

                auto& emitter = entity.emplace<particle_emitter_component>();
                configure_login_effect_emitter(emitter, element, texture_handle, scale, alpha, play_speed);

                ++created;
                ++created_for_effect;
                ++element_index;
            }

            if(created_for_effect == 0)
            {
                ++skipped;
                APPLOG_WARNING("load_login effect skipped: name='{}' ref='{}' reason='no mappable texture elements'",
                               item.value("name", std::string("<unnamed>")),
                               effect_ref);
            }
        }
        catch(const std::exception& e)
        {
            ++skipped;
            APPLOG_WARNING("load_login effect skipped: name='{}' reason='{}'",
                           item.value("name", std::string("<unnamed>")),
                           e.what());
        }
    }

    APPLOG_INFO("load_login effects created: nodes={} emitters={} skipped={} generic_mapped={}",
                effect_index,
                created,
                skipped,
                unmapped);
}

auto login_character_position() -> math::vec3
{
    return {kLoginCharacterX, kLoginCharacterFallbackY, kLoginCharacterZ};
}

auto login_character_camera_position() -> math::vec3
{
    // scenectrl.ini [Camera] idx14 Pos (CREATE prof0), LH (no flip), + s_camPosDelta[0][0]=(0,0.2,0).
    return {190.303848f, 229.390994f, 284.287781f};
}

auto login_character_camera_target() -> math::vec3
{
    // camera pos + dir(0.610395,0.130526,0.781269)*10 (LH, directly from scenectrl.ini).
    return {196.407806f, 230.696259f, 292.100464f};
}

auto login_character_camera_position(const login_scene_config& config) -> math::vec3
{
    if(config.loaded && config.cameras[kLoginSceneCreateIndex].valid)
    {
        auto position = config.cameras[kLoginSceneCreateIndex].pos;
        position.y += 0.2f;
        return position;
    }
    return login_character_camera_position();
}

auto login_character_camera_target(const login_scene_config& config) -> math::vec3
{
    if(config.loaded && config.cameras[kLoginSceneCreateIndex].valid)
    {
        const auto position = login_character_camera_position(config);
        const auto direction = config.cameras[kLoginSceneCreateIndex].dir;
        return {position.x + direction.x, position.y + direction.y, position.z + direction.z};
    }
    return login_character_camera_target();
}

void apply_login_character_camera_pose(entt::handle camera, const login_scene_config& config)
{
    if(!camera)
    {
        return;
    }
    auto& transform = camera.get<transform_component>();
    const auto position = login_character_camera_position(config);
    const auto target = login_character_camera_target(config);
    transform.set_position_global(position);
    transform.look_at(target, {0.0f, 1.0f, 0.0f});
    auto& camera_comp = camera.get<camera_component>();
    camera_comp.set_fov(kLoginCharacterCameraFov);
    camera_comp.set_near_clip(kLoginCharacterCameraNear);
    camera_comp.set_far_clip(kLoginCharacterCameraFar);
}

auto create_login_character(rtti::context& ctx, const std::string& content_root, std::vector<entt::handle>& created_entities)
    -> bool
{
    auto& scn = ctx.get_cached<ecs>().get_scene();
    if(find_scene_entity_named(scn, kLoginCharacterEntityName))
    {
        return true;
    }
    auto& am = ctx.get_cached<asset_manager>();
    const auto flags = load_flags::standard;
    auto mesh_handle = am.get_asset<mesh>(make_asset_key(content_root, kLoginCharacterMeshRef), flags);
    auto idle_clip_handle = am.get_asset<animation_clip>(make_asset_key(content_root, kLoginCharacterIdleClipRef), flags);
    mesh_handle.submit();
    idle_clip_handle.submit();
    if(!mesh_handle.is_ready() || !idle_clip_handle.is_ready())
    {
        return false;
    }
    auto mesh_instance = mesh_handle.get(false);
    auto idle_clip = idle_clip_handle.get(false);
    if(!mesh_instance || mesh_instance->get_submeshes_count(0) == 0 || !idle_clip)
    {
        return false;
    }
    model character_model;
    character_model.set_lod(mesh_handle, 0);
    const auto& material_uids = mesh_instance->get_default_material_uids();
    for(size_t i = 0; i < material_uids.size(); ++i)
    {
        auto material_handle = am.get_asset<material>(material_uids[i], flags);
        material_handle.submit();
        // Force-load the material asset and promote it to a concrete instance. The deferred
        // renderer SKIPS any data-group whose get_material_instance() returns null (model.cpp
        // get_material_instance -> if asset handle not resolved -> nullptr -> submesh skipped),
        // which left the freshly-imported armor sub-skins invisible. Buildings/foliage already
        // use this set_material_instance path; do the same for the dressed character.
        auto material_instance = material_handle.get(true);
        if(material_handle)
        {
            character_model.set_material(material_handle, static_cast<uint32_t>(i));
        }
        if(material_instance)
        {
            // Armor sub-skins (added via AddSkin in the exporter) can carry opposite winding and
            // an alpha channel that is a spec mask (not transparency). Force opaque + two-sided so
            // the deferred pass neither back-face-culls nor alpha-discards them. Buildings already
            // render with cull_type::none; do the same here.
            if(auto pbr = std::dynamic_pointer_cast<pbr_material>(material_instance))
            {
                pbr->set_cull_type(cull_type::none);
                pbr->set_alpha_blend(false);
                pbr->set_alpha_mode(alpha_mode::opaque);
                // Force-load the color map so the (freshly imported) armor texture is resident at render
                // instead of sampling a white default. Buildings/foliage submit their textures the same way.
                auto color_map = pbr->get_color_map();
                color_map.submit();
            }
            character_model.set_material_instance(material_instance, static_cast<uint32_t>(i));
        }
        APPLOG_INFO("load_login character material[{}]: valid={} ready={} instance={}",
                    i,
                    material_handle.is_valid(),
                    material_handle.is_ready(),
                    static_cast<bool>(material_instance));
    }
    const auto scene_config = parse_login_scene_config(content_root);
    auto position = login_character_position();
    if(scene_config.loaded && !scene_config.new_char_positions.empty())
    {
        position.x = scene_config.new_char_positions[0].x;
        position.z = scene_config.new_char_positions[0].z;
    }
    terrain_heightfield terrain;
    try
    {
        terrain = load_login_terrain_heightfield(content_root, "login");
    }
    catch(const std::exception& e)
    {
        APPLOG_WARNING("load_login character terrain sample fallback: reason='{}'", e.what());
    }
    float terrain_surface_y = kLoginCharacterFallbackY;
    const bool terrain_sample_valid =
        terrain.is_valid() && terrain.sample_terrain_height(position.x, position.z, terrain_surface_y);
    const auto& local_bounds = mesh_instance->get_bounds();
    const float local_min_y =
        local_bounds.is_populated() && std::isfinite(local_bounds.min.y) ? local_bounds.min.y : 0.0f;
    position.y = terrain_surface_y - local_min_y + kLoginCharacterGroundOffset;
    auto entity = scene::create_entity(*scn.registry, kLoginCharacterEntityName);
    created_entities.emplace_back(entity);
    auto& transform = entity.get<transform_component>();
    transform.set_position_local(position);
    // Face the char-select camera (horizontal), matching the client create/select pose.
    const auto camera_pos = login_character_camera_position(scene_config);
    transform.look_at(math::vec3{camera_pos.x, position.y, camera_pos.z}, {0.0f, 1.0f, 0.0f});
    auto& model_comp = entity.emplace<model_component>();
    model_comp.set_model(character_model);
    model_comp.init_armature(false);
    auto& animation_comp = entity.emplace<animation_component>();
    animation_comp.set_animation(idle_clip_handle);
    animation_comp.set_autoplay(true);
    animation_comp.set_apply_root_motion(false);
    auto& player = animation_comp.get_player();
    player.blend_to(0, idle_clip_handle, animation_player::seconds_t(0.0f), true);
    player.play();
    APPLOG_INFO("load_login character created: entity='{}' mesh='{}' idle='{}' x={} y={} z={} terrain_surface_y={} "
                "sample_valid={} local_min_y={}",
                kLoginCharacterEntityName,
                kLoginCharacterMeshRef,
                kLoginCharacterIdleClipRef,
                position.x,
                position.y,
                position.z,
                terrain_surface_y,
                terrain_sample_valid,
                local_min_y);
    return true;
}

auto is_map_owned_entity(const std::vector<entt::handle>& created_entities, entt::handle entity) -> bool
{
    return std::any_of(created_entities.begin(), created_entities.end(), [entity](const entt::handle& candidate)
    {
        return candidate == entity;
    });
}

void create_login_lights(rtti::context& ctx,
                         const std::string& map_slug,
                         const std::vector<mcp_system::login_light>& lights,
                         std::vector<entt::handle>& created_entities,
                         std::vector<std::function<void()>>& shared_entity_rollbacks)
{
    auto& scn = ctx.get_cached<ecs>().get_scene();
    size_t directional_count = 0;
    size_t point_count = 0;

    for(const auto& light_item : lights)
    {
        if(light_item.type == mcp_system::login_light::kind::directional)
        {
            auto sun = find_scene_entity_named(scn, "Sun Light");
            if(!sun)
            {
                sun = defaults::create_light_entity(ctx, scn, light_type::directional, "Sun");
                created_entities.emplace_back(sun);
            }

            if(!is_map_owned_entity(created_entities, sun))
            {
                const bool had_transform = sun.all_of<transform_component>();
                const auto previous_transform = had_transform
                                                    ? sun.get<transform_component>().get_transform_global()
                                                    : math::transform{};
                const bool had_light = sun.all_of<light_component>();
                const auto previous_light = had_light ? sun.get<light_component>().get_light() : light{};
                shared_entity_rollbacks.emplace_back([sun, had_transform, previous_transform]()
                {
                    if(!sun.valid())
                    {
                        return;
                    }
                    if(had_transform)
                    {
                        sun.get_or_emplace<transform_component>().set_transform_global(previous_transform);
                    }
                    else if(sun.all_of<transform_component>())
                    {
                        sun.remove<transform_component>();
                    }
                });
                shared_entity_rollbacks.emplace_back([sun, had_light, previous_light]()
                {
                    if(!sun.valid())
                    {
                        return;
                    }
                    if(had_light)
                    {
                        sun.get_or_emplace<light_component>().set_light(previous_light);
                    }
                    else if(sun.all_of<light_component>())
                    {
                        sun.remove<light_component>();
                    }
                });
            }

            auto& transform = sun.get_or_emplace<transform_component>();
            transform.set_rotation_global(
                math::from_to_rotation(math::vec3{0.0f, 0.0f, 1.0f}, light_item.direction));

            auto& light_comp = sun.get_or_emplace<light_component>();
            auto light_data = light_comp.get_light();
            light_data.type = light_type::directional;
            light_data.color = light_item.color;
            // PW authored sun intensity (~1.0) is far below this engine's photometric scale (engine default ~5.0).
            light_data.intensity =
                (light_item.has_intensity ? light_item.intensity : light_data.intensity) * 4.5f;
            light_comp.set_light(light_data);

            ++directional_count;
            continue;
        }

        if(light_item.type == mcp_system::login_light::kind::point)
        {
            auto point_entity = scene::create_entity(*scn.registry,
                                                     map_title_from_slug(map_slug) + " Light " +
                                                         std::to_string(light_item.source_index));
            created_entities.emplace_back(point_entity);
            auto& transform = point_entity.get<transform_component>();
            transform.set_position_local(light_item.position);

            light light_data;
            light_data.type = light_type::point;
            light_data.color = light_item.color;
            if(light_item.has_intensity)
            {
                light_data.intensity = light_item.intensity;
            }
            if(light_item.has_range)
            {
                light_data.point_data.range = light_item.range;
            }
            light_data.casts_shadows = false;

            point_entity.emplace<light_component>().set_light(light_data);

            ++point_count;
        }
    }

    APPLOG_INFO("load_login lights created: directional={} points={}", directional_count, point_count);
}

void create_login_environment(rtti::context& ctx,
                              std::vector<entt::handle>& created_entities,
                              std::vector<std::function<void()>>& shared_entity_rollbacks)
{
    auto& scn = ctx.get_cached<ecs>().get_scene();

    auto volume = find_scene_entity_named(scn, "Volume");
    if(!volume)
    {
        volume = defaults::create_volume_entity(ctx, scn, "Volume", volume_mode::global);
        created_entities.emplace_back(volume);
    }
    else
    {
        if(const auto* tonemapping = volume.try_get<tonemapping_component>())
        {
            const auto previous = *tonemapping;
            shared_entity_rollbacks.emplace_back([volume, previous]()
            {
                if(volume.valid() && volume.all_of<tonemapping_component>())
                {
                    volume.get<tonemapping_component>() = previous;
                }
            });
        }
        if(const auto* auto_exposure = volume.try_get<auto_exposure_component>())
        {
            const auto previous = *auto_exposure;
            shared_entity_rollbacks.emplace_back([volume, previous]()
            {
                if(volume.valid() && volume.all_of<auto_exposure_component>())
                {
                    volume.get<auto_exposure_component>() = previous;
                }
            });
        }
    }

    if(auto* tonemapping = volume.try_get<tonemapping_component>())
    {
        tonemapping->enabled = true;
        tonemapping->settings.method = tonemapping_method::aces;
        tonemapping->settings.exposure = 0.85f;
    }

    if(auto* auto_exposure = volume.try_get<auto_exposure_component>())
    {
        auto_exposure->enabled = false;
    }

    auto sun = find_scene_entity_named(scn, "Sun Light");
    if(!sun)
    {
        sun = defaults::create_light_entity(ctx, scn, light_type::directional, "Sun");
        created_entities.emplace_back(sun);
    }
    else
    {
        const bool had_skylight = sun.all_of<skylight_component>();
        const auto previous = had_skylight ? sun.get<skylight_component>() : skylight_component{};
        shared_entity_rollbacks.emplace_back([sun, had_skylight, previous]()
        {
            if(!sun.valid())
            {
                return;
            }
            if(had_skylight)
            {
                sun.get_or_emplace<skylight_component>() = previous;
            }
            else if(sun.all_of<skylight_component>())
            {
                sun.remove<skylight_component>();
            }
        });
    }

    if(sun)
    {
        auto& skylight = sun.get_or_emplace<skylight_component>();
        skylight.set_cloud_mode(skylight_component::cloud_mode::none);
        skylight.set_irradiance_intensity(0.35f);
    }

    auto reflection_probe = find_scene_entity_named(scn, "Reflection Probe Global");
    if(!reflection_probe)
    {
        reflection_probe = defaults::create_reflection_probe_entity(ctx, scn, probe_type::sphere, " Global");
        created_entities.emplace_back(reflection_probe);
        auto& reflection_comp = reflection_probe.get_or_emplace<reflection_probe_component>();
        auto probe = reflection_comp.get_probe();
        probe.method = reflect_method::environment;
        probe.sphere_data.range = 1600.0f;
        reflection_comp.set_probe(probe);
    }
}

auto create_login_building(rtti::context& ctx,
                           const std::string& content_root,
                           mcp_system::login_building& building,
                           bool allow_untextured,
                           std::vector<entt::handle>& created_entities) -> bool
{
    auto& am = ctx.get_cached<asset_manager>();
    const auto mesh_key = make_asset_key(content_root, building.model);
    const auto flags = load_flags::standard;

    auto mesh_handle = am.get_asset<mesh>(mesh_key, flags);
    mesh_handle.submit();
    if(!mesh_handle.is_ready())
    {
        ++building.attempts;
        return false;
    }

    auto mesh_instance = mesh_handle.get(false);
    if(!mesh_instance || mesh_instance->get_submeshes_count(0) == 0)
    {
        ++building.attempts;
        return false;
    }

    const size_t texture_slot_count = std::max<size_t>(building.textures.empty() ? 0u : building.textures.size(), 1u);
    std::vector<asset_handle<gfx::texture>> texture_handles(texture_slot_count);
    std::vector<bool> texture_usable(texture_slot_count, false);
    uint32_t usable_count = 0;

    for(size_t i = 0; i < texture_slot_count; ++i)
    {
        const std::string& texture_ref =
            (!building.textures.empty() && i < building.textures.size() && !building.textures[i].empty())
                ? building.textures[i]
                : building.texture;
        if(texture_ref.empty())
        {
            continue;
        }

        const auto texture_key = make_asset_key(content_root, texture_ref);
        auto texture_handle = am.get_asset<gfx::texture>(texture_key, flags);
        texture_handle.submit();
        texture_handles[i] = texture_handle;

        auto texture_instance = texture_handle.is_ready() ? texture_handle.get(false) : std::shared_ptr<gfx::texture>{};
        const bool native_valid = texture_instance && texture_instance->is_valid();
        const auto width = texture_instance ? texture_instance->info.width : 0;
        const auto height = texture_instance ? texture_instance->info.height : 0;
        texture_usable[i] = native_valid && width > 0 && height > 0;
        if(texture_usable[i])
        {
            ++usable_count;
        }
    }

    if(!building.texture_request_logged)
    {
        APPLOG_INFO("load_login texture requested: building='{}' slots={} first='{}'",
                    building.name,
                    texture_slot_count,
                    building.texture);
        building.texture_request_logged = true;
    }

    if(!building.texture_ready_logged && (usable_count == texture_slot_count || allow_untextured))
    {
        APPLOG_INFO("load_login texture resolved: building='{}' slots={} usable={} allow_untextured={}",
                    building.name,
                    texture_slot_count,
                    usable_count,
                    allow_untextured);
        building.texture_ready_logged = true;
    }

    if(!allow_untextured && usable_count < texture_slot_count)
    {
        ++building.attempts;
        return false;
    }

    std::vector<material::sptr> material_slots(texture_slot_count);
    const auto make_material = [&](size_t slot_index) -> material::sptr
    {
        if(slot_index >= material_slots.size())
        {
            slot_index = 0u;
        }
        if(material_slots[slot_index])
        {
            return material_slots[slot_index];
        }

        auto material_instance = std::make_shared<pbr_material>();
        material_instance->set_base_color({1.0f, 1.0f, 1.0f, 1.0f});
        material_instance->set_metalness(0.0f);
        material_instance->set_roughness(0.85f);
        material_instance->set_cull_type(cull_type::none);
        material_instance->set_alpha_blend(building.alpha_blend);
        if(building.alpha_blend)
        {
            material_instance->set_alpha_mode(alpha_mode::blend);
        }
        else if(building.alpha_test)
        {
            material_instance->set_alpha_mode(alpha_mode::mask);
            material_instance->set_alpha_cutoff(0.5f);
        }
        else
        {
            material_instance->set_alpha_mode(alpha_mode::opaque);
        }
        if(slot_index < texture_handles.size() && texture_usable[slot_index])
        {
            material_instance->set_color_map(texture_handles[slot_index]);
        }
        material_slots[slot_index] = material_instance;
        return material_instance;
    };

    model building_model;
    building_model.set_lod(mesh_handle, 0);

    const auto material_group_count = mesh_instance->get_data_groups_count();
    for(size_t i = 0; i < material_group_count; ++i)
    {
        size_t material_slot = i;
        if(material_slot >= texture_slot_count)
        {
            material_slot = 0u;
        }
        building_model.set_material_instance(make_material(material_slot), static_cast<uint32_t>(i));
    }

    auto& scn = ctx.get_cached<ecs>().get_scene();
    auto entity = scene::create_entity(*scn.registry, building.name);
    created_entities.emplace_back(entity);
    auto& transform = entity.get<transform_component>();
    auto position = building.position;
    const auto& local_bounds = mesh_instance->get_bounds();
    building.local_min_y = local_bounds.is_populated() && std::isfinite(local_bounds.min.y) ? local_bounds.min.y : 0.0f;
    position.y = building.source_position_y;
    const float local_min_world_y = position.y + building.local_min_y;

    if(!building.placement_logged)
    {
        APPLOG_INFO("load_login placement: building='{}' x={} z={} terrain_surface_y={} final_y={} source_y={} "
                    "local_min_y={} local_min_world_y={} sample_valid={}",
                    building.name,
                    position.x,
                    position.z,
                    building.terrain_surface_y,
                    position.y,
                    building.source_position_y,
                    building.local_min_y,
                    local_min_world_y,
                    building.terrain_sample_valid);
        building.placement_logged = true;
    }

    transform.set_position_local(position);
    transform.look_at(position + building.forward, building.up);
    entity.emplace<model_component>().set_model(building_model);
    return true;
}

auto create_login_foliage(rtti::context& ctx,
                          const std::string& content_root,
                          mcp_system::login_foliage& foliage,
                          bool allow_untextured,
                          std::vector<entt::handle>& created_entities) -> bool
{
    auto& am = ctx.get_cached<asset_manager>();
    const auto mesh_key = make_asset_key(content_root, foliage.model);
    const auto flags = load_flags::standard;

    auto mesh_handle = am.get_asset<mesh>(mesh_key, flags);
    mesh_handle.submit();
    if(!mesh_handle.is_ready())
    {
        ++foliage.attempts;
        return false;
    }

    auto mesh_instance = mesh_handle.get(false);
    if(!mesh_instance || mesh_instance->get_submeshes_count(0) == 0)
    {
        ++foliage.attempts;
        return false;
    }

    const size_t texture_slot_count = std::max<size_t>(foliage.textures.empty() ? 0u : foliage.textures.size(), 1u);
    std::vector<asset_handle<gfx::texture>> texture_handles(texture_slot_count);
    std::vector<bool> texture_usable(texture_slot_count, false);
    uint32_t usable_count = 0;

    for(size_t i = 0; i < texture_slot_count; ++i)
    {
        const std::string& texture_ref =
            (!foliage.textures.empty() && i < foliage.textures.size() && !foliage.textures[i].empty())
                ? foliage.textures[i]
                : foliage.texture;
        if(texture_ref.empty())
        {
            continue;
        }

        const auto texture_key = make_asset_key(content_root, texture_ref);
        auto texture_handle = am.get_asset<gfx::texture>(texture_key, flags);
        texture_handle.submit();
        texture_handles[i] = texture_handle;

        auto texture_instance = texture_handle.is_ready() ? texture_handle.get(false) : std::shared_ptr<gfx::texture>{};
        const bool native_valid = texture_instance && texture_instance->is_valid();
        const auto width = texture_instance ? texture_instance->info.width : 0;
        const auto height = texture_instance ? texture_instance->info.height : 0;
        texture_usable[i] = native_valid && width > 0 && height > 0;
        if(texture_usable[i])
        {
            ++usable_count;
        }
    }

    if(!foliage.texture_request_logged)
    {
        APPLOG_INFO("load_login texture requested: foliage='{}' slots={} first='{}'",
                    foliage.name,
                    texture_slot_count,
                    foliage.texture);
        foliage.texture_request_logged = true;
    }

    if(!foliage.texture_ready_logged && (usable_count == texture_slot_count || allow_untextured))
    {
        APPLOG_INFO("load_login texture resolved: foliage='{}' slots={} usable={} allow_untextured={}",
                    foliage.name,
                    texture_slot_count,
                    usable_count,
                    allow_untextured);
        foliage.texture_ready_logged = true;
    }

    if(!allow_untextured && usable_count < texture_slot_count)
    {
        ++foliage.attempts;
        return false;
    }

    std::vector<material::sptr> material_slots(texture_slot_count);
    const auto make_material = [&](size_t slot_index) -> material::sptr
    {
        if(slot_index >= material_slots.size())
        {
            slot_index = 0u;
        }
        if(material_slots[slot_index])
        {
            return material_slots[slot_index];
        }

        auto material_instance = std::make_shared<pbr_material>();
        material_instance->set_base_color({1.0f, 1.0f, 1.0f, 1.0f});
        material_instance->set_metalness(0.0f);
        material_instance->set_roughness(1.0f);
        material_instance->set_cull_type(cull_type::none);
        material_instance->set_double_sided_normal_flip(true);
        material_instance->set_alpha_blend(foliage.alpha_blend);
        if(foliage.alpha_blend)
        {
            material_instance->set_alpha_mode(alpha_mode::blend);
        }
        else if(foliage.alpha_test)
        {
            material_instance->set_alpha_mode(alpha_mode::mask);
            material_instance->set_alpha_cutoff(0.35f);
        }
        else
        {
            material_instance->set_alpha_mode(alpha_mode::opaque);
        }
        if(slot_index < texture_handles.size() && texture_usable[slot_index])
        {
            material_instance->set_color_map(texture_handles[slot_index]);
        }
        material_slots[slot_index] = material_instance;
        return material_instance;
    };

    model foliage_model;
    foliage_model.set_lod(mesh_handle, 0);

    const auto material_group_count = mesh_instance->get_data_groups_count();
    for(size_t i = 0; i < material_group_count; ++i)
    {
        size_t material_slot = i;
        if(material_slot >= texture_slot_count)
        {
            material_slot = 0u;
        }
        foliage_model.set_material_instance(make_material(material_slot), static_cast<uint32_t>(i));
    }

    auto& scn = ctx.get_cached<ecs>().get_scene();
    auto entity = scene::create_entity(*scn.registry, foliage.name);
    created_entities.emplace_back(entity);
    auto& transform = entity.get<transform_component>();
    auto position = foliage.position;
    const auto& local_bounds = mesh_instance->get_bounds();
    foliage.local_min_y = local_bounds.is_populated() && std::isfinite(local_bounds.min.y) ? local_bounds.min.y : 0.0f;
    position.y = foliage.source_position_y;
    const float local_min_world_y = position.y + foliage.local_min_y;

    if(!foliage.placement_logged)
    {
        APPLOG_INFO("load_login placement: foliage='{}' type={} x={} z={} terrain_surface_y={} final_y={} "
                    "source_y={} local_min_y={} local_min_world_y={} sample_valid={}",
                    foliage.name,
                    foliage.tree_type,
                    position.x,
                    position.z,
                    foliage.terrain_surface_y,
                    position.y,
                    foliage.source_position_y,
                    foliage.local_min_y,
                    local_min_world_y,
                    foliage.terrain_sample_valid);
        foliage.placement_logged = true;
    }

    transform.set_position_local(position);
    entity.emplace<model_component>().set_model(foliage_model);
    return true;
}
}

mcp_system::mcp_system()
    : pw_session_(pw_runtime_)
{
}

auto mcp_system::init(rtti::context& ctx) -> bool
{
    auto& ev = ctx.get_cached<events>();
    ev.on_frame_end.connect(sentinel_, -1000, this, &mcp_system::on_frame_end);

    if(pw_runtime_.init())
    {
        APPLOG_INFO("PW runtime plugin loaded");
    }
    else
    {
        APPLOG_INFO("PW runtime plugin is optional and was not loaded: {}", pw_runtime_.get_last_error());
    }
    pw_session_.start();

    const char* port_env = std::getenv("PW_MCP_PORT");
    if(port_env != nullptr)
    {
        const int port = std::atoi(port_env);
        if(port > 0)
        {
            if(!server_.Start(port))
            {
                APPLOG_ERROR("mcp control server did not start on port {}", port);
            }
        }
        else
        {
            APPLOG_WARNING("mcp control server disabled: invalid PW_MCP_PORT='{}'", port_env);
        }
    }
    else
    {
        APPLOG_INFO("mcp control server disabled: PW_MCP_PORT is not set");
    }

    return true;
}

auto mcp_system::deinit(rtti::context& ctx) -> bool
{
    (void)ctx;
    server_.Stop();
    pw_session_.shutdown();
    pw_runtime_.deinit();
    clear_pending_readback();
    return true;
}

void mcp_system::on_frame_end(rtti::context& ctx, delta_t dt)
{
    (void)dt;
    reset_login_loader_if_scene_changed(ctx);
    service_pw_session(ctx);
    service_login_loader(ctx);
    service_pending_screenshot(ctx);
    server_.Drain([&](const std::string& req)
    {
        return mcp_commands::dispatch(ctx, req, *this);
    });
}

void mcp_system::service_pw_session(rtti::context& ctx)
{
    // Applies the session controller's desired login-scene camera on the main
    // thread. The controller's worker thread never touches the scene; it only
    // publishes a snapshot, and this frame boundary performs the mutation.
    const pw_session_snapshot snapshot = pw_session_.get_snapshot();
    const pw_session_camera desired = snapshot.desired_camera;
    if(desired == pw_session_camera::none || desired == applied_pw_camera_)
    {
        return;
    }
    const login_scene_config config = get_login_scene_config();
    if(!config.loaded)
    {
        return; // preset would fail until the login scene config is parsed; retry later
    }
    try
    {
        const json request = {
            {"seq", 0},
            {"method", "camera_preset"},
            {"params", {{"preset", pw_session_camera_name(desired)}}},
        };
        const std::string response_json = mcp_commands::dispatch(ctx, request.dump(), *this);
        const json response = json::parse(response_json, nullptr, false);
        if(response.is_discarded() || !response.value("ok", false))
        {
            return; // camera not applicable yet; retry on a later frame
        }
        applied_pw_camera_ = desired;
        sync_camera_to_scene_viewport(ctx);
    }
    catch(const std::exception&)
    {
        // Camera failures must never escape the frame loop.
    }
}

auto mcp_system::ensure_camera(rtti::context& ctx) -> entt::handle
{
    auto& scn = ctx.get_cached<ecs>().get_scene();
    const bool camera_valid = mcp_cam_.valid() && mcp_cam_.all_of<transform_component, camera_component>();
    if(!camera_valid)
    {
        mcp_cam_ = defaults::create_camera_entity(ctx, scn, "MCP Camera");
        apply_login_character_camera_pose(mcp_cam_, parse_login_scene_config(login_.content_root));
    }
    return mcp_cam_;
}

void mcp_system::invalidate_camera()
{
    mcp_cam_ = {};
}

auto mcp_system::get_pw_runtime() -> pw_runtime_client&
{
    return pw_runtime_;
}

auto mcp_system::get_pw_session() -> pw_session_controller&
{
    return pw_session_;
}

void mcp_system::sync_camera_to_scene_viewport(rtti::context& ctx)
{
    auto source = ensure_camera(ctx);
    auto target = ctx.get_cached<hub>().get_panels().get_scene_panel().get_camera();
    if(!source || !target ||
       !source.all_of<transform_component, camera_component>() ||
       !target.all_of<transform_component, camera_component>())
    {
        return;
    }

    const auto& source_transform = source.get<transform_component>();
    const auto& source_camera = source.get<camera_component>();
    auto& target_transform = target.get<transform_component>();
    auto& target_camera = target.get<camera_component>();

    target_transform.set_position_global(source_transform.get_position_global());
    target_transform.set_rotation_global(source_transform.get_rotation_global());
    target_camera.set_fov(source_camera.get_fov());
    target_camera.set_near_clip(source_camera.get_near_clip());
    target_camera.set_far_clip(source_camera.get_far_clip());
}

void mcp_system::request_screenshot(const std::string& path, uint32_t w, uint32_t h, bool render_ui)
{
    clear_pending_readback();
    pending_.path = path;
    pending_.w = w;
    pending_.h = h;
    pending_.render_ui = render_ui;
    pending_.frames_left = 2;
    pending_.readback_frames_left = 0;
    pending_.active = true;
    pending_.readback_started = false;
    pending_.pixels.clear();
    pending_.last_path = path;
    pending_.last_error.clear();
    pending_.last_w = w;
    pending_.last_h = h;
    pending_.completed = false;
    ++pending_.request_id;
}

void mcp_system::despawn_loaded_map(rtti::context& ctx)
{
    auto& scene = ctx.get_cached<ecs>().get_scene();
    const bool registry_changed =
        login_.scene_registry != nullptr && login_.scene_registry != scene.registry.get();
    if(registry_changed)
    {
        login_.shared_entity_rollbacks.clear();
        login_.created_entities.clear();
        login_.scene_anchor = {};
    }
    else
    {
        for(auto it = login_.shared_entity_rollbacks.rbegin(); it != login_.shared_entity_rollbacks.rend(); ++it)
        {
            try
            {
                (*it)();
            }
            catch(const std::exception& e)
            {
                APPLOG_WARNING("load_login: failed to restore shared scene state: {}", e.what());
            }
        }
        login_.shared_entity_rollbacks.clear();
        for(auto& handle : login_.created_entities)
        {
            if(handle.valid())
            {
                handle.destroy();
            }
        }
        login_.created_entities.clear();
        if(login_.scene_anchor.valid())
        {
            login_.scene_anchor.destroy();
        }
        login_.scene_anchor = {};
    }

    // Runtime-created assets live in the manager independently of their scene
    // entities. Invalidate their typed handles and metadata only after every
    // entity/material reference has been released above.
    auto& am = ctx.get_cached<asset_manager>();
    for(const auto& key : login_.generated_mesh_keys)
    {
        am.unload_asset<mesh>(key);
        am.remove_asset_info_for_key(key);
    }
    login_.generated_mesh_keys.clear();
    for(const auto& key : login_.generated_texture_keys)
    {
        am.unload_asset<gfx::texture>(key);
        am.remove_asset_info_for_key(key);
    }
    login_.generated_texture_keys.clear();
    login_.scene_registry = nullptr;
}

void mcp_system::reset_login_loader_if_scene_changed(rtti::context& ctx)
{
    if(login_.scene_registry == nullptr)
    {
        return;
    }
    auto& scene = ctx.get_cached<ecs>().get_scene();
    entt::registry* current_registry = scene.registry.get();
    if(login_.scene_registry == current_registry && login_.scene_anchor.valid())
    {
        return;
    }
    despawn_loaded_map(ctx);
    login_ = {};
    attempted_content_root_.clear();
    attempted_map_slug_.clear();
    map_start_error_.clear();
    applied_pw_camera_ = pw_session_camera::none;
    invalidate_camera();
}

void mcp_system::start_map_load(rtti::context& ctx,
                                const std::string& content_root,
                                const std::string& map_slug,
                                uint32_t buildings_per_frame,
                                bool restart)
{
    reset_login_loader_if_scene_changed(ctx);
    std::string normalized_root;
    bool candidate_installed = false;
    attempted_content_root_ = content_root;
    attempted_map_slug_ = map_slug;
    map_start_error_.clear();
    try
    {
        if(!is_valid_map_slug(map_slug))
        {
            throw std::runtime_error("map must match [a-z0-9_]+, got '" + map_slug + "'");
        }
        normalized_root = normalize_content_root(content_root, map_slug);
        attempted_content_root_ = normalized_root;
        const auto& current_scene = ctx.get_cached<ecs>().get_scene();
        const bool current_anchor_valid = login_.scene_registry == current_scene.registry.get() &&
                                          login_.scene_anchor.valid();
        if(!restart && current_anchor_valid && login_.error.empty() && (login_.active || login_.completed) &&
           login_.content_root == normalized_root && login_.map_slug == map_slug)
        {
            attempted_content_root_.clear();
            attempted_map_slug_.clear();
            return;
        }
        if(!restart && login_.active &&
           (login_.content_root != normalized_root || login_.map_slug != map_slug))
        {
            throw std::runtime_error("another map is already loading");
        }

        // Parse the complete target manifest before mutating the scene. A bad
        // or partial conversion must not destroy the map that is still visible.
        login_loader next;
        next.content_root = normalized_root;
        next.map_slug = map_slug;
        next.buildings_per_frame = std::clamp(buildings_per_frame, 1u, 8u);
        next.lights = parse_map_lights(next.content_root, next.map_slug);
        next.terrain = load_login_terrain_heightfield(next.content_root, next.map_slug);
        auto parsed = make_login_buildings(next.content_root, next.map_slug, next.terrain);
        next.skipped = parsed.duplicate_skipped;
        next.buildings = std::move(parsed.buildings);
        auto parsed_foliage = make_login_foliage(next.content_root, next.map_slug, next.terrain);
        next.foliage_skipped = parsed_foliage.skipped;
        next.foliage = std::move(parsed_foliage.foliage);
        auto parsed_water = make_login_water(next.content_root, next.map_slug);
        next.water_skipped = parsed_water.skipped;
        next.water = std::move(parsed_water.water);
        ++next_map_generation_;
        if(next_map_generation_ == 0)
        {
            ++next_map_generation_;
        }
        next.generation = next_map_generation_;
        next.active = true;
        next.status = "loading";

        // A validated map replaces the previous one: despawn every entity the
        // previous load created so two terrains never overlap.
        despawn_loaded_map(ctx);
        login_ = std::move(next);
        candidate_installed = true;
        auto& scene = ctx.get_cached<ecs>().get_scene();
        login_.scene_registry = scene.registry.get();
        login_.scene_anchor = entt::handle(*scene.registry, scene.registry->create());
        attempted_content_root_.clear();
        attempted_map_slug_.clear();
    }
    catch(const std::exception& e)
    {
        const auto attempted_root = normalized_root.empty() ? content_root : normalized_root;
        map_start_error_ = "load_login: failed to start map '" + map_slug + "' from '" + attempted_root +
                           "': " + e.what();
        if(candidate_installed)
        {
            login_.active = false;
            login_.completed = false;
            login_.status = "error";
            login_.error = map_start_error_;
            despawn_loaded_map(ctx);
            login_.terrain = {};
            return;
        }
        const bool has_current_map = login_.active || login_.completed || login_.terrain.is_valid() ||
                                     !login_.created_entities.empty();
        if(!has_current_map)
        {
            login_ = {};
            login_.content_root = normalized_root.empty() ? content_root : normalized_root;
            login_.map_slug = map_slug;
            login_.error = map_start_error_;
            login_.status = "error";
        }
    }
}

void mcp_system::start_login_load(rtti::context& ctx,
                                  const std::string& content_root,
                                  uint32_t buildings_per_frame,
                                  bool restart)
{
    start_map_load(ctx, content_root, "login", buildings_per_frame, restart);
}

auto mcp_system::get_login_load_status() const -> login_load_status
{
    login_load_status result;
    result.status = login_.status;
    result.content_root = login_.content_root;
    result.map = login_.map_slug;
    result.error = login_.error;
    result.attempted_content_root = attempted_content_root_;
    result.attempted_map = attempted_map_slug_;
    result.start_error = map_start_error_;
    result.done = login_.cursor;
    result.total = static_cast<uint32_t>(login_.buildings.size());
    result.created = login_.created;
    result.skipped = login_.skipped;
    result.foliage_done = login_.foliage_cursor;
    result.foliage_total = static_cast<uint32_t>(login_.foliage.size());
    result.foliage_created = login_.foliage_created;
    result.foliage_skipped = login_.foliage_skipped;
    result.water_total = static_cast<uint32_t>(login_.water.size());
    result.water_created = login_.water_created;
    result.water_skipped = login_.water_skipped;
    result.terrain = login_.terrain_created;
    result.current_index = login_.cursor;

    if(login_.cursor < login_.buildings.size())
    {
        const auto& current = login_.buildings[login_.cursor];
        result.has_current = true;
        result.current_kind = "building";
        result.current_attempts = current.attempts;
        result.current_name = current.name;
        result.current_model = current.model;
        result.current_texture = current.texture;
        result.current_position = current.position;
    }
    else if(login_.foliage_cursor < login_.foliage.size())
    {
        const auto& current = login_.foliage[login_.foliage_cursor];
        result.has_current = true;
        result.current_index = login_.foliage_cursor;
        result.current_kind = "foliage";
        result.current_attempts = current.attempts;
        result.current_name = current.name;
        result.current_model = current.model;
        result.current_texture = current.texture;
        result.current_position = current.position;
    }

    if(!login_.error.empty())
    {
        result.status = "error";
    }
    else if(login_.completed)
    {
        result.status = "done";
    }
    else if(!login_.active && login_.buildings.empty())
    {
        result.status = "idle";
    }

    return result;
}

auto mcp_system::get_screenshot_status() const -> screenshot_status
{
    screenshot_status result;
    result.path = pending_.active ? pending_.path : pending_.last_path;
    result.error = pending_.last_error;
    result.w = pending_.active ? pending_.w : pending_.last_w;
    result.h = pending_.active ? pending_.h : pending_.last_h;
    result.frames_left = pending_.frames_left;
    result.readback_frames_left = pending_.readback_frames_left;
    result.active = pending_.active;
    result.readback_started = pending_.readback_started;
    result.completed = pending_.completed;
    result.request_id = pending_.request_id;

    if(pending_.active)
    {
        result.status = pending_.readback_started ? "readback" : "rendering";
    }
    else if(!pending_.last_error.empty())
    {
        result.status = "error";
    }
    else if(pending_.completed)
    {
        result.status = "done";
    }

    return result;
}

auto mcp_system::has_login_terrain() const -> bool
{
    return login_.scene_registry != nullptr && login_.scene_anchor.valid() && login_.map_slug == "login" &&
           login_.terrain.is_valid();
}

auto mcp_system::sample_login_terrain(float world_x, float world_z, float& out_height) const -> bool
{
    return login_.terrain.sample_terrain_height(world_x, world_z, out_height);
}

auto mcp_system::get_login_terrain() const -> const terrain_heightfield&
{
    return login_.terrain;
}

auto mcp_system::get_login_scene_config() const -> login_scene_config
{
    return parse_login_scene_config(login_.content_root);
}

auto mcp_system::get_login_content_root() const -> const std::string&
{
    return login_.content_root;
}

void mcp_system::service_login_loader(rtti::context& ctx)
{
    if(!login_.active)
    {
        return;
    }

    try
    {
        uint32_t processed_this_frame = 0;
        login_.status = "loading";

        if(!login_.environment_created)
        {
            create_login_environment(ctx, login_.created_entities, login_.shared_entity_rollbacks);
            create_login_lights(ctx,
                                login_.map_slug,
                                login_.lights,
                                login_.created_entities,
                                login_.shared_entity_rollbacks);
            create_login_effects(ctx, login_.content_root, login_.map_slug, login_.created_entities);
            login_.environment_created = true;
        }

        while(login_.cursor < login_.buildings.size() && processed_this_frame < login_.buildings_per_frame)
        {
            auto& building = login_.buildings[login_.cursor];
            const bool wait_limit_reached = building.attempts >= kLoginMaxAssetWaitFrames;
            if(!create_login_building(ctx, login_.content_root, building, wait_limit_reached, login_.created_entities))
            {
                if(building.attempts < kLoginMaxAssetWaitFrames)
                {
                    login_.status = "waiting_assets";
                    return;
                }

                ++login_.skipped;
                ++login_.cursor;
                continue;
            }

            ++login_.created;
            ++login_.cursor;
            ++processed_this_frame;
        }

        if(login_.cursor >= login_.buildings.size() && login_.map_slug == "login")
        {
            if(!create_login_character(ctx, login_.content_root, login_.created_entities))
            {
                // The preview character is decorative: bound the wait and continue
                // without it instead of blocking the scene load forever.
                if(login_.character_attempts < kLoginMaxAssetWaitFrames)
                {
                    ++login_.character_attempts;
                    login_.status = "waiting_assets";
                    return;
                }
                APPLOG_WARNING("load_login: character assets unavailable after {} frames; continuing without the preview character",
                               kLoginMaxAssetWaitFrames);
            }
        }

        while(login_.cursor >= login_.buildings.size() && login_.foliage_cursor < login_.foliage.size() &&
              processed_this_frame < login_.buildings_per_frame)
        {
            auto& foliage = login_.foliage[login_.foliage_cursor];
            const bool wait_limit_reached = foliage.attempts >= kLoginMaxAssetWaitFrames;
            if(!create_login_foliage(ctx, login_.content_root, foliage, wait_limit_reached, login_.created_entities))
            {
                if(foliage.attempts < kLoginMaxAssetWaitFrames)
                {
                    login_.status = "waiting_assets";
                    return;
                }

                ++login_.foliage_skipped;
                ++login_.foliage_cursor;
                continue;
            }

            ++login_.foliage_created;
            ++login_.foliage_cursor;
            ++processed_this_frame;
        }

        if(login_.cursor >= login_.buildings.size() && login_.foliage_cursor >= login_.foliage.size())
        {
            if(!login_.terrain_created)
            {
                create_login_terrain(ctx,
                                     login_.content_root,
                                     login_.map_slug,
                                     login_.generation,
                                     login_.terrain,
                                     login_.created_entities,
                                     login_.generated_mesh_keys,
                                     login_.generated_texture_keys);
                login_.terrain_created = true;
            }

            if(!login_.water_created_flag)
            {
                for(size_t i = 0; i < login_.water.size(); ++i)
                {
                    try
                    {
                        if(create_login_water_surface(ctx,
                                                      login_.content_root,
                                                      login_.map_slug,
                                                      login_.generation,
                                                      login_.water[i],
                                                      static_cast<uint32_t>(i),
                                                      login_.created_entities,
                                                      login_.generated_mesh_keys))
                        {
                            ++login_.water_created;
                        }
                        else
                        {
                            ++login_.water_skipped;
                        }
                    }
                    catch(const std::exception& e)
                    {
                        ++login_.water_skipped;
                        APPLOG_WARNING("load_login water skipped: name='{}' payload='{}' reason='{}'",
                                       login_.water[i].name,
                                       login_.water[i].payload,
                                       e.what());
                    }
                }
                login_.water_created_flag = true;
            }

            login_.active = false;
            login_.completed = true;
            login_.status = "done";
            APPLOG_INFO("load_login done: placed={} skipped={} foliage={} foliage_skipped={} water={} water_skipped={}",
                        login_.created,
                        login_.skipped,
                        login_.foliage_created,
                        login_.foliage_skipped,
                        login_.water_created,
                        login_.water_skipped);
            invalidate_camera();
        }
    }
    catch(const std::exception& e)
    {
        login_.active = false;
        login_.completed = false;
        login_.status = "error";
        login_.error = e.what();
        despawn_loaded_map(ctx);
        login_.terrain = {};
        login_.terrain_created = false;
        login_.water_created_flag = false;
        login_.environment_created = false;
    }
}

void mcp_system::service_pending_screenshot(rtti::context& ctx)
{
    if(!pending_.active)
    {
        return;
    }

    try
    {
        if(pending_.readback_started)
        {
            --pending_.readback_frames_left;
            if(pending_.readback_frames_left <= 0)
            {
                bx::FilePath file_path(pending_.path.c_str());
                if(!bx::makeAll(file_path.getPath()))
                {
                    throw std::runtime_error("screenshot: failed to create output directory");
                }

                bx::FileWriter writer;
                if(!bx::open(&writer, file_path))
                {
                    throw std::runtime_error("screenshot: failed to open output file");
                }

                bx::Error err;
                bimg::imageWritePng(&writer,
                                    pending_.w,
                                    pending_.h,
                                    pending_.w * 4,
                                    pending_.pixels.data(),
                                    static_cast<bimg::TextureFormat::Enum>(bgfx::TextureFormat::RGBA8),
                                    false,
                                    &err);
                bx::close(&writer);

                pending_.last_path = pending_.path;
                pending_.last_w = pending_.w;
                pending_.last_h = pending_.h;
                pending_.last_error.clear();
                pending_.completed = true;
                clear_pending_readback();
                pending_.active = false;
            }
            return;
        }

        auto& scn = ctx.get_cached<ecs>().get_scene();
        auto camera = ensure_camera(ctx);
        auto& camera_comp = camera.get<camera_component>();
        camera_comp.set_viewport_size({pending_.w, pending_.h});

        auto& rpath = ctx.get_cached<rendering_system>();
        rpath.on_frame_update(scn, kMcpFrameDt);
        rpath.on_frame_before_render(scn, kMcpFrameDt);
        rpath.render_scene(camera, camera_comp, scn, kMcpFrameDt, pending_.render_ui);

        --pending_.frames_left;
        if(pending_.frames_left <= 0)
        {
            const auto& obuffer = camera_comp.get_render_view().fbo_safe_get("OBUFFER");
            if(!obuffer)
            {
                throw std::runtime_error("screenshot: OBUFFER unavailable");
            }

            const auto input_tex = bgfx::getTexture(obuffer->native_handle());
            const auto format = bgfx::TextureFormat::RGBA8;
            const uint64_t flags = BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | BGFX_SAMPLER_U_CLAMP |
                                   BGFX_SAMPLER_V_CLAMP;
            pending_.readback_texture =
                bgfx::createTexture2D(static_cast<uint16_t>(pending_.w),
                                      static_cast<uint16_t>(pending_.h),
                                      false,
                                      1,
                                      format,
                                      flags,
                                      nullptr);

            bgfx::TextureInfo info;
            bgfx::calcTextureSize(info, pending_.w, pending_.h, 1, false, false, 1, format);
            pending_.pixels.resize(info.storageSize);

            bgfx::ViewId view_id = gfx::render_pass("MCP Blit").id;
            bgfx::touch(view_id);
            bgfx::blit(view_id, pending_.readback_texture, 0, 0, input_tex);
            bgfx::readTexture(pending_.readback_texture, pending_.pixels.data());

            pending_.readback_started = true;
            pending_.readback_frames_left = 3;
        }
    }
    catch(const std::exception& e)
    {
        pending_.last_path = pending_.path;
        pending_.last_w = pending_.w;
        pending_.last_h = pending_.h;
        pending_.last_error = e.what();
        pending_.completed = false;
        clear_pending_readback();
        pending_.active = false;
    }
    catch(...)
    {
        pending_.last_path = pending_.path;
        pending_.last_w = pending_.w;
        pending_.last_h = pending_.h;
        pending_.last_error = "screenshot: unknown failure";
        pending_.completed = false;
        clear_pending_readback();
        pending_.active = false;
    }
}

void mcp_system::clear_pending_readback()
{
    if(bgfx::isValid(pending_.readback_texture))
    {
        bgfx::destroy(pending_.readback_texture);
        pending_.readback_texture = BGFX_INVALID_HANDLE;
    }
    pending_.readback_started = false;
    pending_.readback_frames_left = 0;
    pending_.pixels.clear();
}
} // namespace unravel
