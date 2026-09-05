#include "../tests.h"
#include <engine/pw/pw_effect_model.h>
#include <engine/animation/animation.h>
#include <engine/rendering/mesh.h>
#include <graphics/vertex_decl.h>

#include <cmath>
#include <cstdio>
#include <limits>

using namespace unravel;
namespace
{
int checks = 0;
int failures = 0;
void check(bool condition, const char* message)
{
    ++checks;
    if(!condition) { ++failures; std::printf("  FAIL: %s\n", message); }
}
auto positions_close(const math::vec3& actual, const math::vec3& expected) -> bool
{
    return math::length(actual - expected) < 0.0001f;
}

auto make_skin(mesh& output) -> bool
{
    const auto layout = gfx::mesh_vertex::get_layout();
    std::vector<uint8_t> bytes(6 * layout.getStride());
    for(uint32_t i = 0; i < 6; ++i)
    {
        math::vec4 position(i % 3 == 1 ? 1.f : 0.f, i % 3 == 2 ? 1.f : 0.f, i < 3 ? 0.f : 1.f, 1);
        math::vec4 normal(0, 0, 1, 0);
        math::vec4 uv(i % 3 == 1 ? 1.f : 0.f, i % 3 == 2 ? 1.f : 0.f, 0, 0);
        gfx::vertex_pack(math::value_ptr(position), false, gfx::attribute::Position, layout, bytes.data(), i);
        gfx::vertex_pack(math::value_ptr(normal), true, gfx::attribute::Normal, layout, bytes.data(), i);
        gfx::vertex_pack(math::value_ptr(uv), false, gfx::attribute::TexCoord0, layout, bytes.data(), i);
    }
    mesh::triangle_array_t triangles(2);
    triangles[0].indices = {0, 1, 2}; triangles[0].data_group_id = 0;
    triangles[1].indices = {3, 4, 5}; triangles[1].data_group_id = 1;
    std::vector<mesh::submesh> submeshes(2);
    for(size_t i = 0; i < 2; ++i)
    {
        submeshes[i].data_group_id = static_cast<uint32_t>(i);
        submeshes[i].vertex_start = static_cast<int32_t>(i * 3);
        submeshes[i].vertex_count = 3;
        submeshes[i].face_start = static_cast<int32_t>(i);
        submeshes[i].face_count = 1;
        submeshes[i].skinned = true;
    }
    auto root = std::make_unique<mesh::armature_node>();
    root->name = "scene"; root->index = 0; root->submeshes = {0, 1};
    auto first = std::make_unique<mesh::armature_node>();
    first->name = "first"; first->index = 1; first->local_transform.set_position({2, 0, 0});
    auto second = std::make_unique<mesh::armature_node>();
    second->name = "second"; second->index = 2; second->local_transform.set_position({0, 3, 0});
    first->children.push_back(std::move(second));
    root->children.push_back(std::move(first));
    skin_bind_data skin;
    skin_bind_data::bone_influence bone_first;
    bone_first.bone_id = "first"; bone_first.bind_pose_transform.set_position({-2, 0, 0});
    for(uint32_t i = 3; i < 6; ++i) bone_first.influences.push_back({i, 1});
    skin.add_bone(bone_first);
    skin_bind_data::bone_influence bone_second;
    bone_second.bone_id = "second"; bone_second.bind_pose_transform.set_position({-2, -3, 0});
    for(uint32_t i = 0; i < 3; ++i) bone_second.influences.push_back({i, 1});
    skin.add_bone(bone_second);
    return output.prepare_mesh(layout) && output.set_vertex_source(std::move(bytes), 6, layout) &&
           output.set_primitives(std::move(triangles)) && output.set_submeshes(submeshes) &&
           output.bind_skin(skin) && output.bind_armature(root) && output.end_prepare(false, true, false, false);
}

auto make_action() -> animation_clip
{
    animation_clip clip;
    clip.name = "idle"; clip.duration = animation_clip::seconds_t(1.001f);
    animation_channel first;
    first.node_name = "first";
    first.position_keys = {{animation_channel::seconds_t(0), {2, 0, 0}}, {animation_channel::seconds_t(1), {4, 0, 0}}};
    first.position_keys.push_back({clip.duration, {4, 0, 0}});
    animation_channel second;
    second.node_name = "second";
    second.position_keys = {{animation_channel::seconds_t(0), {0, 3, 0}}, {animation_channel::seconds_t(1), {0, 5, 0}}};
    second.position_keys.push_back({clip.duration, {0, 5, 0}});
    clip.channels = {first, second};
    return clip;
}

auto run_pw_effect_model(rtti::context&) -> int
{
    checks = failures = 0;
    mesh source;
    check(make_skin(source), "two material CPU skin with reversed palette-to-global indices loads without GPU");
    pw_effect_model_geometry geometry;
    auto clip = make_action();
    const bool prepared = geometry.prepare(source, &clip, 2);
    check(prepared, "real CPU vertex/palette snapshot accepts authored action");
    if(!prepared) { std::printf("  reason: %s\n", geometry.error().c_str()); return failures; }
    check(geometry.batch_count() == 2, "source material slots retain independent batches");
    math::transform placement;
    placement.set_position({10, 0, 0});
    std::vector<pw_effect_model_batch> batches;
    check(geometry.append_triangles(batches, .5, 1, placement.get_matrix(), {1, .5, .25, .75}), "native action samples at half time");
    check(batches.size() == 2 && batches[0].triangles.size() == 3 && batches[1].triangles.size() == 3, "each material contains exactly its source triangle");
    if(batches.size() == 2 && batches[0].triangles.size() == 3 && batches[1].triangles.size() == 3)
    {
        check(positions_close(batches[0].triangles[0].position, {11, 1, 0}), "palette-local zero maps to second global bone with parent animation");
        check(positions_close(batches[1].triangles[0].position, {11, 0, 1}), "other material uses first bone and original position");
        check(std::abs(batches[0].triangles[1].uv.x - 1) < .0001f && std::abs(batches[0].triangles[0].color.a - .75f) < .0001f,
              "actual source UV and effect alpha survive skinning");
    }
    batches.clear();
    check(geometry.append_triangles(batches, 2.5, 2, placement.get_matrix(), {1, 1, 1, 1}) &&
          positions_close(batches[0].triangles[0].position, {12, 2, 0}), "finite authored loops retain final action pose");
    batches.clear();
    check(geometry.append_triangles(batches, 2.5, -1, placement.get_matrix(), {1, 1, 1, 1}) &&
          positions_close(batches[0].triangles[0].position, {10.996f, .996f, 0}), "infinite loops wrap inclusive native millisecond action period");
    check(geometry.append_triangles(batches, 0, 1, placement.get_matrix(), {1, 1, 1, 1}) && batches[0].triangles.size() == 6,
          "repeated source placements append without overwriting previous instance");
    check(!geometry.append_triangles(batches, std::numeric_limits<double>::quiet_NaN(), 1, placement.get_matrix(), {1, 1, 1, 1}),
          "non-finite timeline rejected before emitting geometry");
    batches.clear();
    check(geometry.append_triangles(batches, 1, -1, placement.get_matrix(), {1, 1, 1, 1}) &&
          positions_close(batches[0].triangles[0].position, {12, 2, 0}), "inclusive native end frame is not skipped at loop boundary");
    batches.clear();
    check(geometry.append_triangles(batches, 1.001, -1, placement.get_matrix(), {1, 1, 1, 1}) &&
          positions_close(batches[0].triangles[0].position, {10, 0, 0}), "next native millisecond starts the next loop");
    batches.clear();
    check(geometry.append_triangles(batches, .5, 0, placement.get_matrix(), {1, 1, 1, 1}) &&
          positions_close(batches[0].triangles[0].position, {10, 0, 0}), "native zero loop count leaves authored bind pose");
    check(geometry.prepare(source, nullptr, 2), "empty authored action uses native rest pose");
    batches.clear();
    check(geometry.append_triangles(batches, 999, -1, placement.get_matrix(), {1, 1, 1, 1}) &&
          positions_close(batches[0].triangles[0].position, {10, 0, 0}), "rest-pose inverse binds preserve original model geometry");
    clip.channels[1].node_name = "missing";
    check(!geometry.prepare(source, &clip, 2), "missing animation target fails instead of partial animation");
    check(!geometry.prepare(source, nullptr, 1), "unbound material slot fails instead of remapping texture");
    const uint32_t saved = source.get_system_ib()[0];
    source.get_system_ib()[0] = source.get_info().vertices;
    check(!geometry.prepare(source, nullptr, 2), "out-of-range actual CPU index rejected");
    source.get_system_ib()[0] = saved;
    std::printf("PW effect models: %d checks, %d failures\n", checks, failures);
    return failures;
}
} // namespace
REGISTER_TEST_SUITE("pw effect model / native skin / material batches", run_pw_effect_model)
