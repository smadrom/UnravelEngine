#include "../tests.h"
#include <engine/pw/pw_map_loader.h>
#include <engine/pw/pw_map_effects.h>
#include <engine/ecs/ecs.h>
#include <engine/ecs/components/tag_component.h>
#include <engine/ecs/components/id_component.h>
#include <engine/ecs/components/transform_component.h>
#include <engine/rendering/mesh.h>
#include <engine/rendering/model.h>
#include <engine/rendering/ecs/components/model_component.h>
#include <engine/assets/asset_manager.h>
#include <graphics/graphics.h>
#include <graphics/vertex_decl.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
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
                const float z = 2 - row - uv.y;
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
    const std::string old_key = "app:/generated/pw_checkpoint_" + stamp + "_old";
    const std::string middle_key = "app:/generated/pw_checkpoint_" + stamp + "_middle";
    const std::string new_key = "app:/generated/pw_checkpoint_" + stamp + "_new";
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
    const std::string old_key = "app:/generated/pw_runtime_checkpoint_" + stamp + "_old";
    const std::string new_key = "app:/generated/pw_runtime_checkpoint_" + stamp + "_new";
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
    const std::string runtime_only_key = "app:/generated/pw_runtime_checkpoint_" + stamp + "_runtime_only";
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
    test_play_replacement_checkpoint(ctx);
    test_play_without_editor_restore(ctx);
    test_rejected_and_cancelled_preparation(ctx);
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures;
}
REGISTER_TEST_SUITE("PW map runtime / terrain topology / cancellation", run_pw_map_loader_suite)
