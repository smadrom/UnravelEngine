#pragma once

#include <engine/pw/detail/json.hpp>
#include <filesystem/filesystem.h>

#include <string>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace unravel
{

/** @brief The identity and completeness policy for one converted map candidate. */
struct pw_map_manifest_request
{
    fs::path content_root;
    std::string slug;
    int instance_id = -1;
    std::string expected_lineage;
    bool require_full = false;
    bool allow_legacy = false;
};

/** @brief Validated source identities and documents, before any runtime publication. */
struct pw_map_manifest_result
{
    bool valid = false;
    bool legacy = false;
    bool left_handed = true;
    std::string error;
    nlohmann::json manifest_json;
    nlohmann::json scene_json;
    std::vector<std::string> building_ids;
    std::vector<std::string> water_ids;
    std::vector<std::string> water_surface_ids;
    std::vector<std::string> grass_ids;
    std::vector<std::string> ecmodel_ids;
    std::vector<std::string> effect_ids;
    uint64_t grass_blade_count = 0;
    std::vector<std::string> required_outputs;
    std::unordered_map<std::string, std::string> water_surface_by_id;
};

/** @brief Read PW_MAP v2 and verify files, geometry, identities and reference closure. */
auto validate_pw_map_manifest(const pw_map_manifest_request& request) -> pw_map_manifest_result;

/** @brief Validate a supplied manifest against the actual candidate files. */
auto validate_pw_map_manifest(const pw_map_manifest_request& request,
                              const nlohmann::json& manifest) -> pw_map_manifest_result;

} // namespace unravel
