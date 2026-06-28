#include "mcp_system.h"

#include "mcp_commands.h"

#include "json.hpp"

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

#include <algorithm>
#include <cmath>
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

using json = nlohmann::json;

auto normalize_content_root(std::string root) -> std::string
{
    if(root.empty())
    {
        root = "app:/data/login";
    }

    while(!root.empty() && (root.back() == '/' || root.back() == '\\'))
    {
        root.pop_back();
    }

    if(root == "login:")
    {
        const auto login_data_root = std::string("login:/data/login");
        if(fs::has_known_protocol(login_data_root))
        {
            return login_data_root;
        }

        fs::error_code ec;
        const auto default_project_root = fs::path("C:/Temp/UnravelPW");
        if(fs::is_directory(default_project_root / "data" / "login", ec) && !ec)
        {
            fs::add_path_protocol("login", default_project_root);
            return login_data_root;
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
            if(canonical_root.filename() == "login" && data_dir.filename() == "data" && fs::is_directory(project_root, ec))
            {
                fs::add_path_protocol("login", project_root);
                return "login:/data/login";
            }

            fs::add_path_protocol("login", canonical_root);
            return "login:";
        }

        throw std::runtime_error(
            "load_login: content_root must use a known asset protocol or existing directory, got '" + root + "'");
    }

    return root;
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

auto read_login_terrain_albedo_refs(const std::string& content_root) -> std::vector<std::string>
{
    std::vector<std::string> refs;
    try
    {
        const auto layers_doc = read_json_asset(make_asset_key(content_root, "terrain/login/layers.json"));
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

auto load_login_terrain_heightfield(const std::string& content_root) -> terrain_heightfield
{
    const auto layers_doc = read_json_asset(make_asset_key(content_root, "terrain/login/layers.json"));
    const auto heightmap_doc = layers_doc.contains("heightmap") && layers_doc["heightmap"].is_object()
                                   ? layers_doc["heightmap"]
                                   : json::object();

    const auto heightmap_ref = heightmap_doc.value("path", std::string("terrain/login/height.r16.png"));

    terrain_heightfield terrain;
    terrain.height_min = heightmap_doc.value("heightMin", 134.59677124023438f);
    terrain.height_max = heightmap_doc.value("heightMax", 424.2374267578125f);
    terrain.world_width = layers_doc.contains("world") && layers_doc["world"].is_object()
                              ? layers_doc["world"].value("widthM", 1024.0f)
                              : 1024.0f;
    terrain.world_depth = layers_doc.contains("world") && layers_doc["world"].is_object()
                              ? layers_doc["world"].value("depthM", 1024.0f)
                              : 1024.0f;
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

auto create_login_terrain_debug_albedo(rtti::context& ctx, const terrain_heightfield& terrain) -> asset_handle<gfx::texture>
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
    return am.get_asset_from_instance<gfx::texture>("app:/generated/login_terrain_debug_albedo", texture);
}

auto load_login_terrain_albedo(rtti::context& ctx, const std::string& content_root) -> asset_handle<gfx::texture>
{
    try
    {
        const auto albedo_refs = read_login_terrain_albedo_refs(content_root);
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

auto make_login_buildings(const std::string& content_root, const terrain_heightfield& terrain) -> login_buildings_parse_result
{
    const auto scene_doc = read_json_asset(make_asset_key(content_root, "maps/login/scene.eds.json"));
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

auto login_scene_payload_asset_key(const std::string& content_root, std::string payload_ref) -> std::string
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
    return make_asset_key(content_root, "maps/login/" + payload_ref);
}

auto make_login_water(const std::string& content_root) -> login_water_parse_result
{
    const auto scene_doc = read_json_asset(make_asset_key(content_root, "maps/login/scene.eds.json"));

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

            const auto payload_doc = read_json_asset(login_scene_payload_asset_key(content_root, payload_ref));
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

auto make_login_foliage(const std::string& content_root, const terrain_heightfield& terrain) -> login_foliage_parse_result
{
    const auto scene_doc = read_json_asset(make_asset_key(content_root, "maps/login/scene.eds.json"));

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

            const auto payload_doc = read_json_asset(make_asset_key(content_root, "maps/login/" + payload_ref));
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

void create_login_terrain(rtti::context& ctx, const std::string& content_root, const terrain_heightfield& terrain)
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
    auto terrain_handle = am.get_asset_from_instance<mesh>("app:/generated/login_terrain", terrain_mesh);

    auto material_instance = std::make_shared<pbr_material>();
    material_instance->set_base_color({1.0f, 1.0f, 1.0f, 1.0f});
    material_instance->set_metalness(0.0f);
    material_instance->set_roughness(0.85f);
    material_instance->set_cull_type(cull_type::none);

    auto terrain_albedo = load_login_terrain_albedo(ctx, content_root);
    if(terrain_albedo.is_valid())
    {
        material_instance->set_color_map(terrain_albedo);
        APPLOG_INFO("load_login terrain albedo selected: imported");
    }
    else
    {
        terrain_albedo = create_login_terrain_debug_albedo(ctx, terrain);
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

    auto& scn = ctx.get_cached<ecs>().get_scene();
    auto entity = scene::create_entity(*scn.registry, "Login Terrain");
    entity.get<transform_component>().set_position_local({0.0f, terrain.heightfield_entity_y(), 0.0f});
    entity.emplace<model_component>().set_model(terrain_model);
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
                                const mcp_system::login_water& water,
                                uint32_t index) -> bool
{
    const auto payload_doc = read_json_asset(login_scene_payload_asset_key(content_root, water.payload));
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
    auto water_handle = am.get_asset_from_instance<mesh>("app:/generated/login_water_" + std::to_string(index), water_mesh);

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
    material_instance->set_base_color(login_water_color_from_argb(source_argb));
    material_instance->set_metalness(0.0f);
    material_instance->set_roughness(0.06f);
    material_instance->set_cull_type(cull_type::none);

    model water_model;
    water_model.set_lod(water_handle, 0);
    water_model.set_material_instance(material_instance, 0);

    auto& scn = ctx.get_cached<ecs>().get_scene();
    auto entity = scene::create_entity(*scn.registry, water.name.empty() ? std::string("Water ") + std::to_string(index) : water.name);
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

void create_login_environment(rtti::context& ctx)
{
    auto& scn = ctx.get_cached<ecs>().get_scene();

    auto volume = find_scene_entity_named(scn, "Volume");
    if(!volume)
    {
        volume = defaults::create_volume_entity(ctx, scn, "Volume", volume_mode::global);
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

        auto& transform = sun.get<transform_component>();
        transform.set_rotation_euler_local({50.0f, -30.0f, 0.0f});
    }

    if(sun)
    {
        if(auto* light_comp = sun.try_get<light_component>())
        {
            auto light = light_comp->get_light();
            light.intensity = 2.0f;
            light_comp->set_light(light);
        }

        auto& skylight = sun.get_or_emplace<skylight_component>();
        skylight.set_cloud_mode(skylight_component::cloud_mode::none);
        skylight.set_irradiance_intensity(0.08f);
    }

    if(!find_scene_entity_named(scn, "Reflection Probe Global"))
    {
        auto probe_entity = defaults::create_reflection_probe_entity(ctx, scn, probe_type::sphere, " Global");
        auto& reflection_comp = probe_entity.get_or_emplace<reflection_probe_component>();
        auto probe = reflection_comp.get_probe();
        probe.method = reflect_method::environment;
        probe.sphere_data.range = 1600.0f;
        reflection_comp.set_probe(probe);
    }
}

auto create_login_building(rtti::context& ctx,
                           const std::string& content_root,
                           mcp_system::login_building& building,
                           bool allow_untextured) -> bool
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
        if(building.alpha_test)
        {
            material_instance->set_alpha_test_value(0.5f);
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
                          bool allow_untextured) -> bool
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
        material_instance->set_roughness(0.85f);
        material_instance->set_cull_type(cull_type::none);
        material_instance->set_alpha_blend(foliage.alpha_blend);
        if(foliage.alpha_test)
        {
            material_instance->set_alpha_test_value(0.5f);
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

auto mcp_system::init(rtti::context& ctx) -> bool
{
    auto& ev = ctx.get_cached<events>();
    ev.on_frame_end.connect(sentinel_, -1000, this, &mcp_system::on_frame_end);

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
    clear_pending_readback();
    return true;
}

void mcp_system::on_frame_end(rtti::context& ctx, delta_t dt)
{
    (void)dt;
    service_login_loader(ctx);
    service_pending_screenshot(ctx);
    server_.Drain([&](const std::string& req)
    {
        return mcp_commands::dispatch(ctx, req, *this);
    });
}

auto mcp_system::ensure_camera(rtti::context& ctx) -> entt::handle
{
    auto& scn = ctx.get_cached<ecs>().get_scene();
    const bool camera_valid = mcp_cam_.valid() && mcp_cam_.all_of<transform_component, camera_component>();
    if(!camera_valid)
    {
        mcp_cam_ = defaults::create_camera_entity(ctx, scn, "MCP Camera");
    }

    return mcp_cam_;
}

void mcp_system::invalidate_camera()
{
    mcp_cam_ = {};
}

void mcp_system::request_screenshot(const std::string& path, uint32_t w, uint32_t h)
{
    clear_pending_readback();
    pending_.path = path;
    pending_.w = w;
    pending_.h = h;
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

void mcp_system::start_login_load(const std::string& content_root, uint32_t buildings_per_frame, bool restart)
{
    const auto normalized_root = normalize_content_root(content_root);
    if(!restart && (login_.active || login_.completed) && login_.content_root == normalized_root)
    {
        return;
    }
    if(!restart && login_.active && login_.content_root != normalized_root)
    {
        throw std::runtime_error("load_login: another content_root is already loading");
    }

    login_ = {};
    login_.content_root = normalized_root;
    login_.buildings_per_frame = std::clamp(buildings_per_frame, 1u, 8u);
    login_.terrain = load_login_terrain_heightfield(login_.content_root);
    auto parsed = make_login_buildings(login_.content_root, login_.terrain);
    login_.skipped = parsed.duplicate_skipped;
    login_.buildings = std::move(parsed.buildings);
    auto parsed_foliage = make_login_foliage(login_.content_root, login_.terrain);
    login_.foliage_skipped = parsed_foliage.skipped;
    login_.foliage = std::move(parsed_foliage.foliage);
    auto parsed_water = make_login_water(login_.content_root);
    login_.water_skipped = parsed_water.skipped;
    login_.water = std::move(parsed_water.water);
    login_.active = true;
    login_.completed = false;
    login_.status = "loading";
}

auto mcp_system::get_login_load_status() const -> login_load_status
{
    login_load_status result;
    result.status = login_.status;
    result.content_root = login_.content_root;
    result.error = login_.error;
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
    return login_.terrain.is_valid();
}

auto mcp_system::sample_login_terrain(float world_x, float world_z, float& out_height) const -> bool
{
    return login_.terrain.sample_terrain_height(world_x, world_z, out_height);
}

auto mcp_system::get_login_terrain() const -> const terrain_heightfield&
{
    return login_.terrain;
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
            create_login_environment(ctx);
            login_.environment_created = true;
        }

        while(login_.cursor < login_.buildings.size() && processed_this_frame < login_.buildings_per_frame)
        {
            auto& building = login_.buildings[login_.cursor];
            const bool wait_limit_reached = building.attempts >= kLoginMaxAssetWaitFrames;
            if(!create_login_building(ctx, login_.content_root, building, wait_limit_reached))
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

        while(login_.cursor >= login_.buildings.size() && login_.foliage_cursor < login_.foliage.size() &&
              processed_this_frame < login_.buildings_per_frame)
        {
            auto& foliage = login_.foliage[login_.foliage_cursor];
            const bool wait_limit_reached = foliage.attempts >= kLoginMaxAssetWaitFrames;
            if(!create_login_foliage(ctx, login_.content_root, foliage, wait_limit_reached))
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
                create_login_terrain(ctx, login_.content_root, login_.terrain);
                login_.terrain_created = true;
            }

            if(!login_.water_created_flag)
            {
                for(size_t i = 0; i < login_.water.size(); ++i)
                {
                    try
                    {
                        if(create_login_water_surface(ctx, login_.content_root, login_.water[i], static_cast<uint32_t>(i)))
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
        rpath.render_scene(camera, camera_comp, scn, kMcpFrameDt, false);

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
