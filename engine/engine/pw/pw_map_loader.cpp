#include "pw_map_loader.h"
#include "pw_map_manifest.h"
#include "pw_map_effects.h"
#include <engine/threading/threader.h>
#include <unordered_set>
#include <unordered_map>



#include <engine/pw/detail/json.hpp>

#include <engine/animation/animation.h>
#include <engine/animation/ecs/components/animation_component.h>
#include <engine/assets/asset_manager.h>
#include <engine/defaults/defaults.h>
#include <engine/ecs/components/tag_component.h>
#include <engine/ecs/components/id_component.h>
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
#include <graphics/vertex_buffer.h>
#include <graphics/index_buffer.h>
#include <logging/logging.h>

#include <bimg/encode.h>
#include <bx/file.h>




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

constexpr auto kMapAssetTimeout = std::chrono::minutes(30);
constexpr const char* kLoginCharacterEntityName = "Login Character";
// Dressed char-select Blademaster: body + Guardian armor (AddSkinFile) + sword (AddChildModel),
// baked into model_1c939f0c.gltf by ECModelViewer. Same male Blademaster skeleton as the bare body, so the
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
    std::vector<pw_map_material_slot> slots;
};

auto decode_map_material_slots(const json& material_doc, float default_cutoff, bool force_alpha_test)
    -> std::vector<pw_map_material_slot>
{
    std::string fallback_ref = material_doc.value("textureRef", std::string{});
    if(fallback_ref.empty() && material_doc.contains("params") && material_doc["params"].is_object())
    {
        fallback_ref = material_doc["params"].value("diffuseTextureRef", std::string{});
    }

    pw_map_material_slot fallback;
    fallback.texture = fallback_ref;
    fallback.alpha_cutoff = default_cutoff;
    if(material_doc.contains("params") && material_doc["params"].is_object())
    {
        const auto& params = material_doc["params"];
        fallback.alpha_blend = read_boolish_member(params, "alphaBlend", false);
        fallback.alpha_test = read_boolish_member(params, "alphaTest", false);
        fallback.two_sided = read_boolish_member(params, "twoSided", false);
    }

    std::vector<pw_map_material_slot> slots;
    std::unordered_set<int> indices;
    if(material_doc.contains("materialSlots") && material_doc["materialSlots"].is_array())
    {
        for(const auto& slot : material_doc["materialSlots"])
        {
            const int index = slot.at("index").get<int>();
            if(index < 0 || index > 65535 || !indices.insert(index).second)
                throw std::runtime_error("invalid or duplicate map material slot index");
            if(!slot.contains("alphaBlend") || !slot.contains("alphaCutoff"))
                throw std::runtime_error("map material needs per-slot alphaBlend/alphaCutoff; regenerate with the current converter");
            pw_map_material_slot parsed;
            parsed.texture = slot.value("diffuseTextureRef", std::string{});
            parsed.alpha_test = force_alpha_test || read_boolish_member(slot, "alphaTest", false);
            parsed.alpha_blend = !parsed.alpha_test && read_boolish_member(slot, "alphaBlend", false);
            parsed.two_sided = read_boolish_member(slot, "twoSided", false);
            parsed.alpha_cutoff = force_alpha_test ? default_cutoff : slot.at("alphaCutoff").get<float>();
            if(!std::isfinite(parsed.alpha_cutoff) || parsed.alpha_cutoff < 0 || parsed.alpha_cutoff > 1)
                throw std::runtime_error("map material alpha cutoff is outside [0,1]");
            if(static_cast<size_t>(index) >= slots.size()) slots.resize(static_cast<size_t>(index) + 1u);
            slots[static_cast<size_t>(index)] = std::move(parsed);
        }
    }

    if(slots.empty())
    {
        if(fallback_ref.empty())
        {
            throw std::runtime_error("map material has no textureRef");
        }
        fallback.alpha_test = force_alpha_test || fallback.alpha_test;
        fallback.alpha_blend = !fallback.alpha_test && fallback.alpha_blend;
        slots.push_back(fallback);
    }

    else if(indices.size() != slots.size())
        throw std::runtime_error("map material slots are not contiguous");
    return slots;
}

auto read_login_material_info(const std::string& content_root, const std::string& material_ref,
                              float default_cutoff = 0.5f, bool force_alpha_test = false) -> login_material_info
{
    login_material_info info;
    info.slots = decode_map_material_slots(read_json_asset(make_asset_key(content_root, material_ref)),
                                          default_cutoff, force_alpha_test);
    for(const auto& slot : info.slots) info.textures.push_back(slot.texture);
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
                    APPLOG_TRACE("load_login terrain albedo composed map: ref='{}'", baked_albedo_ref);
                    add_unique_terrain_albedo_ref(refs, baked_albedo_ref);
                    // Raw layers are tiled inputs to this composed map, never
                    // substitutes while its GPU resource is still compiling.
                    return refs;
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
    -> std::vector<pw_map_loader::login_light>
{
    const auto lights_doc = read_map_lights_document(content_root, map_slug);
    const auto& items = lights_doc["lights"];
    std::vector<pw_map_loader::login_light> result;
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
            pw_map_loader::login_light parsed;
            parsed.source_index = static_cast<uint32_t>(i);

            if(type == "directional")
            {
                parsed.type = pw_map_loader::login_light::kind::directional;
                parsed.direction = read_login_light_vec3_member(item, "direction");
                validate_login_light_vec3(parsed.direction, "direction");
                parsed.direction = normalize_login_light_direction(parsed.direction);
            }
            else if(type == "point")
            {
                parsed.type = pw_map_loader::login_light::kind::point;
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
    std::vector<pw_map_loader::login_building> buildings;
    uint32_t duplicate_skipped = 0;
};

struct login_foliage_parse_result
{
    std::vector<pw_map_loader::login_foliage> foliage;
    uint32_t skipped = 0;
};

struct login_water_parse_result
{
    std::vector<pw_map_loader::login_water> water;
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

auto load_login_terrain_albedo(rtti::context& ctx, const std::string& content_root, const std::vector<std::string>& albedo_refs)
    -> asset_handle<gfx::texture>
{
    try
    {
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
                APPLOG_TRACE("load_login terrain albedo missing: ref='{}' key='{}' path='{}'",
                            albedo_ref,
                            texture_key,
                            texture_path.generic_string());
                continue;
            }

            auto texture_handle = am.get_asset<gfx::texture>(texture_key, load_flags::standard);
            texture_handle.submit();

            auto texture_instance = texture_handle.get_if_ready();
            const bool texture_loaded = static_cast<bool>(texture_instance);
            const bool native_valid = texture_instance && texture_instance->is_valid();
            const auto native_idx = native_valid ? texture_instance->native_handle().idx : bgfx::kInvalidHandle;
            const auto width = texture_instance ? texture_instance->info.width : 0;
            const auto height = texture_instance ? texture_instance->info.height : 0;
            const auto format = texture_instance ? static_cast<int>(texture_instance->info.format) : -1;
            const bool texture_usable = native_valid && width > 0 && height > 0;

            APPLOG_TRACE("load_login terrain albedo requested: ref='{}' key='{}' handle_valid={} ready={} loaded={} "
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

auto make_login_buildings(const std::string& content_root, const std::string& map_slug, const terrain_heightfield& terrain, const json& scene_doc)
    -> login_buildings_parse_result
{
    if(!scene_doc.contains("buildings") || !scene_doc["buildings"].is_array())
    {
        throw std::runtime_error("load_login: scene.eds.json has no buildings[]");
    }

    login_buildings_parse_result result;
    std::unordered_set<std::string> accepted_ids;
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
        const std::string source_id = item.value("sourceId", std::string{});
        if(source_id.empty())
        {
            throw std::runtime_error("map building is missing its sourceId");
        }
        if(!accepted_ids.insert(source_id).second)
        {
            ++result.duplicate_skipped;
            continue;
        }

        pw_map_loader::login_building building;
        building.source_id = source_id;
        building.name = item.value("name", std::string("Login Building ") + std::to_string(result.buildings.size()));
        building.model = model_ref;
        const auto material_info = read_login_material_info(content_root, material_ref);
        building.textures = material_info.textures;
        building.materials = material_info.slots;
        building.texture = building.textures.empty() ? std::string{} : building.textures.front();
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

auto make_map_mesh_instances(const std::string& content_root, const json& scene_doc,
                             const std::string& kind, bool require_full)
    -> std::vector<pw_map_loader::login_building>
{
    std::vector<pw_map_loader::login_building> result;
    std::unordered_set<std::string> identities;
    for(const auto& node : scene_doc.at("nodes"))
    {
        if(node.value("kind", std::string{}) != kind) continue;
        const auto& format = node.at("openFormat");
        if(!format.value("convertedToOpenFormat", false))
        {
            if(require_full) throw std::runtime_error("unconverted required " + kind + " instance");
            continue;
        }
        const std::string identity = node.value("sourceId", format.value("sourceId", std::string{}));
        if(identity.empty()) throw std::runtime_error(kind + " instance has no source identity");
        if(!identities.insert(identity).second) continue;
        pw_map_loader::login_building item;
        item.kind = kind;
        item.source_id = identity;
        item.name = node.value("name", identity);
        item.model = format.at("model").get<std::string>();
        const auto material = read_login_material_info(content_root, format.at("material").get<std::string>(),
                                                       format.value("alphaCutoff", 0.5f), kind == "Grass");
        item.textures = material.textures;
        item.materials = material.slots;
        item.texture = item.textures.empty() ? std::string{} : item.textures.front();
        item.alpha_cutoff = format.value("alphaCutoff", 0.5f);
        item.position = kind == "Grass" ? read_vec3_array(format.at("position"), "grass position") :
                                          map_source_position_to_unravel(read_vec3_member(node, "pos"));
        item.source_position_y = item.position.y;
        if(kind == "ECModel")
        {
            item.forward = map_source_forward_to_unravel(read_vec3_member(node, "dir"));
            item.up = map_source_up_to_unravel(read_vec3_member(node, "up"));
            item.animation = format.at("animation").get<std::string>();
            item.animation_loop = format.value("loop", true);
        }
        result.emplace_back(std::move(item));
    }
    return result;
}

auto make_login_water(const std::string& content_root, const std::string& map_slug, const json& scene_doc) -> login_water_parse_result
{

    login_water_parse_result result;
    if(!scene_doc.contains("nodes") || !scene_doc["nodes"].is_array())
    {
        APPLOG_WARNING("load_login: scene.eds.json has no nodes[]; water skipped");
        return result;
    }

    std::unordered_set<std::string> accepted_keys;
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
            const std::string source_id = item.value("sourceId", std::string{});
            if(source_id.empty())
            {
                throw std::runtime_error("map water is missing its sourceId");
            }
            if(!accepted_keys.insert(source_id).second)
            {
                ++result.skipped;
                continue;
            }

            pw_map_loader::login_water water;
            water.source_id = source_id;
            water.surface_id = water_surface.value("sourceId", std::string{});
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
            throw std::runtime_error(std::string("map source record failed: ") + e.what());
            APPLOG_WARNING("load_login water skipped: name='{}' reason='{}'",
                           item.value("name", std::string("<unnamed>")),
                           e.what());
        }
    }

    APPLOG_INFO("load_login parsed water: placed={} skipped={}", result.water.size(), result.skipped);

    return result;
}

auto make_login_foliage(const std::string& content_root, const std::string& map_slug, const terrain_heightfield& terrain, const json& scene_doc)
    -> login_foliage_parse_result
{

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

            pw_map_loader::login_foliage foliage;
            foliage.source_id = item.value("sourceId", std::string("tree:") + std::to_string(item.value("exportId", 0u)));
            foliage.name = item.value("name", std::string("Tree ") + std::to_string(result.foliage.size()));
            foliage.model = model_ref;
            const auto material_info = read_login_material_info(content_root, material_ref);
            foliage.textures = material_info.textures;
            foliage.materials = material_info.slots;
            foliage.texture = foliage.textures.empty() ? std::string{} : foliage.textures.front();
            foliage.position = map_source_position_to_unravel(source_position);
            foliage.source_position_y = source_position.y;
            foliage.tree_type = foliage_instance.value("treeType", item.value("treeType", -1));
            foliage.terrain_sample_valid =
                terrain.sample_terrain_height(foliage.position.x, foliage.position.z, foliage.terrain_surface_y);

            result.foliage.emplace_back(std::move(foliage));
        }
        catch(const std::exception& e)
        {
            throw std::runtime_error(std::string("map source record failed: ") + e.what());
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

auto create_login_terrain(rtti::context& ctx,
                          const std::string& content_root,
                          const std::string& map_slug,
                          uint64_t generation,
                          const terrain_heightfield& terrain,
                          const std::shared_ptr<mesh>& prepared_terrain,
                          const std::vector<std::string>& terrain_albedo_refs,
                          std::vector<entt::handle>& created_entities,
                          std::vector<std::string>& generated_mesh_keys,
                          std::vector<std::string>& generated_texture_keys) -> bool
{
    const auto terrain_albedo = load_login_terrain_albedo(ctx, content_root, terrain_albedo_refs);
    const auto albedo_texture = terrain_albedo.get_if_ready();
    if(!albedo_texture || !albedo_texture->is_valid()) return false;
    auto terrain_mesh = prepared_terrain;
    if(!terrain_mesh || !terrain_mesh->upload_gpu_buffers())
    {
        throw std::runtime_error("map: failed to upload prepared terrain");
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

    material_instance->set_color_map(terrain_albedo);
    APPLOG_INFO("map '{}' terrain GPU ready: albedo='{}' dimensions={}x{}", map_slug,
                terrain_albedo.id(), albedo_texture->info.width, albedo_texture->info.height);

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
    return true;
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
                                const pw_map_loader::login_water& water,
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
    auto mesh_instance = mesh_handle.get_if_ready();
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
                         const std::vector<pw_map_loader::login_light>& lights,
                         entt::handle sun,
                         std::vector<entt::handle>& created_entities)
{
    auto& scn = ctx.get_cached<ecs>().get_scene();
    size_t directional_count = 0;
    size_t point_count = 0;

    for(const auto& light_item : lights)
    {
        if(light_item.type == pw_map_loader::login_light::kind::directional)
        {
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

        if(light_item.type == pw_map_loader::login_light::kind::point)
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

auto create_login_environment(rtti::context& ctx,
                              std::vector<entt::handle>& created_entities) -> entt::handle
{
    auto& scn = ctx.get_cached<ecs>().get_scene();
    auto volume = defaults::create_volume_entity(ctx, scn, "PW Map Volume", volume_mode::global);
    created_entities.emplace_back(volume);
    auto& tonemapping = volume.get<tonemapping_component>();
    tonemapping.enabled = true;
    tonemapping.settings.method = tonemapping_method::aces;
    tonemapping.settings.exposure = 0.85f;
    volume.get<auto_exposure_component>().enabled = false;
    auto sun = defaults::create_light_entity(ctx, scn, light_type::directional, "PW Map Sun");
    created_entities.emplace_back(sun);
    auto& skylight = sun.get_or_emplace<skylight_component>();
    skylight.set_cloud_mode(skylight_component::cloud_mode::none);
    skylight.set_irradiance_intensity(0.35f);
    auto reflection = defaults::create_reflection_probe_entity(ctx, scn, probe_type::sphere, " PW Map Global");
    created_entities.emplace_back(reflection);
    auto& reflection_comp = reflection.get<reflection_probe_component>();
    auto probe = reflection_comp.get_probe();
    probe.method = reflect_method::environment;
    probe.sphere_data.range = 1600.0f;
    reflection_comp.set_probe(probe);
    return sun;
}

void suppress_external_map_environment(rtti::context& ctx,
                                      const std::vector<entt::handle>& owned,
                                      pw_map_environment_state& rollbacks)
{
    auto& scn = ctx.get_cached<ecs>().get_scene();
    for(const auto entity : scn.registry->view<transform_component>())
    {
        const entt::handle handle(*scn.registry, entity);
        if(is_map_owned_entity(owned, handle)) continue;
        const auto* light = handle.try_get<light_component>();
        const auto* volume = handle.try_get<volume_component>();
        const auto* tag = handle.try_get<tag_component>();
        const bool global_light = light && light->get_light().type == light_type::directional;
        const bool global_volume = volume && volume->mode == volume_mode::global;
        const bool global_probe = tag && tag->name == "Reflection Probe Global";
        if(!global_light && !global_volume && !global_probe) continue;
        const bool was_active = handle.get<transform_component>().is_active();
        auto& id = handle.get_or_emplace<id_component>();
        id.generate_if_nil();
        rollbacks.emplace_back(id.id, was_active);
        handle.get<transform_component>().set_active(false);
    }
}

auto create_login_building(rtti::context& ctx,
                           const std::string& content_root,
                           pw_map_loader::login_building& building,
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

    auto mesh_instance = mesh_handle.get_if_ready();
    if(!mesh_instance || mesh_instance->get_submeshes_count(0) == 0 ||
       !mesh_instance->get_hardware_vb() || !mesh_instance->get_hardware_vb()->is_valid() ||
       !mesh_instance->get_hardware_ib(0) || !mesh_instance->get_hardware_ib(0)->is_valid())
    {
        ++building.attempts;
        return false;
    }

    asset_handle<animation_clip> animation_handle;
    if(!building.animation.empty())
    {
        animation_handle = am.get_asset<animation_clip>(make_asset_key(content_root, building.animation), flags);
        const auto animation = animation_handle.get_if_ready();
        if(!animation)
        {
            ++building.attempts;
            return false;
        }
        if(animation->channels.empty() || animation->duration.count() <= 0)
            throw std::runtime_error("required animation is empty: " + building.animation);
    }

    const size_t texture_slot_count = std::max<size_t>(building.textures.empty() ? 0u : building.textures.size(), 1u);
    std::vector<asset_handle<gfx::texture>> texture_handles(texture_slot_count);
    std::vector<bool> texture_usable(texture_slot_count, false);
    uint32_t usable_count = 0;

    for(size_t i = 0; i < texture_slot_count; ++i)
    {
        const std::string& texture_ref =
            (!building.textures.empty() && i < building.textures.size())
                ? building.textures[i]
                : building.texture;
        if(texture_ref.empty())
        {
            texture_usable[i] = true;
            ++usable_count;
            continue;
        }

        const auto texture_key = make_asset_key(content_root, texture_ref);
        auto texture_handle = am.get_asset<gfx::texture>(texture_key, flags);
        texture_handle.submit();
        texture_handles[i] = texture_handle;

        auto texture_instance = texture_handle.get_if_ready();
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
        const auto& source = building.materials.at(slot_index);
        material_instance->set_cull_type(source.two_sided ? cull_type::none : cull_type::counter_clockwise);
        material_instance->set_double_sided_normal_flip(source.two_sided);
        material_instance->set_alpha_blend(source.alpha_blend);
        if(source.alpha_test)
        {
            material_instance->set_alpha_mode(alpha_mode::mask);
            material_instance->set_alpha_cutoff(source.alpha_cutoff);
        }
        else if(source.alpha_blend) material_instance->set_alpha_mode(alpha_mode::blend);
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
    entity.get<tag_component>().tag = building.source_id;
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
    if(animation_handle)
    {
        auto& animation = entity.emplace<animation_component>();
        animation.set_animation(animation_handle);
        animation.set_autoplay(false);
        animation.set_apply_root_motion(false);
        animation.set_speed(1.0f);
        animation.set_culling_mode(animation_component::culling_mode::renderer_based);
        auto& player = animation.get_player();
        player.blend_to(0, animation_handle, animation_clip::seconds_t(0), building.animation_loop);
        player.play();
    }
    return true;
}

auto create_login_foliage(rtti::context& ctx,
                          const std::string& content_root,
                          pw_map_loader::login_foliage& foliage,
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

    auto mesh_instance = mesh_handle.get_if_ready();
    if(!mesh_instance || mesh_instance->get_submeshes_count(0) == 0 ||
       !mesh_instance->get_hardware_vb() || !mesh_instance->get_hardware_vb()->is_valid() ||
       !mesh_instance->get_hardware_ib(0) || !mesh_instance->get_hardware_ib(0)->is_valid())
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
            (!foliage.textures.empty() && i < foliage.textures.size())
                ? foliage.textures[i]
                : foliage.texture;
        if(texture_ref.empty())
        {
            texture_usable[i] = true;
            ++usable_count;
            continue;
        }

        const auto texture_key = make_asset_key(content_root, texture_ref);
        auto texture_handle = am.get_asset<gfx::texture>(texture_key, flags);
        texture_handle.submit();
        texture_handles[i] = texture_handle;

        auto texture_instance = texture_handle.get_if_ready();
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
        const auto& source = foliage.materials.at(slot_index);
        material_instance->set_cull_type(source.two_sided ? cull_type::none : cull_type::counter_clockwise);
        material_instance->set_double_sided_normal_flip(source.two_sided);
        material_instance->set_alpha_blend(source.alpha_blend);
        if(source.alpha_test)
        {
            material_instance->set_alpha_mode(alpha_mode::mask);
            material_instance->set_alpha_cutoff(source.alpha_cutoff);
        }
        else if(source.alpha_blend) material_instance->set_alpha_mode(alpha_mode::blend);
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
    entity.get<tag_component>().tag = foliage.source_id;
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

auto parse_pw_map_material_slots(const std::string& document, float default_cutoff, bool force_alpha_test)
    -> std::vector<pw_map_material_slot>
{
    return decode_map_material_slots(nlohmann::json::parse(document), default_cutoff, force_alpha_test);
}

auto pw_map_ownership::rebind(entt::registry& registry, entt::handle& root, std::vector<entt::handle>& entities,
                              bool require_all) const -> bool
{
    // Work only in the current registry; the old handles may already point to freed storage.
    root = {};
    entities.clear();
    if(root_id.is_nil() || root_tag.empty()) return false;
    std::unordered_map<hpp::uuid, entt::handle> by_id;
    size_t roots = 0;
    for(const auto entity : registry.view<id_component, transform_component>())
    {
        const entt::handle handle(registry, entity);
        const auto id = handle.get<id_component>().id;
        if(id.is_nil()) continue;
        if(!by_id.emplace(id, handle).second) return false;
        const auto* tag = handle.try_get<tag_component>();
        if(tag && tag->tag == root_tag)
        {
            ++roots;
            if(id == root_id) root = handle;
        }
    }
    if(roots != 1 || !root.valid())
    {
        root = {};
        return false;
    }
    std::vector<entt::handle> resolved;
    resolved.reserve(entity_ids.size());
    for(const auto& id : entity_ids)
    {
        const auto found = by_id.find(id);
        if(found == by_id.end() || !transform_component::is_parent_of(root, found->second))
        {
            if(require_all) return false;
            continue;
        }
        resolved.push_back(found->second);
    }
    entities = std::move(resolved);
    return true;
}

void restore_pw_map_environment(entt::registry& registry, pw_map_environment_state& original_states)
{
    for(auto original = original_states.rbegin(); original != original_states.rend(); ++original)
    {
        const auto entity = find_entity_by_uuid(registry, original->first);
        if(entity.valid() && entity.all_of<transform_component>())
            entity.get<transform_component>().set_active(original->second);
    }
    original_states.clear();
}

auto pw_map_loader::init(rtti::context& ctx) -> bool
{
    if(!sentinel_) sentinel_ = std::make_shared<int>(0);
    ctx.get_cached<events>().on_frame_end.connect(sentinel_, -999, this, &pw_map_loader::on_frame_end);
    // Snapshot before the editor serializes and reloads its checkpoint (1000).
    ctx.get_cached<events>().on_play_before_begin.connect(sentinel_, 2000, this, &pw_map_loader::on_play_before_begin);
    // Checkpoint restoration (-1000) and animation-system autoplay (10) run first.
    ctx.get_cached<events>().on_play_begin.connect(sentinel_, -2000, this, &pw_map_loader::on_play_transition);
    ctx.get_cached<events>().on_play_after_end.connect(sentinel_, -2000, this, &pw_map_loader::on_play_after_end);
    ctx.get_cached<events>().on_skip_next_frame.connect(sentinel_, 10, this, &pw_map_loader::on_skip_next_frame);
    return true;
}
auto pw_map_loader::deinit(rtti::context& ctx) -> bool
{
    sentinel_.reset();
    despawn_loaded_map(ctx);
    auto checkpoint = std::move(play_checkpoint_);
    play_session_ = false;
    if(checkpoint) destroy_map(ctx, checkpoint->map);
    login_ = {};
    attempted_content_root_.clear();
    attempted_map_slug_.clear();
    map_start_error_.clear();
    return true;
}
void pw_map_loader::on_frame_end(rtti::context& ctx, delta_t dt)
{
    reset_login_loader_if_scene_changed(ctx);
    install_prepared_map(ctx);
    service_login_loader(ctx);
    update_map_effects(ctx, dt);
}
void pw_map_loader::on_skip_next_frame(rtti::context& ctx)
{
    update_map_effects(ctx, delta_t(1.0f / 60.0f));
}
void pw_map_loader::update_map_effects(rtti::context& ctx, delta_t dt)
{
    auto& accepted = previous_ ? *previous_ : login_;
    if(accepted.completed && accepted.effects)
    {
        accepted.effects->update(ctx, dt.count());
        if(!accepted.effects->error().empty() && accepted.error != accepted.effects->error())
        {
            accepted.error = accepted.effects->error();
            accepted.status = "error";
            APPLOG_ERROR("map '{}' effect update failed: {}", accepted.map_slug, accepted.error);
        }
    }
}
void pw_map_loader::on_play_transition(rtti::context& ctx)
{
    // A splash scene can temporarily remove the game graph before on_play_begin.
    // Reattach its retained map when the editor installs the deferred checkpoint.
    if(play_checkpoint_ && login_.scene_registry == nullptr)
    {
        auto& current = ctx.get_cached<ecs>().get_scene();
        entt::handle root;
        std::vector<entt::handle> entities;
        if(play_checkpoint_->map.ownership.rebind(*current.registry, root, entities))
        {
            login_ = play_checkpoint_->map;
            login_.scene_registry = current.registry.get();
            login_.scene_anchor = root;
            login_.created_entities = std::move(entities);
        }
    }
    reset_login_loader_if_scene_changed(ctx, true);
    // Animation players are transient across serialization. Restart the source's
    // idle clip after both scene cloning and editor restoration, retaining loop policy.
    const auto restart = [](login_loader& state)
    {
        for(const auto& item : state.ecmodels)
        {
            if(!item.ready || item.animation.empty()) continue;
            for(const auto entity : state.created_entities)
            {
                const auto* tag = entity.valid() ? entity.try_get<tag_component>() : nullptr;
                auto* animation = entity.valid() ? entity.try_get<animation_component>() : nullptr;
                if(!tag || tag->tag != item.source_id || !animation) continue;
                animation->set_autoplay(false);
                auto& player = animation->get_player();
                player.stop();
                player.blend_to(0, animation->get_animation(), animation_clip::seconds_t(0), item.animation_loop);
                player.play();
            }
        }
    };
    if(login_.scene_registry) restart(login_);
    if(previous_ && previous_->scene_registry) restart(*previous_);
}
auto pw_map_loader::is_play_checkpoint(const login_loader& state) const -> bool
{
    return play_checkpoint_ && state.generation == play_checkpoint_->map.generation &&
           state.ownership.root_id == play_checkpoint_->map.ownership.root_id;
}

void pw_map_loader::on_play_before_begin(rtti::context& ctx)
{
    if(play_session_) return;
    reset_login_loader_if_scene_changed(ctx);
    // A serialized checkpoint must contain the accepted map, never an inactive
    // half-published candidate whose preparation job cannot be serialized.
    const bool cancelled = preparing_ || login_.active;
    cancel_preparation(ctx);
    if(login_.active)
    {
        destroy_map(ctx, login_);
        login_ = previous_ ? std::move(*previous_) : login_loader{};
        previous_.reset();
    }
    if(cancelled)
    {
        attempted_content_root_.clear();
        attempted_map_slug_.clear();
        map_start_error_.clear();
        APPLOG_INFO("unpublished map load cancelled before editor Play checkpoint");
    }
    play_session_ = true;
    if(login_.completed)
    {
        play_checkpoint_ = std::make_unique<play_checkpoint>();
        play_checkpoint_->map = login_;
        play_checkpoint_->attempted_content_root = attempted_content_root_;
        play_checkpoint_->attempted_map_slug = attempted_map_slug_;
        play_checkpoint_->start_error = map_start_error_;
    }
}

void pw_map_loader::on_play_after_end(rtti::context& ctx)
{
    if(!play_session_)
    {
        on_play_transition(ctx);
        return;
    }
    cancel_preparation(ctx);
    auto& current = ctx.get_cached<ecs>().get_scene();
    entt::handle restored_root;
    std::vector<entt::handle> restored_entities;
    const bool restored = play_checkpoint_ &&
        play_checkpoint_->map.ownership.rebind(*current.registry, restored_root, restored_entities);
    if(restored)
    {
        // The editor already restored the old graph. Discard runtime replacement
        // state without ever destroying the matching checkpoint hierarchy.
        if(!is_play_checkpoint(login_)) destroy_map(ctx, login_);
        if(previous_ && !is_play_checkpoint(*previous_)) destroy_map(ctx, *previous_);
        previous_.reset();
        login_ = std::move(play_checkpoint_->map);
        login_.scene_registry = current.registry.get();
        login_.scene_anchor = restored_root;
        login_.created_entities = std::move(restored_entities);
        attempted_content_root_ = std::move(play_checkpoint_->attempted_content_root);
        attempted_map_slug_ = std::move(play_checkpoint_->attempted_map_slug);
        map_start_error_ = std::move(play_checkpoint_->start_error);
        play_checkpoint_.reset();
        // Retiring a replacement restores external lights by UUID. Reapply the
        // accepted checkpoint's suppression, keeping its original flags for unload.
        for(const auto& original : login_.shared_entity_rollbacks)
        {
            auto entity = find_entity_by_uuid(*current.registry, original.first);
            if(entity.valid() && entity.all_of<transform_component>())
                entity.get<transform_component>().set_active(false);
        }
    }
    else if(play_checkpoint_)
    {
        // Player runtime has no editor graph restoration. Release the reserved
        // old resources and let normal identity checks retain the current map.
        auto checkpoint = std::move(play_checkpoint_);
        checkpoint->map.shared_entity_rollbacks.clear();
        destroy_map(ctx, checkpoint->map);
    }
    play_session_ = false;
    on_play_transition(ctx);
}
void apply_pw_login_camera_pose(entt::handle camera, const login_scene_config& config)
{
    apply_login_character_camera_pose(camera, config);
}
void pw_map_loader::destroy_map(rtti::context& ctx, login_loader& state)
{
    const bool retain_checkpoint_resources = is_play_checkpoint(state);
    auto& scene = ctx.get_cached<ecs>().get_scene();
    entt::handle current_root;
    std::vector<entt::handle> current_entities;
    const bool same_map = state.ownership.rebind(*scene.registry, current_root, current_entities, false);
    restore_pw_map_environment(*scene.registry, state.shared_entity_rollbacks);
    if(!same_map)
    {
        state.shared_entity_rollbacks.clear();
        state.created_entities.clear();
        state.scene_anchor = {};
    }
    else
    {
        for(auto& handle : current_entities)
        {
            if(handle.valid())
            {
                scene::destroy_entity(handle);
            }
        }
        state.created_entities.clear();
        if(current_root.valid())
        {
            scene::destroy_entity(current_root);
        }
        state.scene_anchor = {};
    }
    if(state.effects)
    {
        if(!retain_checkpoint_resources) state.effects->destroy(ctx);
        state.effects.reset();
    }
    state.ownership = {};
    if(retain_checkpoint_resources)
    {
        state.generated_mesh_keys.clear();
        state.generated_texture_keys.clear();
        state.scene_registry = nullptr;
        return;
    }

    // Runtime-created assets live in the manager independently of their scene
    // entities. Invalidate their typed handles and metadata only after every
    // entity/material reference has been released above.
    auto& am = ctx.get_cached<asset_manager>();
    for(const auto& key : state.generated_mesh_keys)
    {
        am.unload_asset<mesh>(key);
        am.remove_asset_info_for_key(key);
    }
    state.generated_mesh_keys.clear();
    for(const auto& key : state.generated_texture_keys)
    {
        am.unload_asset<gfx::texture>(key);
        am.remove_asset_info_for_key(key);
    }
    state.generated_texture_keys.clear();
    state.scene_registry = nullptr;
}

void pw_map_loader::cancel_preparation(rtti::context& ctx)
{
    preparation_generation_->store(++next_map_generation_);
    preparing_ = false;
    preparation_ = {};
    const auto& scene = ctx.get_cached<ecs>().get_scene();
    if(preparation_registry_ == scene.registry.get() && preparation_anchor_.valid())
        preparation_anchor_.destroy();
    preparation_anchor_ = {};
    preparation_registry_ = nullptr;
}

void pw_map_loader::despawn_loaded_map(rtti::context& ctx)
{
    cancel_preparation(ctx);
    destroy_map(ctx, login_);
    if(previous_)
    {
        destroy_map(ctx, *previous_);
        previous_.reset();
    }
}

void pw_map_loader::reset_login_loader_if_scene_changed(rtti::context& ctx, bool force_rebind)
{
    if(login_.scene_registry == nullptr)
    {
        return;
    }
    auto& scene = ctx.get_cached<ecs>().get_scene();
    entt::registry* current_registry = scene.registry.get();
    const auto rebind = [&](login_loader& state)
    {
        if(!force_rebind && state.scene_registry == current_registry && state.scene_anchor.valid())
        {
            const auto* id = state.scene_anchor.try_get<id_component>();
            const auto* tag = state.scene_anchor.try_get<tag_component>();
            if(id && tag && id->id == state.ownership.root_id && tag->tag == state.ownership.root_tag) return true;
        }
        if(!state.ownership.rebind(*current_registry, state.scene_anchor, state.created_entities)) return false;
        state.scene_registry = current_registry;
        return true;
    };
    const bool current_alive = rebind(login_);
    const bool previous_alive = previous_ && rebind(*previous_);
    if(current_alive && (!previous_ || previous_alive)) return;
    if(!current_alive && previous_alive)
    {
        fail_candidate(ctx, "candidate hierarchy removed while accepted map remains");
        return;
    }
    if(current_alive && previous_)
    {
        destroy_map(ctx, *previous_);
        previous_.reset();
        return;
    }
    despawn_loaded_map(ctx);
    login_ = {};
    attempted_content_root_.clear();
    attempted_map_slug_.clear();
    map_start_error_.clear();
}

void pw_map_loader::start_map_load(rtti::context& ctx,
                                   const std::string& content_root,
                                   const std::string& map_slug,
                                   uint32_t buildings_per_frame,
                                   bool restart, bool require_full)
{
    reset_login_loader_if_scene_changed(ctx);
    const std::string pending_root = attempted_content_root_;
    const std::string pending_map = attempted_map_slug_;
    attempted_content_root_ = content_root;
    attempted_map_slug_ = map_slug;
    map_start_error_.clear();
    try
    {
        if(!is_valid_map_slug(map_slug))
        {
            throw std::runtime_error("map must match [a-z0-9_]+");
        }
        const std::string normalized_root = normalize_content_root(content_root, map_slug);
        if(!restart && preparing_ && pending_root == normalized_root && pending_map == map_slug)
        {
            attempted_content_root_ = pending_root;
            return;
        }
        if(!restart && !preparing_ && (login_.active || login_.completed) &&
           login_.content_root == normalized_root && login_.map_slug == map_slug && login_.require_full == require_full)
        {
            attempted_content_root_.clear();
            attempted_map_slug_.clear();
            return;
        }
        // A replacement request discards only the unpublished candidate.
        if(login_.active)
        {
            destroy_map(ctx, login_);
            login_ = previous_ ? std::move(*previous_) : login_loader{};
            previous_.reset();
        }
        auto& scn = ctx.get_cached<ecs>().get_scene();
        if(preparation_registry_ == scn.registry.get() && preparation_anchor_.valid())
        {
            preparation_anchor_.destroy();
        }
        preparation_registry_ = scn.registry.get();
        preparation_anchor_ = entt::handle(*scn.registry, scn.registry->create());
        const uint64_t generation = ++next_map_generation_;
        preparation_generation_->store(generation);
        const auto generation_token = preparation_generation_;
        const auto preparation_mutex = preparation_mutex_;
        const fs::path physical_root = fs::resolve_protocol(normalized_root);
        attempted_content_root_ = normalized_root;
        preparing_ = true;
        preparation_ = ctx.get_cached<threader>().pool->schedule(
            "Prepare PW map " + map_slug,
            [normalized_root, physical_root, map_slug, buildings_per_frame, require_full,
             generation, generation_token, preparation_mutex]() -> login_loader
            {
                // Rapid restart requests may retire a job while its SDF is being
                // built. Serialize preparations so stale jobs cannot multiply RAM.
                std::lock_guard<std::mutex> lock(*preparation_mutex);
                const auto check_generation = [&]()
                {
                    if(generation_token->load() != generation)
                    {
                        throw std::runtime_error("map preparation superseded");
                    }
                };
                check_generation();
                login_loader next;
                next.content_root = normalized_root;
                next.map_slug = map_slug;
                next.generation = generation;
                next.require_full = require_full;
                next.buildings_per_frame = std::clamp(buildings_per_frame, 1u, 32u);
                pw_map_manifest_request request;
                request.content_root = physical_root;
                request.slug = map_slug;
                request.require_full = require_full;
                request.allow_legacy = !require_full;
                next.manifest = std::make_shared<pw_map_manifest_result>(validate_pw_map_manifest(request));
                if(!next.manifest->valid)
                {
                    throw std::runtime_error(next.manifest->error);
                }
                check_generation();
                if(next.manifest->scene_json.is_null())
                {
                    next.manifest->scene_json = read_json_asset(make_asset_key(normalized_root, "maps/" + map_slug + "/scene.eds.json"));
                }
                const auto& scene_doc = next.manifest->scene_json;
                next.lights = parse_map_lights(normalized_root, map_slug);
                next.terrain = load_login_terrain_heightfield(normalized_root, map_slug);
                next.terrain_albedo_refs = read_login_terrain_albedo_refs(normalized_root, map_slug);
                if(next.terrain_albedo_refs.empty()) throw std::runtime_error("map terrain has no usable albedo reference");
                if(!next.manifest->legacy)
                    next.terrain.stitch_block_grid = next.manifest->manifest_json.at("terrain").at("native").at("blockGrid").get<uint32_t>();
                auto parsed = make_login_buildings(normalized_root, map_slug, next.terrain, scene_doc);
                next.duplicate_references = parsed.duplicate_skipped;
                next.buildings = std::move(parsed.buildings);
                auto foliage = make_login_foliage(normalized_root, map_slug, next.terrain, scene_doc);
                if(foliage.skipped != 0)
                {
                    throw std::runtime_error("map contains unconverted required tree instances");
                }
                next.foliage = std::move(foliage.foliage);
                next.grass = make_map_mesh_instances(normalized_root, scene_doc, "Grass", require_full);
                next.ecmodels = make_map_mesh_instances(normalized_root, scene_doc, "ECModel", require_full);
                if(!next.manifest->legacy)
                {
                    auto effects = prepare_pw_map_effects(physical_root, scene_doc);
                    if(!effects.valid) throw std::runtime_error(effects.error);
                    next.effects = std::make_shared<pw_map_effects_runtime>();
                    next.effects->begin(std::move(effects), normalized_root, generation);
                }
                auto water = make_login_water(normalized_root, map_slug, scene_doc);
                next.water_duplicate_references = water.skipped;
                next.water = std::move(water.water);
                check_generation();
                next.prepared_terrain = std::make_shared<mesh>();
                if(!next.prepared_terrain->create_heightfield(gfx::mesh_vertex::get_layout(),
                       next.terrain.heights, next.terrain.width - 1, next.terrain.height - 1,
                       next.terrain.world_width * 0.5f, next.terrain.world_depth * 0.5f,
                       next.terrain.heightfield_mesh_scale(), mesh_create_origin::center, false, next.terrain.stitch_block_grid))
                {
                    throw std::runtime_error("map CPU terrain preparation failed");
                }
                check_generation();
                next.active = true;
                next.status = "loading";
                return next;
            });
    }
    catch(const std::exception& error)
    {
        preparing_ = false;
        map_start_error_ = error.what();
        if(!login_.completed)
        {
            login_.error = map_start_error_;
            login_.status = "error";
        }
    }
}

void pw_map_loader::install_prepared_map(rtti::context& ctx)
{
    if(!preparing_)
    {
        return;
    }
    auto& scn = ctx.get_cached<ecs>().get_scene();
    if(preparation_registry_ != scn.registry.get() || !preparation_anchor_.valid())
    {
        preparation_generation_->store(++next_map_generation_);
        preparing_ = false;
        preparation_ = {};
        preparation_anchor_ = {};
        preparation_registry_ = nullptr;
        attempted_content_root_.clear();
        attempted_map_slug_.clear();
        return;
    }
    if(!preparation_.valid() || !preparation_.is_ready())
    {
        return;
    }
    preparing_ = false;
    preparation_anchor_.destroy();
    preparation_anchor_ = {};
    try
    {
        login_loader next = preparation_.get();
        preparation_ = {};
        if(next.generation != preparation_generation_->load())
        {
            return;
        }
        if(login_.completed)
        {
            previous_ = std::make_unique<login_loader>(std::move(login_));
        }
        else
        {
            destroy_map(ctx, login_);
        }
        login_ = std::move(next);
        login_.resource_wait_started = std::chrono::steady_clock::now();
        login_.scene_registry = scn.registry.get();
        login_.scene_anchor = scene::create_entity(*scn.registry, "PW Map " + login_.map_slug);
        login_.ownership.root_tag = "pw-map:" + login_.map_slug + ":" + std::to_string(login_.generation);
        login_.ownership.root_id = login_.scene_anchor.get<id_component>().id;
        login_.scene_anchor.get<tag_component>().tag = login_.ownership.root_tag;
        login_.scene_anchor.get<transform_component>().set_active(false);
        attempted_content_root_.clear();
        attempted_map_slug_.clear();
    }
    catch(const std::exception& error)
    {
        preparation_ = {};
        if(previous_)
        {
            fail_candidate(ctx, error.what());
            return;
        }
        map_start_error_ = error.what();
        if(!login_.completed)
        {
            login_.error = map_start_error_;
            login_.status = "error";
        }
        APPLOG_ERROR("map preparation failed: {}", map_start_error_);
    }
}

void pw_map_loader::fail_candidate(rtti::context& ctx, const std::string& error)
{
    const std::string failed_root = login_.content_root;
    const std::string failed_map = login_.map_slug;
    destroy_map(ctx, login_);
    login_ = previous_ ? std::move(*previous_) : login_loader{};
    previous_.reset();
    attempted_content_root_ = failed_root;
    attempted_map_slug_ = failed_map;
    map_start_error_ = error;
    if(!login_.completed)
    {
        login_.content_root = failed_root;
        login_.map_slug = failed_map;
        login_.status = "error";
        login_.error = error;
    }
    APPLOG_ERROR("map candidate '{}' failed; accepted map retained: {}", failed_map, error);
}

void pw_map_loader::start_login_load(rtti::context& ctx,
                                  const std::string& content_root,
                                  uint32_t buildings_per_frame,
                                  bool restart)
{
    start_map_load(ctx, content_root, "login", buildings_per_frame, restart);
}

auto pw_map_loader::get_login_load_status() const -> login_load_status
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
    result.grass_total = static_cast<uint32_t>(login_.grass.size());
    result.grass_created = login_.grass_created;
    result.grass_blades = login_.manifest ? login_.manifest->grass_blade_count : 0;
    result.ecmodels_total = static_cast<uint32_t>(login_.ecmodels.size());
    result.ecmodels_created = login_.ecmodels_created;
    result.effects_total = login_.manifest ? static_cast<uint32_t>(login_.manifest->effect_ids.size()) : 0;
    if(login_.effects) result.ready_effect_ids = login_.effects->ready_ids();
    result.effects_created = static_cast<uint32_t>(result.ready_effect_ids.size());
    for(const auto& item : login_.grass)
        if(item.ready) result.ready_grass_ids.push_back(item.source_id);
    for(const auto& item : login_.ecmodels)
        if(item.ready) result.ready_ecmodel_ids.push_back(item.source_id);
    result.water_total = static_cast<uint32_t>(login_.water.size());
    result.water_created = login_.water_created;
    result.water_skipped = login_.water_skipped;
    result.terrain = login_.terrain_created;
    result.full = login_.require_full && login_.completed && login_.error.empty();
    result.generation = login_.generation;
    result.active_map = active_state().completed ? active_state().map_slug : std::string{};
    result.duplicate_references = login_.duplicate_references;
    result.water_duplicate_references = login_.water_duplicate_references;
    for(const auto& building : login_.buildings)
    {
        if(building.ready) result.ready_building_ids.push_back(building.source_id);
    }
    if(login_.water_created_flag)
    {
        for(const auto& water : login_.water) result.ready_water_ids.push_back(water.source_id);
    }
    result.current_index = login_.cursor;

    const auto pending_building = std::find_if(login_.buildings.begin(), login_.buildings.end(),
                                              [](const auto& item) { return !item.ready; });
    const auto pending_foliage = std::find_if(login_.foliage.begin(), login_.foliage.end(),
                                             [](const auto& item) { return !item.ready; });
    if(pending_building != login_.buildings.end())
    {
        result.current_index = static_cast<uint32_t>(pending_building - login_.buildings.begin());
        const auto& current = *pending_building;
        result.has_current = true;
        result.current_kind = "building";
        result.current_attempts = current.attempts;
        result.current_name = current.name;
        result.current_model = current.model;
        result.current_texture = current.texture;
        result.current_position = current.position;
    }
    else if(pending_foliage != login_.foliage.end())
    {
        const auto& current = *pending_foliage;
        result.has_current = true;
        result.current_index = static_cast<uint32_t>(pending_foliage - login_.foliage.begin());
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

    if(!map_start_error_.empty())
    {
        result.status = "error";
        result.error = map_start_error_;
        result.full = false;
    }
    if(preparing_)
    {
        result = login_load_status{};
        result.active_map = active_state().completed ? active_state().map_slug : std::string{};
        result.status = "preparing";
        result.content_root = attempted_content_root_;
        result.map = attempted_map_slug_;
        result.full = false;
    }
    return result;
}

auto pw_map_loader::active_state() const -> const login_loader&
{
    return previous_ ? *previous_ : login_;
}
auto pw_map_loader::generation() const -> uint64_t
{
    return active_state().completed ? active_state().generation : 0;
}
auto pw_map_loader::has_login_terrain() const -> bool
{
    return active_state().scene_registry != nullptr && active_state().completed && active_state().map_slug == "login" &&
           active_state().terrain.is_valid();
}

auto pw_map_loader::sample_login_terrain(float world_x, float world_z, float& out_height) const -> bool
{
    return active_state().terrain.sample_terrain_height(world_x, world_z, out_height);
}

auto pw_map_loader::get_login_terrain() const -> const terrain_heightfield&
{
    return active_state().terrain;
}

auto pw_map_loader::get_login_scene_config() const -> login_scene_config
{
    return parse_login_scene_config(active_state().content_root);
}

auto pw_map_loader::get_login_content_root() const -> const std::string&
{
    return active_state().content_root;
}

void pw_map_loader::service_login_loader(rtti::context& ctx)
{
    if(!login_.active || preparing_)
    {
        return;
    }
    const size_t old_entity_count = login_.created_entities.size();
    const auto parent_new_entities = [&]()
    {
        if(!login_.scene_anchor.valid()) return;
        for(size_t i = old_entity_count; i < login_.created_entities.size(); ++i)
        {
            auto entity = login_.created_entities[i];
            if(entity.valid() && entity.all_of<transform_component>())
            {
                if(!transform_component::is_parent_of(login_.scene_anchor, entity))
                    entity.get<transform_component>().set_parent(login_.scene_anchor, true);
                if(i >= login_.ownership.entity_ids.size())
                    login_.ownership.entity_ids.push_back(entity.get<id_component>().id);
            }
        }
    };
    try
    {
        const auto frame_start = std::chrono::steady_clock::now();
        if(frame_start - login_.resource_wait_started >= kMapAssetTimeout)
            throw std::runtime_error("map mandatory resources did not become usable before the deadline");
        login_.status = "loading";
        // The expensive CPU terrain/SDF work has already completed off-thread.
        // Upload and stage terrain before any delayed building resources.
        if(!login_.terrain_created)
        {
            login_.terrain_created = create_login_terrain(ctx, login_.content_root, login_.map_slug, login_.generation,
                                 login_.terrain, login_.prepared_terrain, login_.terrain_albedo_refs, login_.created_entities,
                                 login_.generated_mesh_keys, login_.generated_texture_keys);
            if(!login_.terrain_created) login_.status = "waiting_terrain_texture";
            parent_new_entities();
            return;
        }
        uint32_t created_this_frame = 0;
        const auto try_instances = [&](auto& instances, uint32_t& scan, uint32_t& created, auto create_instance)
        {
            const size_t maximum_scans = std::min<size_t>(instances.size(), 128);
            for(size_t inspected = 0; inspected < maximum_scans && created < instances.size(); ++inspected)
            {
                if(created_this_frame >= login_.buildings_per_frame ||
                   std::chrono::steady_clock::now() - frame_start >= std::chrono::milliseconds(4))
                {
                    break;
                }
                auto& item = instances[scan++ % instances.size()];
                if(item.ready)
                {
                    continue;
                }
                const auto now = std::chrono::steady_clock::now();
                if(item.waiting_since.time_since_epoch().count() == 0)
                {
                    item.waiting_since = now;
                }
                if(create_instance(ctx, login_.content_root, item, false, login_.created_entities))
                {
                    item.ready = true;
                    ++created;
                    ++created_this_frame;
                }
                else if(now - item.waiting_since >= kMapAssetTimeout)
                {
                    throw std::runtime_error("required asset timeout for " + item.source_id + ": " + item.model);
                }
            }
        };
        try_instances(login_.buildings, login_.building_scan, login_.created, create_login_building);
        try_instances(login_.foliage, login_.foliage_scan, login_.foliage_created, create_login_foliage);
        try_instances(login_.grass, login_.grass_scan, login_.grass_created, create_login_building);
        try_instances(login_.ecmodels, login_.ecmodels_scan, login_.ecmodels_created, create_login_building);
        login_.cursor = login_.created;
        login_.foliage_cursor = login_.foliage_created;
        parent_new_entities();
        if(login_.created != login_.buildings.size() || login_.foliage_created != login_.foliage.size() ||
           login_.grass_created != login_.grass.size() || login_.ecmodels_created != login_.ecmodels.size())
        {
            login_.status = "waiting_assets";
            return;
        }
        if(!login_.water_created_flag)
        {
            for(size_t i = 0; i < login_.water.size(); ++i)
            {
                const size_t before = login_.created_entities.size();
                if(!create_login_water_surface(ctx, login_.content_root, login_.map_slug, login_.generation,
                        login_.water[i], static_cast<uint32_t>(i), login_.created_entities, login_.generated_mesh_keys))
                {
                    throw std::runtime_error("required water surface failed: " + login_.water[i].source_id);
                }
                for(size_t e = before; e < login_.created_entities.size(); ++e)
                {
                    login_.created_entities[e].get<tag_component>().tag = login_.water[i].source_id;
                }
                ++login_.water_created;
            }
            login_.water_created_flag = true;
            parent_new_entities();
            return;
        }
        if(login_.manifest && !login_.manifest->legacy &&
           (login_.created != login_.manifest->building_ids.size() ||
            login_.water_created != login_.manifest->water_ids.size() ||
            login_.grass_created != login_.manifest->grass_ids.size() ||
            login_.ecmodels_created != login_.manifest->ecmodel_ids.size()))
        {
            throw std::runtime_error("created map instances do not match validated source identities");
        }
        if(login_.effects)
        {
            const auto staged = login_.effects->stage(ctx, login_.scene_anchor, login_.created_entities, login_.generated_mesh_keys);
            parent_new_entities();
            if(staged == pw_effect_stage::failed) throw std::runtime_error(login_.effects->error());
            if(staged == pw_effect_stage::waiting)
            {
                login_.status = "waiting_effects";
                return;
            }
            auto ready = login_.effects->ready_ids();
            std::sort(ready.begin(), ready.end());
            if(!login_.manifest || ready != login_.manifest->effect_ids)
                throw std::runtime_error("created effects do not match validated source identities");
            // Exercise the complete effect graph before retiring the accepted map.
            login_.effects->update(ctx, 0.0f);
            if(!login_.effects->error().empty()) throw std::runtime_error(login_.effects->error());
        }
        // Stage all allocations before retiring the accepted map.
        const auto sun = create_login_environment(ctx, login_.created_entities);
        create_login_lights(ctx, login_.map_slug, login_.lights, sun, login_.created_entities);
        if(login_.manifest && login_.manifest->legacy)
            create_login_effects(ctx, login_.content_root, login_.map_slug, login_.created_entities);
        if(login_.map_slug == "login" && fs::exists(fs::resolve_protocol(make_asset_key(login_.content_root, kLoginCharacterMeshRef))))
            create_login_character(ctx, login_.content_root, login_.created_entities);
        parent_new_entities();
        login_.environment_created = true;
        if(previous_)
        {
            // Carry the original external-scene state across map generations.
            login_.shared_entity_rollbacks = std::move(previous_->shared_entity_rollbacks);
            destroy_map(ctx, *previous_);
            previous_.reset();
        }
        else
        {
            suppress_external_map_environment(ctx, login_.created_entities, login_.shared_entity_rollbacks);
        }
        login_.scene_anchor.get<transform_component>().set_active(true);
        login_.active = false;
        login_.completed = true;
        login_.status = "done";
        if(login_.manifest) login_.manifest->scene_json = {};
        APPLOG_INFO("map '{}' done: buildings={} water={} duplicate_refs={} water_duplicate_refs={} generation={}",
                    login_.map_slug, login_.created, login_.water_created,
                    login_.duplicate_references, login_.water_duplicate_references, login_.generation);
    }
    catch(const std::exception& error)
    {
        parent_new_entities();
        fail_candidate(ctx, error.what());
    }
}

} // namespace unravel
