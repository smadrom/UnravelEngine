#include "pw_effect_model.h"
#include "pw_map_effects.h"

#include <engine/animation/animation.h>
#include <engine/assets/asset_manager.h>
#include <engine/rendering/mesh.h>
#include <graphics/index_buffer.h>
#include <graphics/vertex_buffer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <type_traits>

namespace unravel
{
namespace
{
using json = nlohmann::json;
constexpr size_t MAX_NODES = 16384;
constexpr size_t MAX_VERTICES = 8 * 1024 * 1024;
constexpr size_t MAX_TRIANGLES = 4 * 1024 * 1024;
constexpr size_t NO_INDEX = std::numeric_limits<size_t>::max();

void require(bool condition, const std::string& error)
{
    if(!condition) throw std::runtime_error(error);
}

auto safe_path(const std::string& path) -> bool
{
    if(path.empty() || path.front() == '/' || path.back() == '/' || path.find('\\') != std::string::npos ||
       path.find(':') != std::string::npos) return false;
    for(unsigned char c : path) if(c < 32) return false;
    for(const auto& part : fs::path(path)) if(part == "." || part == "..") return false;
    return path.find("//") == std::string::npos;
}

auto read_json(const fs::path& root, const std::string& relative) -> json
{
    require(safe_path(relative), "GFX model reference is not a safe relative path");
    const fs::path path = root / relative;
    require(fs::is_regular_file(path) && fs::file_size(path) <= 64 * 1024 * 1024,
            "GFX model material is missing or too large: " + relative);
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "GFX model material cannot be read: " + relative);
    return json::parse(stream);
}

template<typename Vector>
auto finite_vector(const Vector& vector) -> bool
{
    for(typename Vector::length_type i = 0; i < vector.length(); ++i)
        if(!std::isfinite(vector[i])) return false;
    return true;
}

auto finite_matrix(const math::mat4& matrix) -> bool
{
    for(int i = 0; i != 4; ++i) if(!finite_vector(matrix[i])) return false;
    return true;
}

template<typename T>
auto sample(const std::vector<animation_channel::key<T>>& keys, double time, const T& bind) -> T
{
    if(keys.empty()) return bind;
    const auto next = std::lower_bound(keys.begin(), keys.end(), time,
        [](const auto& key, double value) { return key.time.count() < value; });
    if(next == keys.begin()) return next->value;
    if(next == keys.end()) return keys.back().value;
    const auto& previous = *(next - 1);
    const double interval = next->time.count() - previous.time.count();
    const float fraction = interval > 0 ? static_cast<float>((time - previous.time.count()) / interval) : 1.0f;
    if constexpr(std::is_same_v<T, math::quat>) return math::normalize(math::slerp(previous.value, next->value, fraction));
    else return math::lerp(previous.value, next->value, fraction);
}

template<typename T>
void validate_keys(const std::vector<animation_channel::key<T>>& keys, double duration)
{
    double previous = -1;
    for(const auto& key : keys)
    {
        const double time = key.time.count();
        require(std::isfinite(time) && time >= 0 && time >= previous && time <= duration + 0.002 && finite_vector(key.value),
                "GFX model action contains invalid keyframes");
        if constexpr(std::is_same_v<T, math::quat>)
            require(math::dot(key.value, key.value) > 0.0001f, "GFX model action contains zero quaternion");
        previous = time;
    }
}
} // namespace

auto prepare_pw_effect_model(const fs::path& content_root, const json& dependency)
    -> std::shared_ptr<pw_effect_model_prepared>
{
    auto output = std::make_shared<pw_effect_model_prepared>();
    try
    {
        require(dependency.at("kind") == "model" && dependency.at("generateSdf") == false &&
                dependency.at("lodCount") == 1, "GFX model compilation policy is invalid");
        output->model_ref = dependency.at("modelRef").get<std::string>();
        require(safe_path(output->model_ref), "GFX model reference is invalid");
        output->joint_count = dependency.at("jointCount").get<size_t>();
        require(output->joint_count > 0 && output->joint_count <= MAX_NODES, "GFX model skeleton count is invalid");
        const std::string action = dependency.at("actionNameUtf8").get<std::string>();
        if(!action.empty())
        {
            output->animation_ref = dependency.at("animationRef").get<std::string>();
            output->animation_duration = dependency.at("animationDurationSeconds").get<double>();
            require(safe_path(output->animation_ref) && fs::path(output->animation_ref).extension() == ".anim" &&
                    dependency.at("animationName") == "idle" && std::isfinite(output->animation_duration) &&
                    output->animation_duration > 0 && dependency.at("frameCount").get<size_t>() >= 2,
                    "GFX model authored action is incomplete");
        }
        else require(!dependency.contains("animationRef") && dependency.at("frameCount") == 0,
                     "GFX rest-pose model contains an invented action");
        const json material = read_json(content_root, dependency.at("materialRef").get<std::string>());
        require(material.at("format") == "EDS_MATERIAL" && material.at("schemaVersion") == 1,
                "GFX model material format is invalid");
        if(material.contains("materialSlots") && !material.at("materialSlots").empty())
        {
            const auto& slots = material.at("materialSlots");
            require(slots.is_array() && slots.size() <= 4096, "GFX model material slot count is invalid");
            output->textures.resize(slots.size());
            std::vector<bool> seen(slots.size(), false);
            for(const auto& slot : slots)
            {
                const size_t index = slot.at("index").get<size_t>();
                require(index < slots.size() && !seen[index], "GFX model material slots are sparse or duplicated");
                seen[index] = true;
                output->textures[index] = slot.value("diffuseTextureRef", std::string{});
                require(output->textures[index].empty() || safe_path(output->textures[index]), "GFX model texture reference is invalid");
                require(!output->textures[index].empty() || slot.value("diffuseTexture", std::string{}).empty(),
                        "GFX model authored texture was not converted");
            }
        }
        else
        {
            const std::string texture = material.value("textureRef", std::string{});
            require(texture.empty() || safe_path(texture), "GFX model texture reference is invalid");
            output->textures.push_back(texture);
        }
        output->valid = true;
    }
    catch(const std::exception& error) { output->error = error.what(); }
    return output;
}

struct pw_effect_model_geometry::implementation
{
    struct node
    {
        size_t parent = NO_INDEX;
        math::transform bind;
        size_t channel = NO_INDEX;
    };
    struct vertex
    {
        math::vec3 position{};
        math::vec2 uv{};
        math::vec4 color{1};
        std::array<size_t, 4> bones{};
        math::vec4 weights{};
    };
    struct submesh_data
    {
        size_t material = 0;
        size_t node_index = NO_INDEX;
        bool skinned = false;
        std::vector<vertex> vertices;
        std::vector<uint32_t> indices;
    };
    bool valid = false;
    std::string error;
    size_t materials = 0;
    std::vector<node> nodes;
    std::vector<size_t> bone_nodes;
    std::vector<math::mat4> inverse_binds;
    std::vector<submesh_data> meshes;
    animation_clip animation;
};

pw_effect_model_geometry::pw_effect_model_geometry() : impl_(std::make_unique<implementation>()) {}
pw_effect_model_geometry::~pw_effect_model_geometry() = default;
auto pw_effect_model_geometry::error() const -> const std::string& { return impl_->error; }
auto pw_effect_model_geometry::batch_count() const -> size_t { return impl_->valid ? impl_->materials : 0; }

auto pw_effect_model_geometry::prepare(mesh& source, const animation_clip* animation, size_t material_count) -> bool
{
    *impl_ = implementation{};
    try
    {
        auto& output = *impl_;
        const auto info = source.get_info();
        require(info.vertices > 0 && info.vertices <= MAX_VERTICES && info.triangles > 0 && info.triangles <= MAX_TRIANGLES &&
                material_count > 0 && material_count <= 4096, "GFX model geometry count is invalid");
        const auto* vb = source.get_system_vb();
        const auto* ib = source.get_system_ib();
        const auto& layout = source.get_vertex_format();
        require(vb && ib && layout.has(gfx::attribute::Position) && layout.has(gfx::attribute::TexCoord0),
                "GFX model has no readable position/UV/index data");
        const auto& submeshes = source.get_submeshes();
        const auto& bones = source.get_skin_bind_data().get_bones();
        const auto& palettes = source.get_bone_palettes();
        require(source.get_armature() && !submeshes.empty(), "GFX model has no armature or submeshes");
        output.materials = material_count;
        std::map<std::string, size_t> names;
        std::vector<size_t> mesh_nodes(submeshes.size(), NO_INDEX);
        std::function<void(const mesh::armature_node&, size_t, size_t)> visit;
        visit = [&](const mesh::armature_node& input, size_t parent, size_t depth)
        {
            require(depth <= 256 && output.nodes.size() < MAX_NODES && finite_matrix(input.local_transform.get_matrix()),
                    "GFX model armature is invalid or too deep");
            const size_t index = output.nodes.size();
            require(names.emplace(input.name, index).second, "GFX model armature has ambiguous node names");
            output.nodes.push_back({parent, input.local_transform, NO_INDEX});
            for(uint32_t mesh_index : input.submeshes)
            {
                require(mesh_index < mesh_nodes.size() && mesh_nodes[mesh_index] == NO_INDEX, "GFX model submesh has ambiguous owner");
                mesh_nodes[mesh_index] = index;
            }
            for(const auto& child : input.children) { require(bool(child), "GFX model has null armature child"); visit(*child, index, depth + 1); }
        };
        visit(*source.get_armature(), NO_INDEX, 0);
        for(const auto& bone : bones)
        {
            const auto found = names.find(bone.bone_id);
            require(found != names.end() && finite_matrix(bone.bind_pose_transform.get_matrix()), "GFX model bone has no valid armature binding");
            output.bone_nodes.push_back(found->second);
            output.inverse_binds.push_back(bone.bind_pose_transform.get_matrix());
        }
        if(animation)
        {
            require(animation->name == "idle" && !animation->channels.empty() && std::isfinite(animation->duration.count()) &&
                    animation->duration.count() > 0, "GFX model action is not the exported idle clip");
            output.animation = *animation;
            for(size_t i = 0; i < animation->channels.size(); ++i)
            {
                const auto& channel = animation->channels[i];
                const auto found = names.find(channel.node_name);
                require(found != names.end() && output.nodes[found->second].channel == NO_INDEX,
                        "GFX model action contains missing or duplicated armature channels");
                output.nodes[found->second].channel = i;
                validate_keys(channel.position_keys, animation->duration.count());
                validate_keys(channel.rotation_keys, animation->duration.count());
                validate_keys(channel.scaling_keys, animation->duration.count());
            }
        }
        std::vector<bool> used(material_count, false);
        for(size_t index = 0; index < submeshes.size(); ++index)
        {
            const auto* submesh = submeshes[index];
            require(submesh && submesh->data_group_id < material_count && submesh->face_start >= 0 && submesh->face_count > 0 &&
                    uint64_t(submesh->face_start) + submesh->face_count <= info.triangles,
                    "GFX model submesh material or index range is invalid");
            auto& destination = output.meshes.emplace_back();
            destination.material = submesh->data_group_id;
            destination.node_index = mesh_nodes[index];
            destination.skinned = submesh->skinned;
            used[destination.material] = true;
            const std::vector<uint32_t>* palette = nullptr;
            if(submesh->skinned)
            {
                require(index < palettes.size() && layout.has(gfx::attribute::Weight) && layout.has(gfx::attribute::Indices),
                        "GFX model skin has no bone palette or vertex influences");
                palette = &palettes[index].get_bones();
                require(!palette->empty(), "GFX model skin palette is empty");
                for(uint32_t bone : *palette) require(bone < bones.size(), "GFX model palette references missing bone");
            }
            else require(destination.node_index != NO_INDEX, "GFX model rigid submesh has no authored transform");
            std::map<uint32_t, uint32_t> remap;
            const size_t count = size_t(submesh->face_count) * 3;
            destination.indices.reserve(count);
            for(size_t i = 0; i < count; ++i)
            {
                const uint32_t vertex_index = ib[size_t(submesh->face_start) * 3 + i];
                require(vertex_index < info.vertices, "GFX model triangle references missing vertex");
                const auto inserted = remap.emplace(vertex_index, static_cast<uint32_t>(destination.vertices.size()));
                destination.indices.push_back(inserted.first->second);
                if(!inserted.second) continue;
                auto& vertex = destination.vertices.emplace_back();
                math::vec4 temporary{};
                gfx::vertex_unpack(math::value_ptr(temporary), gfx::attribute::Position, layout, vb, vertex_index);
                vertex.position = math::vec3(temporary);
                gfx::vertex_unpack(math::value_ptr(temporary), gfx::attribute::TexCoord0, layout, vb, vertex_index);
                vertex.uv = math::vec2(temporary);
                if(layout.has(gfx::attribute::Color0)) gfx::vertex_unpack(math::value_ptr(vertex.color), gfx::attribute::Color0, layout, vb, vertex_index);
                require(finite_vector(vertex.position) && finite_vector(vertex.uv) && finite_vector(vertex.color), "GFX model vertex contains non-finite values");
                if(palette)
                {
                    math::vec4 indices{};
                    gfx::vertex_unpack(math::value_ptr(indices), gfx::attribute::Indices, layout, vb, vertex_index);
                    gfx::vertex_unpack(math::value_ptr(vertex.weights), gfx::attribute::Weight, layout, vb, vertex_index);
                    require(finite_vector(indices) && finite_vector(vertex.weights), "GFX model vertex skin values are invalid");
                    float total = 0;
                    for(int lane = 0; lane < 4; ++lane)
                    {
                        const float weight = vertex.weights[lane];
                        require(weight >= 0 && weight <= 1, "GFX model skin weight is outside its range");
                        total += weight;
                        if(weight == 0) continue;
                        require(indices[lane] >= 0 && std::floor(indices[lane]) == indices[lane] && indices[lane] < palette->size(),
                                "GFX model skin index is outside its palette");
                        vertex.bones[lane] = palette->at(static_cast<size_t>(indices[lane]));
                    }
                    require(std::abs(total - 1) < 0.01f, "GFX model skin weights do not sum to one");
                }
            }
        }
        require(std::all_of(used.begin(), used.end(), [](bool value) { return value; }), "GFX model material inventory does not match imported geometry");
        output.valid = true;
    }
    catch(const std::exception& error) { impl_->error = error.what(); }
    return impl_->valid;
}

auto pw_effect_model_geometry::append_triangles(std::vector<pw_effect_model_batch>& batches, double time_seconds, int loops,
                                               const math::mat4& world, const math::vec4& color) const -> bool
{
    const auto& source = *impl_;
    if(!source.valid || !std::isfinite(time_seconds) || !finite_matrix(world) || !finite_vector(color)) return false;
    if(batches.empty()) batches.resize(source.materials);
    if(batches.size() != source.materials) return false;
    const double duration = source.animation.duration.count();
    double time = std::max(0.0, time_seconds);
    if(duration > 0)
    {
        // The converter includes A3DSkinModelActionCore's final inclusive
        // millisecond in clip.duration and duplicates the final pose there.
        // Update wraps after that tick; a zero loop count never plays.
        const double period_ms = std::max(1.0, std::round(duration * 1000.0));
        const double end_ms = period_ms - 1;
        const double elapsed_ms = std::floor(time * 1000.0 + 0.000001);
        time = (loops > 0 && elapsed_ms >= period_ms * loops - 1) ? end_ms / 1000.0 :
               std::fmod(elapsed_ms, period_ms) / 1000.0;
    }
    std::vector<math::mat4> nodes(source.nodes.size());
    for(size_t i = 0; i < source.nodes.size(); ++i)
    {
        const auto& node = source.nodes[i];
        math::transform local = node.bind;
        if(node.channel != NO_INDEX && loops != 0)
        {
            const auto& channel = source.animation.channels[node.channel];
            local.set_position(sample(channel.position_keys, time, local.get_position()));
            local.set_rotation(sample(channel.rotation_keys, time, local.get_rotation()));
            local.set_scale(sample(channel.scaling_keys, time, local.get_scale()));
        }
        nodes[i] = (node.parent == NO_INDEX ? world : nodes[node.parent]) * local.get_matrix();
    }
    std::vector<math::mat4> bones(source.bone_nodes.size());
    for(size_t i = 0; i < bones.size(); ++i) bones[i] = nodes[source.bone_nodes[i]] * source.inverse_binds[i];
    for(const auto& mesh : source.meshes)
    {
        std::vector<pw_effect_vertex> vertices(mesh.vertices.size());
        for(size_t i = 0; i < mesh.vertices.size(); ++i)
        {
            const auto& input = mesh.vertices[i];
            auto& output = vertices[i];
            math::vec4 position{};
            if(mesh.skinned)
            {
                for(int lane = 0; lane < 4; ++lane)
                    if(input.weights[lane] > 0) position += (bones[input.bones[lane]] * math::vec4(input.position, 1)) * input.weights[lane];
            }
            else position = nodes[mesh.node_index] * math::vec4(input.position, 1);
            output.position = math::vec3(position);
            output.uv = input.uv;
            output.color = input.color * color;
            if(!finite_vector(output.position)) return false;
        }
        auto& destination = batches[mesh.material].triangles;
        destination.reserve(destination.size() + mesh.indices.size());
        for(uint32_t index : mesh.indices) destination.push_back(vertices[index]);
    }
    return true;
}

struct pw_effect_model_runtime::implementation
{
    std::shared_ptr<pw_effect_model_prepared> prepared;
    std::string root;
    std::string error;
    asset_handle<mesh> model;
    asset_handle<animation_clip> animation;
    std::vector<asset_handle<gfx::texture>> textures;
    std::shared_ptr<mesh> model_resource;
    std::shared_ptr<animation_clip> animation_resource;
    std::vector<std::shared_ptr<gfx::texture>> texture_resources;
    pw_effect_model_geometry geometry;
    bool ready = false;
};

pw_effect_model_runtime::pw_effect_model_runtime() : impl_(std::make_unique<implementation>()) {}
pw_effect_model_runtime::~pw_effect_model_runtime() = default;
pw_effect_model_runtime::pw_effect_model_runtime(pw_effect_model_runtime&&) noexcept = default;
auto pw_effect_model_runtime::operator=(pw_effect_model_runtime&&) noexcept -> pw_effect_model_runtime& = default;
void pw_effect_model_runtime::begin(std::shared_ptr<pw_effect_model_prepared> prepared, std::string protocol_root)
{
    impl_ = std::make_unique<implementation>();
    impl_->prepared = std::move(prepared);
    impl_->root = std::move(protocol_root);
    if(!impl_->prepared || !impl_->prepared->valid || impl_->root.empty())
        impl_->error = impl_->prepared && !impl_->prepared->error.empty() ? impl_->prepared->error : "GFX model preparation or content root is absent";
    else
    {
        impl_->textures.resize(impl_->prepared->textures.size());
        impl_->texture_resources.resize(impl_->textures.size());
    }
}
auto pw_effect_model_runtime::error() const -> const std::string& { return impl_->error; }
auto pw_effect_model_runtime::batch_count() const -> size_t { return impl_->ready ? impl_->geometry.batch_count() : 0; }

auto pw_effect_model_runtime::poll(asset_manager& manager) -> pw_effect_resource_status
{
    auto& state = *impl_;
    if(!state.error.empty() || !state.prepared || !state.prepared->valid) return pw_effect_resource_status::failed;
    if(state.ready) return pw_effect_resource_status::ready;
    const auto key = [&](const std::string& reference) { return state.root + (state.root.back() == '/' ? "" : "/") + reference; };
    if(!state.model) state.model = manager.get_asset<mesh>(key(state.prepared->model_ref), load_flags::standard);
    state.model.submit();
    state.model_resource = state.model.get_if_ready();
    bool waiting = !state.model_resource;
    if(state.model_resource)
    {
        const auto vb = state.model_resource->get_hardware_vb();
        const auto ib = state.model_resource->get_hardware_ib();
        waiting = !vb || !ib || !vb->is_valid() || !ib->is_valid();
    }
    if(!state.prepared->animation_ref.empty())
    {
        if(!state.animation) state.animation = manager.get_asset<animation_clip>(key(state.prepared->animation_ref), load_flags::standard);
        state.animation.submit();
        state.animation_resource = state.animation.get_if_ready();
        waiting = waiting || !state.animation_resource;
    }
    for(size_t i = 0; i < state.textures.size(); ++i)
    {
        if(state.prepared->textures[i].empty()) continue; // Explicit authored untextured slot, never a failed resource.
        if(!state.textures[i]) state.textures[i] = manager.get_asset<gfx::texture>(key(state.prepared->textures[i]), load_flags::standard);
        state.textures[i].submit();
        state.texture_resources[i] = state.textures[i].get_if_ready();
        const auto& texture = state.texture_resources[i];
        waiting = waiting || !texture || !texture->is_valid() || texture->info.width == 0 || texture->info.height == 0;
    }
    if(waiting) return pw_effect_resource_status::waiting;
    if(state.animation_resource && std::abs(state.animation_resource->duration.count() - state.prepared->animation_duration) > 0.002)
        state.error = "GFX model imported action duration differs from source manifest";
    else if(state.model_resource->get_skin_bind_data().get_bones().size() > state.prepared->joint_count)
        state.error = "GFX model imported skeleton exceeds source joint inventory";
    else if(!state.geometry.prepare(*state.model_resource, state.animation_resource.get(), state.textures.size()))
        state.error = state.geometry.error();
    if(!state.error.empty()) return pw_effect_resource_status::failed;
    state.ready = true;
    return pw_effect_resource_status::ready;
}

auto pw_effect_model_runtime::append_triangles(std::vector<pw_effect_model_batch>& batches, double time_seconds, int loops,
                                              const math::mat4& world, const math::vec4& color) const -> bool
{
    if(!impl_->ready || !impl_->geometry.append_triangles(batches, time_seconds, loops, world, color)) return false;
    for(size_t i = 0; i < batches.size(); ++i)
    {
        batches[i].texture = impl_->textures[i];
        batches[i].untextured = impl_->prepared->textures[i].empty();
    }
    return true;
}
} // namespace unravel
