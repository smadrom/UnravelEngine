#include "../tests.h"

#include <engine/assets/asset_manager.h>
#include <engine/assets/impl/asset_reader.h>
#include <engine/assets/impl/importers/mesh_importer.h>
#include <engine/ecs/prefab.h>
#include <engine/meta/animation/animation.hpp>
#include <engine/meta/rendering/mesh.hpp>
#include <engine/pw/detail/json.hpp>
#include <engine/threading/threader.h>
#include <graphics/index_buffer.h>
#include <graphics/vertex_buffer.h>
#include <uuid/uuid.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <functional>
#include <map>
#include <set>
#include <thread>

using namespace unravel;

namespace
{
int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if(!condition)
    {
        ++g_failures;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

template<typename T>
auto wait_for_completion(const asset_handle<T>& handle) -> bool
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        handle.get_if_ready();
        if(handle.is_ready())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while(std::chrono::steady_clock::now() < deadline);
    return false;
}

void test_nonblocking_handle(tpp::thread_pool& pool)
{
    asset_handle<prefab> handle;
    check(!handle.get_if_ready(), "invalid handle has no ready data");
    std::promise<void> release;
    const auto gate = release.get_future().share();
    auto expected = std::make_shared<prefab>();
    expected->buffer.data = {'r', 'e', 'a', 'd', 'y'};
    handle.set_internal_job(pool.create_job("asset_readiness_deferred", [gate, expected]()
    {
        gate.wait_for(std::chrono::seconds(2));
        return expected;
    }).share());
    check(handle.is_deferred(), "fixture starts deferred");
    check(!handle.get_if_ready(), "pending task does not return the empty fallback as ready data");
    check(!handle.is_deferred(), "nonblocking access submits the deferred task");
    release.set_value();
    check(wait_for_completion(handle), "deferred task completes");
    check(handle.get_if_ready() == expected, "completed task returns its actual data");
    auto retained_handle = handle;
    handle.set_internal_job(pool.schedule("asset_readiness_failed", []() -> std::shared_ptr<prefab>
    {
        return nullptr;
    }).share());
    check(wait_for_completion(handle), "failed task still completes");
    check(handle && handle.is_ready(), "task validity and completion do not imply usable data");
    check(!retained_handle.get_if_ready(), "failed reload clears the old cached data on retained handles");
    handle.set_internal_job(pool.create_job("asset_readiness_recover", [expected] { return expected; }).share());
    check(wait_for_completion(retained_handle), "retained handle submits a replacement deferred task");
    check(retained_handle.get_if_ready() == expected, "retained handle recovers when loading succeeds");
}

void write_u32(std::ostream& out, uint32_t value)
{
    const std::array<char, 4> bytes = {static_cast<char>(value), static_cast<char>(value >> 8),
                                     static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
    out.write(bytes.data(), bytes.size());
}

// Two asymmetric triangles and two otherwise identical materials reveal both coordinate and slot changes.
void write_gltf_fixture(const fs::path& path, const std::string& generator, const std::string& usage = {})
{
    std::string json = R"({"asset":{"version":"2.0","generator":"GENERATOR"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"buffers":[{"byteLength":84}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":72,"target":34962},{"buffer":0,"byteOffset":72,"byteLength":12,"target":34963}],"accessors":[{"bufferView":0,"componentType":5126,"count":6,"type":"VEC3","min":[1,0,2],"max":[8,4,11]},{"bufferView":1,"byteOffset":0,"componentType":5123,"count":3,"type":"SCALAR"},{"bufferView":1,"byteOffset":6,"componentType":5123,"count":3,"type":"SCALAR"}],"materials":[{},{}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0},{"attributes":{"POSITION":0},"indices":2,"material":1}]}]})";
    json.replace(json.find("GENERATOR"), std::string("GENERATOR").size(), generator);
    if(!usage.empty())
    {
        const std::string scene = "\"scenes\":[{\"nodes\":[0]}]";
        json.replace(json.find(scene), scene.size(), "\"scenes\":[{\"nodes\":[0],\"extras\":{\"pwMeshUsage\":\"" + usage + "\"}}]");
    }
    while(json.size() % 4 != 0)
    {
        json.push_back(' ');
    }
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    write_u32(out, 0x46546c67);
    write_u32(out, 2);
    write_u32(out, static_cast<uint32_t>(12 + 8 + json.size() + 8 + 84));
    write_u32(out, static_cast<uint32_t>(json.size()));
    write_u32(out, 0x4e4f534a);
    out.write(json.data(), json.size());
    write_u32(out, 84);
    write_u32(out, 0x004e4942);
    const std::array<float, 18> positions = {1, 0, 2, 3, 0, 2, 1, 4, 5, 5, 1, 7, 8, 1, 7, 5, 3, 11};
    for(const float value : positions)
    {
        write_u32(out, std::bit_cast<uint32_t>(value));
    }
    for(uint16_t index = 0; index < 6; ++index)
    {
        out.put(static_cast<char>(index));
        out.put(0);
    }
}

void test_failed_mesh_load(tpp::thread_pool& pool, const std::string& prefix)
{
    const std::string key = prefix + "/uncompiled.glb";
    write_gltf_fixture(fs::resolve_protocol(key), "A3DMapEditor EDS converter");
    asset_handle<mesh> handle;
    check(!asset_reader::load_from_file(pool, handle, key), "raw glTF is not read as compiled binary mesh");
    check(!handle.get_if_ready(), "uncompiled glTF cannot produce a ready mesh");
    const auto compiled = asset_reader::resolve_compiled_path<mesh>(key);
    fs::create_directories(compiled.parent_path());
    mesh::load_data empty_mesh;
    empty_mesh.vertex_format = gfx::mesh_vertex::get_layout();
    save_to_file_bin(compiled.string(), empty_mesh);
    check(asset_reader::load_from_file(pool, handle, key, load_mode::deferred), "empty compiled fixture schedules a load");
    check(wait_for_completion(handle), "empty compiled fixture finishes loading");
    check(!handle.get_if_ready(), "mesh::load_mesh rejection does not publish an empty mesh");
    {
        std::ofstream truncated(compiled, std::ios::binary | std::ios::trunc);
    }
    check(asset_reader::load_from_file(pool, handle, key), "truncated compiled fixture schedules a load");
    check(wait_for_completion(handle), "truncated compiled fixture finishes loading");
    check(!handle.get_if_ready(), "failed binary reading does not publish an empty mesh");
}

void test_animation_waits_for_compiled_cache(tpp::thread_pool& pool, const std::string& prefix)
{
    const std::string key = prefix + "/authored_idle.anim";
    const auto source = fs::resolve_protocol(key);
    animation_clip expected;
    expected.name = "authored_idle";
    expected.duration = animation_clip::seconds_t(1.25f);
    animation_channel channel;
    channel.node_name = "root";
    channel.position_keys = {{animation_channel::seconds_t(0), {1, 2, 3}}, {expected.duration, {4, 5, 6}}};
    channel.rotation_keys = {{animation_channel::seconds_t(0), math::identity<math::quat>()}};
    channel.scaling_keys = {{animation_channel::seconds_t(0), {1, 1, 1}}};
    expected.channels.push_back(channel);
    fs::create_directories(source.parent_path());
    save_to_file(source.string(), expected);
    animation_clip source_roundtrip;
    load_from_file(source.string(), source_roundtrip);
    check(source_roundtrip.name == expected.name && source_roundtrip.duration == expected.duration &&
          source_roundtrip.channels == expected.channels, "source animation is a valid nonempty associative archive");
    const auto compiled = asset_reader::resolve_compiled_path<animation_clip>(key);
    check(!fs::exists(compiled), "source animation starts without its compiled cache");
    asset_handle<animation_clip> handle;
    check(!asset_reader::load_from_file(pool, handle, key), "JSON animation is not accepted as binary before compilation");
    check(!handle.get_if_ready(), "JSON source cannot publish an empty ready animation");
    check(asset_reader::load_from_file(pool, handle, key, load_mode::deferred), "uncompiled animation can schedule a deferred load");
    check(wait_for_completion(handle), "uncompiled animation deferred attempt completes");
    check(!handle.get_if_ready(), "deferred JSON source does not publish an empty ready animation");
    const auto retained_handle = handle;
    fs::create_directories(compiled.parent_path());
    save_to_file_bin(compiled.string(), expected);
    check(asset_reader::load_from_file(pool, handle, key, load_mode::deferred), "compiled animation schedules a replacement load");
    check(wait_for_completion(retained_handle), "retained animation handle resolves after binary compilation");
    const auto ready = retained_handle.get_if_ready();
    check(ready && ready->name == expected.name && ready->duration == expected.duration &&
          ready->channels == expected.channels, "compiled animation returns the same authored name, duration and channels");
}

void test_invalid_skin_vertex_rejected()
{
    constexpr uint32_t vertex_count = 3;
    for(const uint32_t invalid_vertex : {vertex_count, vertex_count + 1})
    {
        mesh::load_data data;
        data.vertex_format = gfx::mesh_vertex::get_layout();
        data.vertex_count = vertex_count;
        data.vertex_data.resize(vertex_count * data.vertex_format.getStride());
        skin_bind_data::bone_influence bone;
        bone.bone_id = "root";
        bone.influences = {{invalid_vertex, 1.0f}};
        data.skin_data.add_bone(bone);
        const auto original_vertices = data.vertex_data;
        check(!mesh::apply_skin_to_load_data(data), "skin influence at or beyond vertex_count is rejected: " + std::to_string(invalid_vertex));
        check(!data.skin_is_prepared && data.vertex_count == vertex_count && data.vertex_data == original_vertices &&
              data.bone_palette_bones.empty(), "invalid skin does not partially prepare or overwrite geometry");
    }
}

auto import_fixture(asset_manager& am, const fs::path& path, mesh::load_data& data,
                    importer::source_mesh_policy* source_policy = nullptr) -> bool
{
    mesh_importer_meta settings;
    settings.model.optimize_meshes = false;
    std::vector<animation_clip> animations;
    std::vector<importer::imported_material> materials;
    std::vector<importer::imported_texture> textures;
    return importer::load_mesh_data_from_file(am, path, settings, data, animations, materials, textures, source_policy);
}

auto material_slots(const mesh::load_data& data) -> std::set<uint32_t>
{
    std::set<uint32_t> slots;
    for(const auto& submesh : data.submeshes)
    {
        slots.insert(submesh.data_group_id);
    }
    return slots;
}

void test_pw_gltf_convention(asset_manager& am, const fs::path& source_root)
{
    const auto pw_path = source_root / "relocated" / "a61" / "pw.glb";
    const auto login_path = source_root / "data" / "login" / "pw.glb";
    const auto control_path = source_root / "data" / "login" / "ordinary.glb";
    write_gltf_fixture(pw_path, "A3DMapEditor EDS converter");
    write_gltf_fixture(login_path, "A3DMapEditor EDS converter");
    write_gltf_fixture(control_path, "ordinary glTF exporter");
    mesh::load_data pw;
    mesh::load_data login;
    mesh::load_data control;
    const bool pw_loaded = import_fixture(am, pw_path, pw);
    const bool login_loaded = import_fixture(am, login_path, login);
    const bool control_loaded = import_fixture(am, control_path, control);
    check(pw_loaded && login_loaded && control_loaded, "PW and ordinary glTF fixtures import without a renderer");
    if(!pw_loaded || !login_loaded || !control_loaded)
    {
        return;
    }
    check(pw.bbox.min.x > 0 && pw.bbox.min.z > 0, "relocated PW mesh preserves its asymmetric coordinate frame");
    check(pw.vertex_data == login.vertex_data && pw.triangle_data.size() == login.triangle_data.size(),
          "PW mesh geometry is independent of the Login folder");
    check(pw.bbox.min == login.bbox.min && pw.bbox.max == login.bbox.max,
          "PW mesh bounds are independent of the Login folder");
    check(material_slots(pw) == std::set<uint32_t>{0, 1}, "PW glTF preserves both original material slots");
    check(control.bbox.max.x < 0, "ordinary glTF in Login still receives the ordinary facing correction");
    check(material_slots(control).size() == 1, "ordinary glTF still merges redundant materials");
    const auto grass_path = source_root / "relocated" / "authored-grass.glb";
    write_gltf_fixture(grass_path, "A3DMapEditor EDS converter", "grass");
    importer::source_mesh_policy grass_policy;
    mesh::load_data grass;
    check(import_fixture(am, grass_path, grass, &grass_policy), "explicit PW grass imports real mesh geometry");
    check(!grass_policy.generate_sdf && !grass_policy.generate_lods, "grass source prevents opaque SDF and destructive generic LOD baking");
    write_gltf_fixture(grass_path, "A3DMapEditor EDS converter", "ecmodel");
    check(import_fixture(am, grass_path, grass, &grass_policy), "explicit PW animated source imports");
    check(!grass_policy.generate_sdf && !grass_policy.generate_lods, "animated source prevents a stale bind-pose field and generic LOD baking");
    write_gltf_fixture(grass_path, "ordinary glTF exporter", "grass");
    check(import_fixture(am, grass_path, grass, &grass_policy), "ordinary asset with a similarly named extra still imports");
    check(grass_policy.generate_sdf && grass_policy.generate_lods, "PW grass policy never changes an ordinary exporter");
}

void test_external_map_meshes(asset_manager& am)
{
    const char* root = std::getenv("PW_MAP_MESH_TEST_ROOT");
    if(root == nullptr || root[0] == '\0') return;
    const char* slug = std::getenv("PW_MAP_TEST_SLUG");
    const fs::path content_root(root);
    const fs::path manifest_path = content_root / "maps" / (slug && slug[0] ? slug : "a61") / "map.manifest.json";
    std::ifstream stream(manifest_path);
    const auto manifest = nlohmann::json::parse(stream, nullptr, false);
    check(manifest.is_object(), "external mesh fixture has a readable manifest");
    if(!manifest.is_object()) return;
    // The output inventory includes root effects and recursively exported GFX
    // children. Walk every hash-bound effect document, then import each actual
    // model once while requiring all references to agree on its contract.
    nlohmann::json gfx_entries = nlohmann::json::array();
    std::map<std::string, nlohmann::json> gfx_models;
    for(const auto& output : manifest.at("outputs"))
    {
        if(output.value("kind", std::string{}) != "effect") continue;
        const std::string path = output.at("path").get<std::string>();
        std::ifstream effect_stream(content_root / path);
        const auto effect = nlohmann::json::parse(effect_stream, nullptr, false);
        check(effect.is_object() && effect.contains("dependencies") && effect.at("dependencies").is_array(),
              "actual root/nested GFX has readable dependency inventory: " + path);
        if(!effect.is_object() || !effect.contains("dependencies") || !effect.at("dependencies").is_array()) continue;
        for(const auto& dependency : effect.at("dependencies"))
        {
            if(dependency.value("kind", std::string{}) != "model") continue;
            nlohmann::json entry = {{"model", dependency.at("modelRef")}, {"material", dependency.at("materialRef")},
                {"jointCount", dependency.at("jointCount")}, {"frameCount", dependency.at("frameCount")},
                {"actionNameUtf8", dependency.at("actionNameUtf8")}};
            if(dependency.contains("animationName")) entry["animationName"] = dependency.at("animationName");
            if(dependency.contains("animationDurationSeconds")) entry["animationDurationSeconds"] = dependency.at("animationDurationSeconds");
            const std::string model = entry.at("model").get<std::string>();
            const auto inserted = gfx_models.emplace(model, entry);
            check(inserted.second || inserted.first->second == entry, "repeated GFX model dependencies agree: " + model);
        }
    }
    for(const auto& record : gfx_models) gfx_entries.push_back(record.second);
    std::set<std::string> imported;
    size_t imported_gfx = 0;
    for(const std::string section : {"grass", "ecmodels", "gfx"})
    {
        if(section != "gfx")
        {
            check(manifest.contains(section) && manifest.at(section).contains("entries"), "external mesh fixture includes " + section);
            if(!manifest.contains(section) || !manifest.at(section).contains("entries")) continue;
        }
        const auto& entries = section == "gfx" ? gfx_entries : manifest.at(section).at("entries");
        for(const auto& entry : entries)
        {
            const std::string relative = entry.value("model", std::string{});
            if(!imported.insert(relative).second) continue;
            mesh::load_data data;
            mesh_importer_meta settings;
            std::vector<animation_clip> animations;
            std::vector<importer::imported_material> materials;
            std::vector<importer::imported_texture> textures;
            importer::source_mesh_policy policy;
            const bool loaded = importer::load_mesh_data_from_file(am, content_root / relative, settings, data,
                                                                   animations, materials, textures, &policy);
            check(loaded, "actual PW mesh imports: " + relative);
            if(!loaded) continue;
            if(section == "gfx") ++imported_gfx;
            check(!data.vertex_data.empty() && !data.triangle_data.empty() && !data.submeshes.empty(),
                  "actual PW mesh retains usable geometry: " + relative);
            check(!policy.generate_sdf && !policy.generate_lods, "actual PW mesh carries the authored compiler policy: " + relative);
            if(section == "grass") continue;
            check(data.skin_data.has_bones() && data.root_node != nullptr, "actual " + section + " retains its skin and armature: " + relative);
            std::ifstream material_stream(content_root / entry.at("material").get<std::string>());
            const auto material = nlohmann::json::parse(material_stream, nullptr, false);
            check(material.is_object() && material.contains("materialSlots") && material.at("materialSlots").is_array(),
                  "actual skinned model has source material slots: " + relative);
            if(material.is_object() && material.contains("materialSlots") && material.at("materialSlots").is_array())
            {
                std::set<uint32_t> expected_slots;
                for(const auto& slot : material.at("materialSlots")) expected_slots.insert(slot.at("index").get<uint32_t>());
                check(material_slots(data) == expected_slots, "Assimp retains exact native material indices: " + relative);
            }
            std::set<std::string> nodes;
            std::function<void(const mesh::armature_node&)> visit = [&](const auto& node)
            {
                nodes.insert(node.name);
                for(const auto& child : node.children) if(child) visit(*child);
            };
            if(data.root_node) visit(*data.root_node);
            bool valid_skin = true;
            for(const auto& bone : data.skin_data.get_bones())
            {
                valid_skin = valid_skin && nodes.count(bone.bone_id) != 0;
                for(const auto& influence : bone.influences)
                    valid_skin = valid_skin && influence.vertex_index < data.vertex_count &&
                                 std::isfinite(influence.weight) && influence.weight >= 0 && influence.weight <= 1;
            }
            check(valid_skin, "Assimp skin targets existing armature nodes and bounded vertices: " + relative);
            if(valid_skin && data.skin_data.has_bones())
            {
                const uint32_t imported_vertices = data.vertex_count;
                const uint32_t imported_triangles = data.triangle_count;
                const bool prepared = mesh::apply_skin_to_load_data(data);
                check(prepared && data.skin_is_prepared, "actual PW skin completes the compiler preparation stage: " + relative);
                if(prepared)
                {
                    check(data.vertex_count >= imported_vertices && data.triangle_count == imported_triangles &&
                          data.triangle_data.size() == imported_triangles &&
                          data.vertex_data.size() == size_t(data.vertex_count) * data.vertex_format.getStride(),
                          "prepared PW skin retains its complete geometry and vertex buffer: " + relative);
                    bool valid_indices = true;
                    for(const auto& triangle : data.triangle_data)
                        for(const uint32_t index : triangle.indices) valid_indices = valid_indices && index < data.vertex_count;
                    check(valid_indices && data.bone_palette_bones.size() == data.submeshes.size(),
                          "prepared PW skin has bounded triangles and a palette for every submesh: " + relative);
                }
                std::printf("PW skin preparation: %s vertices %u -> %u, triangles %u, prepared %d\n",
                            relative.c_str(), imported_vertices, data.vertex_count, imported_triangles, prepared ? 1 : 0);
            }
            if(section == "gfx")
            {
                check(data.skin_data.get_bones().size() <= entry.at("jointCount").get<size_t>(),
                      "actual GFX retained skin fits native joint inventory: " + relative);
                if(entry.at("actionNameUtf8").get<std::string>().empty())
                {
                    check(entry.at("frameCount") == 0 && animations.empty(),
                          "authored GFX rest-pose model needs no invented animation clip: " + relative);
                    continue;
                }
            }
            const auto clip = std::find_if(animations.begin(), animations.end(), [&](const auto& value)
            {
                return value.name == fs::path(relative).stem().string() + "_" + entry.value("animationName", std::string{});
            });
            check(clip != animations.end(), "actual " + section + " retains the imported authored animation name: " + relative);
            if(clip == animations.end()) continue;
            check(std::isfinite(clip->duration.count()) && clip->duration.count() > 0 &&
                  std::abs(clip->duration.count() - entry.value("animationDurationSeconds", 0.0f)) < 0.002f,
                  "actual " + section + " retains the source animation duration: " + relative);
            check(!clip->channels.empty(), "actual " + section + " retains animation channels: " + relative);
            for(const auto& channel : clip->channels)
                check(nodes.count(channel.node_name) != 0 && !channel.position_keys.empty() &&
                      !channel.rotation_keys.empty() && !channel.scaling_keys.empty(),
                      "actual animated channel retains armature target and TRS: " + relative + "/" + channel.node_name);
        }
    }
    check(imported_gfx == gfx_models.size(), "all unique root/nested GFX dependency models imported");
    std::printf("PW external mesh imports: %zu unique models, GFX dependency models %zu/%zu\n",
                imported.size(), imported_gfx, gfx_models.size());
}

void test_cpu_terrain_preparation()
{
    mesh terrain;
    check(!terrain.upload_gpu_buffers(), "unprepared mesh is rejected before any GPU access");
    const std::array<float, 9> heights = {0, 1, 0, 1, 2, 1, 0, 1, 0};
    const bool prepared = terrain.create_heightfield(gfx::mesh_vertex::get_layout(),
                                                    hpp::span<const float>(heights.data(), heights.size()),
                                                    2, 2, 2, 2, 1, mesh_create_origin::center, false);
    check(prepared && terrain.get_status() == mesh_status::prepared, "terrain prepares without a renderer");
    if(!prepared)
    {
        return;
    }
    check(terrain.get_vertex_count() == 9 && terrain.get_face_count() == 8,
          "CPU terrain retains its complete geometry for later upload");
    check(terrain.get_system_vb() && terrain.get_system_ib(), "CPU terrain owns both source buffers");
    check((!terrain.get_hardware_vb() || !terrain.get_hardware_vb()->is_valid()) &&
          (!terrain.get_hardware_ib() || !terrain.get_hardware_ib()->is_valid()),
          "CPU terrain preparation does not allocate GPU buffers");
    check(terrain.get_sdf_count() > 0 && terrain.get_sdf(0).is_valid(), "CPU terrain retains the normal SDF bake");
}
} // namespace

auto run_asset_readiness_suite(rtti::context& ctx) -> int
{
    g_checks = 0;
    g_failures = 0;
    auto& pool = *ctx.get_cached<threader>().pool;
    auto& am = ctx.get_cached<asset_manager>();
    const std::string prefix = "app:/data/asset-readiness-" + hpp::to_string(generate_uuid());
    const auto source_root = fs::resolve_protocol(prefix);
    const auto compiled_root = asset_reader::resolve_compiled_path<mesh>(prefix + "/fixture.glb").parent_path();
    test_nonblocking_handle(pool);
    test_failed_mesh_load(pool, prefix);
    test_animation_waits_for_compiled_cache(pool, prefix);
    test_invalid_skin_vertex_rejected();
    test_pw_gltf_convention(am, source_root);
    test_cpu_terrain_preparation();
    test_external_map_meshes(am);
    am.unload_group(prefix);
    fs::error_code ec;
    fs::remove_all(source_root, ec);
    fs::remove_all(compiled_root, ec);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}

REGISTER_TEST_SUITE("asset readiness / PW glTF convention", run_asset_readiness_suite)
