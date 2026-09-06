#pragma once

#include <engine/ecs/components/basic_component.h>

#include <uuid/uuid.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace unravel
{

/**
 * @brief Persisted map settings used to restore PW content when a scene opens.
 */
struct pw_map_component : public component_crtp<pw_map_component>
{
    std::string content_root;
    std::string map_slug = "login";
    bool auto_load = true;
    bool require_full = true;
    uint32_t buildings_per_frame = 3;

    /// Runtime feedback and identity are reset when the descriptor is deserialized.
    std::string status = "idle";
    std::string error;
    uint64_t runtime_instance_token = 0;
};

/**
 * @brief Ownership marker retained by memory checkpoints; its subtree is omitted from scene files.
 */
struct pw_map_generated_component
{
    hpp::uuid descriptor_id;
    uint64_t generation = 0;

    /// Original authored flags; memory checkpoints retain these with the map.
    std::vector<std::pair<hpp::uuid, bool>> authored_environment_active;
};

} // namespace unravel
