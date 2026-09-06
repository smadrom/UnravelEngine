#include "../tests.h"
#include <engine/pw/pw_map_loader.h>
#include <engine/pw/pw_map_effects.h>
#include <engine/pw/pw_map_component.h>
#include <engine/meta/ecs/components/pw_map_component.hpp>
#include <engine/meta/ecs/entity.hpp>
#include <engine/ecs/ecs.h>
#include <engine/ecs/components/tag_component.h>
#include <engine/ecs/components/id_component.h>
#include <engine/ecs/components/transform_component.h>
#include <engine/rendering/mesh.h>
#include <engine/rendering/model.h>
#include <engine/rendering/ecs/components/model_component.h>
#include <engine/rendering/ecs/components/volume_component.h>
#include <engine/assets/asset_manager.h>
#include <filesystem/filesystem.h>
#include <graphics/graphics.h>
#include <graphics/vertex_decl.h>
#include <serialization/binary_archive.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

using namespace unravel;
namespace unravel
{
// Isolate publication/lifetime from asynchronous conversion and GPU allocation.
// The graph, serialized model asset reference and asset-manager retirement below
// are real; this accessor only seeds the accepted state that conversion produces.
struct pw_map_loader_test_access
{
    static void publish(pw_map_loader& loader, rtti::context& ctx, const std::string& slug,
                        uint64_t generation, const std::string& key, const hpp::uuid& external)
    {
        auto& scn = ctx.get_cached<ecs>().get_scene();
        auto& manager = ctx.get_cached<asset_manager>();
        auto geometry = std::make_shared<mesh>();
        geometry->create_heightfield(gfx::mesh_vertex::get_layout(), std::vector<float>(4, 0), 1, 1,
                                      .5f, .5f, -1, mesh_create_origin::center, false);
        auto asset = manager.get_asset_from_instance<mesh>(key, geometry);
        // Publication seeds an accepted map, so its trivial instance job must be complete.
        asset.get();
        pw_map_loader::login_loader next;
        next.map_slug = slug;
        next.content_root = "app:/pw-lifecycle-test";
        next.generation = generation;
        next.require_full = true;
        next.completed = true;
        next.status = "done";
        next.scene_registry = scn.registry.get();
        next.scene_anchor = scn.create_entity("PW checkpoint map " + slug);
        next.ownership.root_tag = "pw-map:" + slug + ":" + std::to_string(generation);
        next.ownership.root_id = next.scene_anchor.get<id_component>().id;
        next.scene_anchor.get<tag_component>().tag = next.ownership.root_tag;
        auto owner = scn.create_entity("PW checkpoint generated geometry");
        owner.get<tag_component>().tag = "ornament:00000001:00000002";
        owner.get<transform_component>().set_parent(next.scene_anchor, true);
        model instance;
        instance.set_lod(asset, 0);
        owner.emplace<model_component>().set_model(instance);
        next.created_entities.push_back(owner);
        next.ownership.entity_ids.push_back(owner.get<id_component>().id);
        next.generated_mesh_keys.push_back(key);
        next.created = next.cursor = 1;
        next.terrain_created = true;
        pw_map_loader::login_building building;
        building.source_id = owner.get<tag_component>().tag;
        building.ready = true;
        next.buildings.push_back(building);
        if(loader.login_.completed)
        {
            // Same ownership transfer and retirement as a successful real candidate.
            next.shared_entity_rollbacks = std::move(loader.login_.shared_entity_rollbacks);
            loader.destroy_map(ctx, loader.login_);
        }
        else if(!external.is_nil())
        {
            auto entity = scn.find_entity_by_uuid(external);
            next.shared_entity_rollbacks.emplace_back(external, entity.get<transform_component>().is_active());
            entity.get<transform_component>().set_active(false);
        }
        loader.login_ = std::move(next);
        loader.next_map_generation_ = std::max(loader.next_map_generation_, generation);
    }

    static auto root_id(const pw_map_loader& loader) -> hpp::uuid { return loader.login_.ownership.root_id; }
    static auto next_map_generation(const pw_map_loader& loader) -> uint64_t { return loader.next_map_generation_; }
    static auto lock_preparation(pw_map_loader& loader) -> std::unique_lock<std::mutex>
    {
        return std::unique_lock<std::mutex>(*loader.preparation_mutex_);
    }
    static void install_failed_effect(pw_map_loader& loader)
    {
        pw_effect_preparation rejected;
        rejected.error = "effect frame rejected";
        loader.login_.effects = std::make_shared<pw_map_effects_runtime>();
        loader.login_.effects->begin(std::move(rejected), loader.login_.content_root, loader.login_.generation);
    }
};
}
namespace
{
int checks = 0;
int failures = 0;
void check(bool ok, const char* message)
{
    ++checks;
    if(!ok)
    {
        ++failures;
        std::printf("  FAIL: %s\n", message);
    }
}
auto sample_mesh(mesh& geometry, float x, float z, float& height) -> bool
{
    const auto* indices = geometry.get_system_ib();
    const auto* vertices = geometry.get_system_vb();
    for(uint32_t triangle = 0; triangle < geometry.get_face_count(); ++triangle)
    {
        std::array<math::vec4, 3> p{};
        for(uint32_t i = 0; i < 3; ++i)
            gfx::vertex_unpack(math::value_ptr(p[i]), gfx::attribute::Position, geometry.get_vertex_format(),
                               vertices, indices[triangle * 3 + i]);
        const float det = (p[1].z - p[2].z) * (p[0].x - p[2].x) +
                          (p[2].x - p[1].x) * (p[0].z - p[2].z);
        if(std::abs(det) < 1e-8f) continue;
        const float a = ((p[1].z - p[2].z) * (x - p[2].x) + (p[2].x - p[1].x) * (z - p[2].z)) / det;
        const float b = ((p[2].z - p[0].z) * (x - p[2].x) + (p[0].x - p[2].x) * (z - p[2].z)) / det;
        const float c = 1.0f - a - b;
        if(a >= -1e-5f && b >= -1e-5f && c >= -1e-5f)
        {
            height = a * p[0].y + b * p[1].y + c * p[2].y;
            return true;
        }
    }
    return false;
}
void test_native_terrain()
{
    terrain_heightfield terrain;
    terrain.width = terrain.height = 5;
    terrain.world_width = terrain.world_depth = 4;
    terrain.height_min = 0;
    terrain.height_max = 1;
    terrain.stitch_block_grid = 2;
    for(uint32_t z = 0; z < 5; ++z)
        for(uint32_t x = 0; x < 5; ++x)
            terrain.heights.push_back(static_cast<float>(x * x + z * z * 3 + x * z * 2) / 100.0f);
    mesh geometry;
    const bool prepared = geometry.create_heightfield(gfx::mesh_vertex::get_layout(), terrain.heights, 4, 4, 2, 2, -1,
                                                       mesh_create_origin::center, false, terrain.stitch_block_grid);
    check(prepared, "native terrain prepares without a renderer");
    if(!prepared) return;
    for(uint32_t row = 0; row < 4; ++row)
    {
        for(uint32_t col = 0; col < 4; ++col)
        {
            for(const auto& uv : std::array<math::vec2, 3>{{{0.2f, 0.6f}, {0.8f, 0.3f}, {0.5f, 0.5f}}})
            {
                const float x = col + uv.x - 2;
                const float z = 2.0f - static_cast<float>(row) - uv.y;
                float sampled = 0;
                float rendered = 0;
                check(terrain.sample_terrain_height(x, z, sampled), "interior native height query succeeds");
                check(sample_mesh(geometry, x, z, rendered), "interior point lies on a rendered triangle");
                check(std::abs(sampled - rendered) < 1e-5f, "query and actual mesh agree inside every stitched cell");
            }
        }
    }
    // Independent native source oracle: TL=0, TR=.01, BL=.03, BR=.06.
    float sampled = 0;
    check(terrain.sample_terrain_height(-1.8f, 1.4f, sampled) && std::abs(sampled - .024f) < 1e-6f,
          "native main diagonal follows TL-BR and is not bilinear or anti-diagonal");
    // Block top-right is reversed: TL=.01, TR=.04, BL=.06, BR=.11.
    check(terrain.sample_terrain_height(-.8f, 1.4f, sampled) && std::abs(sampled - .046f) < 1e-6f,
          "native top-right stitching corner uses its authored reverse diagonal");
    check(!terrain.sample_terrain_height(2.01f, 0, sampled), "terrain rejects points outside its world bounds");
}

void test_material_slots()
{
    const std::string document = R"({"params":{"alphaBlend":"true","alphaTest":"true","twoSided":"true"},
      "materialSlots":[
        {"index":0,"diffuseTextureRef":"textures/leaves.ktx2","alphaTest":true,"alphaBlend":true,"twoSided":true,"alphaCutoff":0.25},
        {"index":1,"diffuseTextureRef":"textures/trunk.ktx2","alphaTest":false,"alphaBlend":false,"twoSided":false,"alphaCutoff":0.5},
        {"index":2,"diffuseTextureRef":"textures/glass.ktx2","alphaTest":false,"alphaBlend":true,"twoSided":false,"alphaCutoff":0.5}]})";
    const auto slots = parse_pw_map_material_slots(document);
    check(slots.size() == 3 && slots[0].texture != slots[1].texture, "independent material textures retain their slot indices");
    check(slots[0].alpha_test && !slots[0].alpha_blend && slots[0].two_sided && slots[0].alpha_cutoff == .25f,
          "cutout slot retains its cutoff and takes precedence over aggregate blend");
    check(!slots[1].alpha_test && !slots[1].alpha_blend && !slots[1].two_sided,
          "opaque trunk does not inherit leaf flags");
    check(!slots[2].alpha_test && slots[2].alpha_blend, "real translucent slot remains blended");
    const auto untextured = parse_pw_map_material_slots(R"({"textureRef":"textures/first.ktx2",
        "materialSlots":[{"index":0,"alphaTest":false,"alphaBlend":false,"alphaCutoff":0.5}]})");
    check(untextured[0].texture.empty(), "native untextured slot never inherits the first material texture");
    const auto grass = parse_pw_map_material_slots(document, 84.0f / 255.0f, true);
    check(grass[0].alpha_test && !grass[0].alpha_blend && grass[0].alpha_cutoff == 84.0f / 255.0f,
          "authored grass alpha reference overrides neutral material cutoff");
    bool rejected = false;
    try { parse_pw_map_material_slots(R"({"materialSlots":[{"index":0,"alphaTest":false}]})"); }
    catch(const std::exception&) { rejected = true; }
    check(rejected, "incomplete per-slot material flags reject instead of lossy aggregate fallback");
    rejected = false;
    try { parse_pw_map_material_slots(R"({"materialSlots":[{"index":0,"alphaBlend":false,"alphaCutoff":2}]})"); }
    catch(const std::exception&) { rejected = true; }
    check(rejected, "invalid material cutoff rejects before GPU material creation");
}
void test_cloned_map_ownership()
{
    scene authored("pw-loader-authored-test");
    auto anchor = authored.create_entity("Map a61");
    anchor.get<tag_component>().tag = "pw-map:a61:42";
    auto first = authored.create_entity("Grass one");
    auto second = authored.create_entity("Grass two at the same position");
    first.get<tag_component>().tag = "grass:00000001:00000002";
    second.get<tag_component>().tag = "grass:00000003:00000004";
    first.get<transform_component>().set_parent(anchor, true);
    second.get<transform_component>().set_parent(anchor, true);
    auto external_sun = authored.create_entity("External sun");
    auto already_disabled = authored.create_entity("Already disabled external volume");
    auto unrelated = authored.create_entity("Unrelated disabled entity");
    external_sun.get<transform_component>().set_active(false);
    already_disabled.get<transform_component>().set_active(false);
    unrelated.get<transform_component>().set_active(false);
    pw_map_ownership owned;
    owned.root_tag = anchor.get<tag_component>().tag;
    owned.root_id = anchor.get<id_component>().id;
    owned.entity_ids = {first.get<id_component>().id, second.get<id_component>().id};
    pw_map_environment_state original = {{external_sun.get<id_component>().id, true},
                                         {already_disabled.get<id_component>().id, false}};
    scene play("pw-loader-play-test");
    scene::clone_scene(authored, play, false);
    entt::handle rebound_root;
    std::vector<entt::handle> rebound;
    check(owned.rebind(*play.registry, rebound_root, rebound), "owned hierarchy rebinds through the actual Play scene serializer");
    check(rebound_root.registry() == play.registry.get() && rebound.size() == 2 &&
          rebound[0].registry() == play.registry.get() && rebound[1].registry() == play.registry.get(),
          "all rebound handles point only to the new registry");
    if(rebound.size() == 2)
    {
        check(rebound[0].get<tag_component>().tag == "grass:00000001:00000002" &&
              rebound[1].get<tag_component>().tag == "grass:00000003:00000004",
              "distinct authored source IDs survive cloning at identical positions");
    }
    scene restored("pw-loader-restored-test");
    scene::clone_scene(play, restored, false);
    play.unload();
    check(owned.rebind(*restored.registry, rebound_root, rebound) && rebound.size() == 2,
          "restoration rebinds after the previous registry's entities are destroyed");
    restore_pw_map_environment(*restored.registry, original);
    check(original.empty() && restored.find_entity_by_uuid(external_sun.get<id_component>().id).get<transform_component>().is_active(),
          "shared environment restores by UUID in the restored scene");
    check(!restored.find_entity_by_uuid(already_disabled.get<id_component>().id).get<transform_component>().is_active() &&
          !restored.find_entity_by_uuid(unrelated.get<id_component>().id).get<transform_component>().is_active(),
          "rollback preserves originally inactive and unrelated objects");
    auto wrong_generation = owned;
    wrong_generation.root_tag = "pw-map:a61:43";
    check(!wrong_generation.rebind(*restored.registry, rebound_root, rebound), "another loading generation cannot adopt this map");
    auto duplicate = restored.create_entity("Duplicate ownership tag");
    duplicate.get<tag_component>().tag = owned.root_tag;
    check(!owned.rebind(*restored.registry, rebound_root, rebound), "ambiguous ownership roots reject rebinding");
    scene::destroy_entity(duplicate);
    scene::destroy_entity(restored.find_entity_by_uuid(owned.entity_ids.front()));
    check(!owned.rebind(*restored.registry, rebound_root, rebound), "missing owned entity cannot remain a complete cloned map");
    check(owned.rebind(*restored.registry, rebound_root, rebound, false) && rebound.size() == 1,
          "cleanup can resolve the remaining owned hierarchy after an entity was removed");
    scene changed("pw-loader-unrelated-test");
    auto retained = changed.create_entity("Unrelated scene object");
    check(!owned.rebind(*changed.registry, rebound_root, rebound) && !rebound_root.valid() && rebound.empty() && retained.valid(),
          "unrelated scene cancels ownership without adopting or touching its entities");
}

void test_descriptor_marked_ownership()
{
    scene authored("pw-map-marked-ownership");
    auto descriptor = authored.create_entity("PW descriptor");
    const auto descriptor_id = descriptor.get<id_component>().id;
    auto anchor = authored.create_entity("PW generated anchor", descriptor);
    anchor.get<tag_component>().tag = "pw-map:a61:91";
    auto moved = authored.create_entity("Reparented generated geometry", anchor);
    auto retained = authored.create_entity("Other generated geometry", anchor);
    auto unrelated = authored.create_entity("Not a known map member");
    auto foreign = authored.create_entity("Other generation");
    for(auto entity : {anchor, moved, retained, unrelated, foreign})
    {
        auto& marker = entity.emplace<pw_map_generated_component>();
        marker.descriptor_id = descriptor_id;
        marker.generation = 91;
    }
    foreign.get<pw_map_generated_component>().generation = 92;
    pw_map_ownership owned;
    owned.root_id = anchor.get<id_component>().id;
    owned.root_tag = anchor.get<tag_component>().tag;
    owned.descriptor_id = descriptor_id;
    owned.generation = 91;
    owned.entity_ids = {moved.get<id_component>().id, retained.get<id_component>().id};
    const auto unrelated_id = unrelated.get<id_component>().id;
    const auto foreign_id = foreign.get<id_component>().id;
    moved.get<transform_component>().set_parent({}, true);
    scene cloned("pw-map-reparented-checkpoint");
    scene::clone_scene(authored, cloned, false);
    entt::handle rebound_root;
    std::vector<entt::handle> rebound;
    check(owned.rebind(*cloned.registry, rebound_root, rebound) && rebound.size() == 2 &&
          rebound[0].get<id_component>().id == owned.entity_ids[0],
          "known generated UUIDs with exact markers rebind after reparent and memory clone");
    auto changed = cloned.find_entity_by_uuid(owned.entity_ids[0]);
    changed.get<pw_map_generated_component>().generation = 92;
    check(!owned.rebind(*cloned.registry, rebound_root, rebound),
          "a known UUID with a different map generation cannot be adopted");
    check(owned.rebind(*cloned.registry, rebound_root, rebound, false) && rebound.size() == 1 &&
          rebound[0].get<id_component>().id == owned.entity_ids[1],
          "cleanup skips a known UUID whose generation marker no longer matches");
    changed.get<pw_map_generated_component>().generation = 91;
    changed.get<pw_map_generated_component>().descriptor_id = generate_uuid();
    check(!owned.rebind(*cloned.registry, rebound_root, rebound),
          "a known UUID with a different descriptor cannot be adopted");
    changed.get<pw_map_generated_component>().descriptor_id = descriptor_id;
    scene::destroy_entity(cloned.find_entity_by_uuid(owned.root_id));
    check(!owned.rebind(*cloned.registry, rebound_root, rebound),
          "strict Play rebind still requires its map anchor");
    check(owned.rebind(*cloned.registry, rebound_root, rebound, false) && !rebound_root.valid() &&
          rebound.size() == 1 && rebound[0].get<id_component>().id == owned.entity_ids[0],
          "cleanup finds the known moved member after its original map anchor was deleted");
    for(auto entity : rebound) scene::destroy_entity(entity);
    check(!cloned.find_entity_by_uuid(owned.entity_ids[0]) && cloned.find_entity_by_uuid(unrelated_id) &&
          cloned.find_entity_by_uuid(foreign_id),
          "cleanup never claims unknown UUIDs even with a matching marker, or another generation");
    auto legacy = owned;
    legacy.descriptor_id = {};
    check(!legacy.rebind(*authored.registry, rebound_root, rebound),
          "legacy ownership still requires its original ancestor relationship");
}

struct scene_file_fixture
{
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("unravel-pw-scene-" + hpp::to_string(generate_uuid()));
    std::filesystem::path associative = root / "descriptor.spfb";
    std::filesystem::path binary = root / "descriptor.bin";
    std::filesystem::path component_binary = root / "component.bin";
    scene_file_fixture()
    {
        std::filesystem::create_directory(root);
    }
    ~scene_file_fixture()
    {
        // Remove only the files this fixture owns; never recurse into a temp tree.
        std::error_code error;
        std::filesystem::remove(associative, error);
        std::filesystem::remove(binary, error);
        std::filesystem::remove(component_binary, error);
        std::filesystem::remove(root, error);
    }
};

auto read_file_bytes(const std::filesystem::path& path) -> std::string
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void check_descriptor_config(entt::handle entity, const pw_map_component& expected)
{
    const auto* actual = entity ? entity.try_get<pw_map_component>() : nullptr;
    check(actual != nullptr, "native scene restores its PW map descriptor");
    if(!actual) return;
    check(actual->content_root == expected.content_root && actual->map_slug == expected.map_slug &&
          actual->auto_load == expected.auto_load && actual->require_full == expected.require_full &&
          actual->buildings_per_frame == expected.buildings_per_frame,
          "all map configuration fields survive native scene serialization");
    check(actual->status == "idle" && actual->error.empty() && actual->runtime_instance_token == 0,
          "native deserialization resets status, error and runtime instance identity");
}

void test_scene_descriptor_persistence(rtti::context& ctx)
{
    scene_file_fixture files;
    scene authored("pw-map-descriptor-authored");
    auto descriptor = authored.create_entity("Author map descriptor");
    auto& config = descriptor.emplace<pw_map_component>();
    config.content_root = "app:/data/descriptor-fixture";
    config.map_slug = "a61";
    config.auto_load = false;
    config.require_full = false;
    config.buildings_per_frame = 11;
    config.status = "runtime-status-not-persisted";
    config.error = "runtime-error-not-persisted";
    config.runtime_instance_token = 12345;
    const auto descriptor_id = descriptor.get<id_component>().id;
    auto anchor = authored.create_entity("Generated map anchor", descriptor);
    auto& marker = anchor.emplace<pw_map_generated_component>();
    marker.descriptor_id = descriptor_id;
    marker.generation = 73;
    auto child = authored.create_entity("Generated map geometry", anchor);
    // Camera construction allocates GPU uniforms; keep this serialization fixture CPU-only.
    auto authored_child = authored.create_entity("Authored child", descriptor);
    const math::vec3 authored_position{3.0f, 8.0f, -11.0f};
    authored_child.get<transform_component>().set_position_local(authored_position);
    auto unrelated = authored.create_entity("Unrelated authored object");
    const auto anchor_id = anchor.get<id_component>().id;
    const auto child_id = child.get<id_component>().id;
    const auto authored_child_id = authored_child.get<id_component>().id;
    const auto unrelated_id = unrelated.get<id_component>().id;
    auto& manager = ctx.get_cached<asset_manager>();
    const std::string mesh_key = "app:/embedded/pw_descriptor_" + hpp::to_string(generate_uuid());
    auto geometry = std::make_shared<mesh>();
    geometry->create_heightfield(gfx::mesh_vertex::get_layout(), std::vector<float>(4, 0), 1, 1,
                                 .5f, .5f, -1, mesh_create_origin::center, false);
    auto generated_mesh = manager.get_asset_from_instance<mesh>(mesh_key, geometry);
    const auto generated_uid = generated_mesh.uid();
    check(generated_mesh.get() == geometry, "runtime mesh registration completes with the original CPU mesh");
    const auto by_key = manager.find_asset<mesh>(mesh_key);
    const auto by_uid = manager.get_asset<mesh>(generated_uid);
    check(by_key.uid() == generated_uid && by_uid.uid() == generated_uid &&
          by_key.id() == mesh_key && by_uid.id() == mesh_key &&
          by_key.get_if_ready() == geometry && by_uid.get_if_ready() == geometry,
          "extensionless embedded key and serialized UID resolve the same runtime mesh instance");
    model instance;
    instance.set_lod(generated_mesh, 0);
    child.emplace<model_component>().set_model(instance);
    check(!generated_mesh.uid().is_nil(), "generated scene fixture has a real mesh asset UID");
    const auto reflected = entt::resolve<pw_map_component>();
    const auto status_property = reflected.data(entt::hashed_string{"status"}.value());
    const auto error_property = reflected.data(entt::hashed_string{"error"}.value());
    check(status_property && status_property.is_const() && error_property && error_property.is_const(),
          "map status and error are read-only inspector properties");
    check(!reflected.data(entt::hashed_string{"runtime_instance_token"}.value()),
          "runtime map identity is not exposed as editable metadata");
    save_to_file(files.associative.string(), authored);
    save_to_file_bin(files.binary.string(), authored);
    for(const auto& path : {files.associative, files.binary})
    {
        const auto bytes = read_file_bytes(path);
        check(!bytes.empty(), "native scene file is written");
        check(bytes.find(hpp::to_string(descriptor_id)) != std::string::npos &&
              bytes.find(hpp::to_string(authored_child_id)) != std::string::npos &&
              bytes.find(hpp::to_string(unrelated_id)) != std::string::npos,
              "scene file retains descriptor, authored child and unrelated entity records");
        check(bytes.find(hpp::to_string(anchor_id)) == std::string::npos &&
              bytes.find(hpp::to_string(child_id)) == std::string::npos &&
              bytes.find(hpp::to_string(generated_mesh.uid())) == std::string::npos,
              "scene file excludes generated records, hierarchy links and generated mesh UID references");
        check(bytes.find(config.status) == std::string::npos && bytes.find(config.error) == std::string::npos,
              "runtime map feedback is absent from both native file formats");
        // Binary scene loading cannot represent absent optional components; the
        // existing ecs_serialization binary probe documents its desynchronization.
        // Exercise that format's file filter here, and fixed-field config below.
        if(path == files.binary) continue;
        scene restored("pw-map-descriptor-file-restored");
        load_from_file(path.string(), restored);
        const auto restored_descriptor = restored.find_entity_by_uuid(descriptor_id);
        const auto restored_child = restored.find_entity_by_uuid(authored_child_id);
        check_descriptor_config(restored_descriptor, config);
        check(restored_child && restored_child.all_of<transform_component>() &&
              restored_child.get<transform_component>().get_parent() == restored_descriptor &&
              math::distance(restored_child.get<transform_component>().get_position_local(), authored_position) < 1e-5f &&
              restored.find_entity_by_uuid(unrelated_id),
              "native scene load retains the authored child hierarchy, pose and unrelated object");
        check(!restored.find_entity_by_uuid(anchor_id) && !restored.find_entity_by_uuid(child_id),
              "native scene load cannot resurrect generated entities through transform links");
        if(restored_descriptor)
            check(restored_descriptor.get<transform_component>().get_children().size() == 1,
                  "restored descriptor has only its authored child");
    }
    {
        std::ofstream output(files.component_binary, std::ios::binary);
        ser20::oarchive_binary_t archive(output);
        archive(ser20::make_nvp("component", config));
    }
    scene binary_component_scene("pw-map-binary-component");
    auto binary_descriptor = binary_component_scene.create_entity("Binary descriptor");
    auto& binary_config = binary_descriptor.emplace<pw_map_component>();
    binary_config.status = "stale runtime status";
    binary_config.error = "stale runtime error";
    binary_config.runtime_instance_token = 98765;
    {
        std::ifstream input(files.component_binary, std::ios::binary);
        ser20::iarchive_binary_t archive(input);
        archive(ser20::make_nvp("component", binary_config));
    }
    check_descriptor_config(binary_descriptor, config);
    scene checkpoint("pw-map-descriptor-memory-checkpoint");
    scene::clone_scene(authored, checkpoint, false);
    const auto cloned_descriptor = checkpoint.find_entity_by_uuid(descriptor_id);
    const auto cloned_anchor = checkpoint.find_entity_by_uuid(anchor_id);
    const auto cloned_child = checkpoint.find_entity_by_uuid(child_id);
    check_descriptor_config(cloned_descriptor, config);
    check(cloned_anchor && cloned_child && checkpoint.find_entity_by_uuid(authored_child_id) &&
          checkpoint.find_entity_by_uuid(unrelated_id), "memory scene clone retains the complete authored and generated graph");
    if(cloned_anchor && cloned_child)
    {
        const auto* cloned_marker = cloned_anchor.try_get<pw_map_generated_component>();
        check(cloned_marker && cloned_marker->descriptor_id == descriptor_id && cloned_marker->generation == 73,
              "memory checkpoint retains generated-map descriptor identity and generation");
        check(cloned_anchor.get<transform_component>().get_parent() == cloned_descriptor &&
              cloned_child.get<transform_component>().get_parent() == cloned_anchor,
              "memory checkpoint preserves generated hierarchy relationships");
        const auto* cloned_model = cloned_child.try_get<model_component>();
        check(cloned_model && cloned_model->get_model().get_lod(0).uid() == generated_uid &&
              cloned_model->get_model().get_lod(0).get_if_ready() == geometry,
              "memory checkpoint preserves its generated mesh reference");
    }
    checkpoint.unload();
    authored.unload();
    manager.unload_asset<mesh>(mesh_key);
    manager.remove_asset_info_for_key(mesh_key);
    check(!manager.find_asset<mesh>(mesh_key).is_valid() &&
          manager.get_metadata(generated_uid).location.empty() &&
          !generated_mesh.get_if_ready() && !by_key.get_if_ready() && !by_uid.get_if_ready(),
          "runtime mesh unload removes metadata and invalidates every retained handle");
}

void test_play_replacement_checkpoint(rtti::context& ctx)
{
    auto& current = ctx.get_cached<ecs>().get_scene();
    auto& manager = ctx.get_cached<asset_manager>();
    scene original("pw-map-lifecycle-original");
    scene::clone_scene(current, original, false);
    current.unload();
    auto external = current.create_entity("External checkpoint environment");
    const auto external_id = external.get<id_component>().id;
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string old_key = "app:/embedded/pw_checkpoint_" + stamp + "_old";
    const std::string middle_key = "app:/embedded/pw_checkpoint_" + stamp + "_middle";
    const std::string new_key = "app:/embedded/pw_checkpoint_" + stamp + "_new";
    const auto usable = [&manager](const std::string& key)
    {
        return manager.find_asset<mesh>(key).get_if_ready() != nullptr;
    };
    pw_map_loader loader;
    pw_map_loader_test_access::publish(loader, ctx, "a61", 40, old_key, external_id);
    const auto checkpoint_root = pw_map_loader_test_access::root_id(loader);
    check(usable(old_key), "checkpoint owns a real generated asset-manager mesh");
    loader.on_play_before_begin(ctx);
    scene checkpoint("pw-map-editor-checkpoint");
    scene::clone_scene(current, checkpoint, false);
    scene::clone_scene(checkpoint, current, false);
    loader.on_play_transition(ctx);
    check(loader.generation() == 40 && loader.get_login_load_status().full, "Play clone retains accepted map before replacement");
    pw_map_loader_test_access::publish(loader, ctx, "login", 41, middle_key, external_id);
    check(loader.generation() == 41 && usable(old_key) && usable(middle_key),
          "successful Play replacement retains exactly the editor checkpoint assets");
    check(!current.find_entity_by_uuid(checkpoint_root), "retired Play clone entities are removed while assets remain reserved");
    pw_map_loader_test_access::publish(loader, ctx, "a61", 42, new_key, external_id);
    check(usable(old_key) && !usable(middle_key) && usable(new_key),
          "second Play replacement retires the intermediate map without archiving generations");
    scene::clone_scene(checkpoint, current, false);
    loader.on_play_after_end(ctx);
    const auto restored = loader.get_login_load_status();
    check(loader.generation() == 40 && restored.map == "a61" && restored.active_map == "a61" &&
          restored.status == "done" && restored.full && restored.created == 1,
          "Stop restores checkpoint map identity, complete status and accounting");
    check(current.find_entity_by_uuid(checkpoint_root).valid() && usable(old_key) && !usable(new_key),
          "Stop preserves restored generated geometry and removes runtime replacement assets");
    check(!current.find_entity_by_uuid(external_id).get<transform_component>().is_active(),
          "Stop retains checkpoint environment suppression after replacement rollback");
    pw_map_loader_test_access::install_failed_effect(loader);
    loader.on_frame_end(ctx, delta_t(0));
    const auto effect_failure = loader.get_login_load_status();
    check(effect_failure.status == "error" && !effect_failure.full && effect_failure.error == "effect frame rejected",
          "post-publication effect failure cannot remain a complete map");
    loader.deinit(ctx);
    check(!usable(old_key) && current.find_entity_by_uuid(external_id).get<transform_component>().is_active(),
          "final checkpoint unload releases reserved assets and restores original external state");
    scene::clone_scene(original, current, false);
}

void test_play_without_editor_restore(rtti::context& ctx)
{
    auto& current = ctx.get_cached<ecs>().get_scene();
    auto& manager = ctx.get_cached<asset_manager>();
    scene original("pw-map-runtime-original");
    scene::clone_scene(current, original, false);
    current.unload();
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string old_key = "app:/embedded/pw_runtime_checkpoint_" + stamp + "_old";
    const std::string new_key = "app:/embedded/pw_runtime_checkpoint_" + stamp + "_new";
    pw_map_loader loader;
    pw_map_loader_test_access::publish(loader, ctx, "a61", 60, old_key, {});
    loader.on_play_before_begin(ctx);
    pw_map_loader_test_access::publish(loader, ctx, "login", 61, new_key, {});
    // No editor: on_play_after_end has no restored graph to adopt.
    loader.on_play_after_end(ctx);
    check(loader.generation() == 61 && loader.get_login_load_status().map == "login" &&
          manager.find_asset<mesh>(old_key).get_if_ready() == nullptr &&
          manager.find_asset<mesh>(new_key).get_if_ready() != nullptr,
          "player Stop keeps its live map and releases an unused editor checkpoint reservation");
    loader.deinit(ctx);
    // Beginning Play from an empty editor scene must not leave a runtime-only
    // loaded map or its generated assets behind when that empty scene returns.
    current.unload();
    scene empty_checkpoint("pw-map-empty-checkpoint");
    scene::clone_scene(current, empty_checkpoint, false);
    const std::string runtime_only_key = "app:/embedded/pw_runtime_checkpoint_" + stamp + "_runtime_only";
    loader.on_play_before_begin(ctx);
    pw_map_loader_test_access::publish(loader, ctx, "a61", 62, runtime_only_key, {});
    scene::clone_scene(empty_checkpoint, current, false);
    loader.on_play_after_end(ctx);
    check(loader.generation() == 0 && loader.get_login_load_status().status == "idle" &&
          manager.find_asset<mesh>(runtime_only_key).get_if_ready() == nullptr,
          "restoring an empty editor checkpoint removes runtime-only maps and assets");
    loader.deinit(ctx);
    scene::clone_scene(original, current, false);
}

void pump_until_settled(pw_map_loader& loader, rtti::context& ctx)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(loader.get_login_load_status().status == "preparing" && std::chrono::steady_clock::now() < deadline)
    {
        loader.on_frame_end(ctx, delta_t(0));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void test_descriptor_adopts_loaded_map(rtti::context& ctx)
{
    auto& current = ctx.get_cached<ecs>().get_scene();
    auto& manager = ctx.get_cached<asset_manager>();
    scene original("pw-map-adoption-original");
    scene::clone_scene(current, original, false);
    current.unload();
    auto descriptor = current.create_entity("Accepted map descriptor");
    auto& config = descriptor.emplace<pw_map_component>();
    config.content_root = "app:/pw-lifecycle-test";
    config.map_slug = "a61";
    const auto descriptor_id = descriptor.get<id_component>().id;
    auto external_volume = current.create_entity("Authored external global volume");
    external_volume.emplace<volume_component>().mode = volume_mode::global;
    const auto external_id = external_volume.get<id_component>().id;
    auto inactive = current.create_entity("Authored disabled environment");
    inactive.get<transform_component>().set_active(false);
    const auto inactive_id = inactive.get<id_component>().id;
    const std::string mesh_key = "app:/embedded/pw_adoption_" + hpp::to_string(generate_uuid());
    pw_map_loader loader;
    pw_map_loader_test_access::publish(loader, ctx, "a61", 81, mesh_key, external_id);
    const auto anchor_id = pw_map_loader_test_access::root_id(loader);
    loader.on_frame_end(ctx, delta_t(0));
    check(loader.generation() == 81 && pw_map_loader_test_access::next_map_generation(loader) == 81 &&
          config.status == "done", "descriptor adopts its already accepted map without preparing a duplicate");
    auto anchor = current.find_entity_by_uuid(anchor_id);
    const auto* marker = anchor ? anchor.try_get<pw_map_generated_component>() : nullptr;
    check(marker && marker->descriptor_id == descriptor_id && marker->generation == 81 &&
          anchor.get<transform_component>().get_parent() == descriptor,
          "adoption attaches the ownership marker and parents the accepted map to its descriptor");
    check(marker && marker->authored_environment_active == pw_map_environment_state{{external_id, true}},
          "descriptor marker retains the authored active flag of the suppressed global volume");
    const auto generated = anchor.get<transform_component>().get_children().at(0);
    const auto generated_id = generated.get<id_component>().id;
    const auto generated_asset_id = generated.get<model_component>().get_model().get_lod(0).uid();
    const auto* owned_marker = generated.try_get<pw_map_generated_component>();
    check(owned_marker && owned_marker->descriptor_id == descriptor_id && owned_marker->generation == 81,
          "descriptor adoption marks every generated entity independently of its parent");
    scene_file_fixture files;
    const auto check_saved_environment = [&]()
    {
        auto detached = current.find_entity_by_uuid(generated_id);
        detached.get<transform_component>().set_parent({}, true);
        save_to_file(files.associative.string(), current);
        const auto bytes = read_file_bytes(files.associative);
        check(bytes.find(hpp::to_string(generated_id)) == std::string::npos &&
              bytes.find(hpp::to_string(generated_asset_id)) == std::string::npos,
              "disk save excludes a generated node and asset reference after reparenting outside its map");
        check(!current.find_entity_by_uuid(external_id).get<transform_component>().is_active(),
              "disk save never changes the live map's environment suppression");
        scene reopened("pw-map-authored-environment-restored");
        load_from_file(files.associative.string(), reopened);
        check(!reopened.find_entity_by_uuid(generated_id),
              "scene reopen cannot duplicate a generated entity moved outside the map anchor");
        const auto saved_volume = reopened.find_entity_by_uuid(external_id);
        const auto saved_inactive = reopened.find_entity_by_uuid(inactive_id);
        check(saved_volume && saved_volume.all_of<volume_component>() &&
              saved_volume.get<transform_component>().is_active(),
              "scene reopen restores the global volume's authored active flag instead of temporary map suppression");
        check(saved_inactive && !saved_inactive.get<transform_component>().is_active(),
              "scene save preserves unrelated authored inactive flags");
    };
    check_saved_environment();
    loader.on_play_before_begin(ctx);
    const auto checkpoint_generation = pw_map_loader_test_access::next_map_generation(loader);
    scene checkpoint("pw-map-adoption-checkpoint");
    scene::clone_scene(current, checkpoint, false);
    const auto checkpoint_light = checkpoint.find_entity_by_uuid(external_id);
    const auto checkpoint_anchor = checkpoint.find_entity_by_uuid(anchor_id);
    check(checkpoint_light && !checkpoint_light.get<transform_component>().is_active() &&
          checkpoint_anchor && checkpoint_anchor.get<pw_map_generated_component>().authored_environment_active ==
              pw_map_environment_state{{external_id, true}},
          "Play checkpoint keeps runtime suppression and the original authored flag separately");
    scene::clone_scene(checkpoint, current, false);
    loader.on_play_transition(ctx);
    loader.on_frame_end(ctx, delta_t(0));
    check(loader.generation() == 81 && pw_map_loader_test_access::next_map_generation(loader) == checkpoint_generation &&
          loader.get_login_load_status().full, "Play clone adopts the checkpoint map without a second preparation");
    scene::clone_scene(checkpoint, current, false);
    loader.on_play_after_end(ctx);
    loader.on_frame_end(ctx, delta_t(0));
    anchor = current.find_entity_by_uuid(anchor_id);
    marker = anchor ? anchor.try_get<pw_map_generated_component>() : nullptr;
    check(loader.generation() == 81 && marker && marker->descriptor_id == descriptor_id &&
          marker->generation == 81 && manager.find_asset<mesh>(mesh_key).get_if_ready(),
          "Stop restores the accepted descriptor ownership and retained generated mesh");
    check_saved_environment();
    scene::destroy_entity(current.find_entity_by_uuid(anchor_id));
    auto restored_descriptor = current.find_entity_by_uuid(descriptor_id);
    restored_descriptor.get<pw_map_component>().auto_load = false;
    loader.on_frame_end(ctx, delta_t(0));
    check(current.find_entity_by_uuid(external_id).get<transform_component>().is_active(),
          "disabling the descriptor after Stop restores the authored external global volume");
    check(!current.find_entity_by_uuid(generated_id) && !manager.find_asset<mesh>(mesh_key).get_if_ready(),
          "descriptor cleanup removes its reparented generated node and asset after the anchor was deleted");
    loader.deinit(ctx);
    scene::clone_scene(original, current, false);
}

void test_descriptor_auto_load(rtti::context& ctx)
{
    auto& current = ctx.get_cached<ecs>().get_scene();
    scene original("pw-map-descriptor-auto-original");
    scene::clone_scene(current, original, false);
    current.unload();
    auto descriptor = current.create_entity("Automatic PW map descriptor");
    const std::string missing_root = "app:/missing-pw-descriptor-" + hpp::to_string(generate_uuid());
    check(!fs::exists(fs::resolve_protocol(missing_root)), "automatic loading fixture has no manifest or content directory");
    auto& config = descriptor.emplace<pw_map_component>();
    config.content_root = missing_root;
    config.map_slug = "a61";
    config.auto_load = false;
    pw_map_loader loader;
    loader.on_frame_end(ctx, delta_t(0));
    const auto disabled_generation = pw_map_loader_test_access::next_map_generation(loader);
    check(loader.get_login_load_status().status == "idle", "disabled map descriptor does not start preparation");
    config.auto_load = true;
    loader.on_frame_end(ctx, delta_t(0));
    check(pw_map_loader_test_access::next_map_generation(loader) > disabled_generation &&
          config.runtime_instance_token != 0, "enabled descriptor starts its first real asynchronous map request");
    pump_until_settled(loader, ctx);
    loader.on_frame_end(ctx, delta_t(0));
    const auto first_generation = pw_map_loader_test_access::next_map_generation(loader);
    const auto first_token = config.runtime_instance_token;
    check(loader.get_login_load_status().status == "error" && config.status == "error" && !config.error.empty(),
          "missing descriptor manifest reports the actual asynchronous failure in the inspector");
    for(int frame = 0; frame < 4; ++frame) loader.on_frame_end(ctx, delta_t(0));
    check(pw_map_loader_test_access::next_map_generation(loader) == first_generation,
          "an unchanged failed descriptor does not retry every frame");
    config.map_slug = "login";
    loader.on_frame_end(ctx, delta_t(0));
    check(pw_map_loader_test_access::next_map_generation(loader) > first_generation,
          "changing descriptor configuration starts a new request after failure");
    pump_until_settled(loader, ctx);
    loader.on_frame_end(ctx, delta_t(0));
    const auto changed_generation = pw_map_loader_test_access::next_map_generation(loader);
    check(loader.get_login_load_status().attempted_map == "login" && config.status == "error",
          "configuration retry reports the newly requested map");
    descriptor.remove<pw_map_component>();
    auto& replacement = descriptor.emplace<pw_map_component>();
    replacement.content_root = missing_root;
    replacement.map_slug = "login";
    check(replacement.runtime_instance_token == 0, "re-added descriptor starts with a fresh runtime identity");
    loader.on_frame_end(ctx, delta_t(0));
    check(replacement.runtime_instance_token != 0 && replacement.runtime_instance_token != first_token &&
          pw_map_loader_test_access::next_map_generation(loader) > changed_generation,
          "remove and re-add on the same entity retries even when configuration is unchanged");
    pump_until_settled(loader, ctx);
    loader.on_frame_end(ctx, delta_t(0));
    const auto replacement_generation = pw_map_loader_test_access::next_map_generation(loader);
    check(replacement.status == "error" && !replacement.error.empty(), "replacement descriptor receives its own preparation failure");
    loader.on_frame_end(ctx, delta_t(0));
    check(pw_map_loader_test_access::next_map_generation(loader) == replacement_generation,
          "replacement descriptor also attempts unchanged configuration only once");
    {
        auto preparation_lock = pw_map_loader_test_access::lock_preparation(loader);
        replacement.map_slug = "a61";
        loader.on_frame_end(ctx, delta_t(0));
        check(loader.get_login_load_status().status == "preparing", "scene-unload fixture holds a real preparation in flight");
        current.unload();
    }
    loader.on_frame_end(ctx, delta_t(0));
    pump_until_settled(loader, ctx);
    check(loader.get_login_load_status().status == "idle" && loader.generation() == 0 &&
          current.registry->view<id_component>().size() == 0,
          "unloading the descriptor scene cancels pending preparation without publishing entities");
    loader.deinit(ctx);
    scene::clone_scene(original, current, false);
}

void test_rejected_and_cancelled_preparation(rtti::context& ctx)
{
    auto& scene = ctx.get_cached<ecs>().get_scene();
    auto unrelated = scene.create_entity("PW loader test external entity");
    pw_map_loader loader;
    loader.start_map_load(ctx, "app:/missing-map-fixture", "../a61", 3, true);
    check(loader.get_login_load_status().status == "error", "unsafe slug fails synchronously");
    check(unrelated.valid(), "invalid request preserves external scene objects");
    loader.start_map_load(ctx, "app:/missing-map-first", "login", 3, true);
    check(loader.get_login_load_status().status == "preparing", "source validation is asynchronous");
    loader.start_map_load(ctx, "app:/missing-map-latest", "a61", 3, true);
    loader.start_map_load(ctx, "app:/missing-map-latest", "a61", 3, false);
    check(loader.get_login_load_status().map == "a61", "idempotent request retains the pending map identity");
    pump_until_settled(loader, ctx);
    const auto failed = loader.get_login_load_status();
    check(failed.status == "error" && !failed.start_error.empty(), "missing manifest reports actual asynchronous failure");
    check(failed.attempted_map == "a61", "superseded preparation cannot overwrite the newest request");
    check(!failed.full && failed.active_map.empty() && loader.generation() == 0, "failed map cannot be reported as accepted");
    check(unrelated.valid(), "preparation failure never removes external entities");
    loader.start_map_load(ctx, "app:/missing-map-cancelled", "login", 3, true);
    loader.deinit(ctx);
    loader.on_frame_end(ctx, delta_t(0));
    check(loader.get_login_load_status().status == "idle", "deinit cancels in-flight publication");
    check(unrelated.valid(), "deinit removes only map-owned objects");
    unrelated.destroy();
}
}
auto run_pw_map_loader_suite(rtti::context& ctx) -> int
{
    checks = failures = 0;
    test_material_slots();
    test_native_terrain();
    test_cloned_map_ownership();
    test_descriptor_marked_ownership();
    test_scene_descriptor_persistence(ctx);
    test_play_replacement_checkpoint(ctx);
    test_play_without_editor_restore(ctx);
    test_descriptor_adopts_loaded_map(ctx);
    test_descriptor_auto_load(ctx);
    test_rejected_and_cancelled_preparation(ctx);
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures;
}
REGISTER_TEST_SUITE("PW map runtime / terrain topology / cancellation", run_pw_map_loader_suite)
