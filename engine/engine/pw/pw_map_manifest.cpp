#include "pw_map_manifest.h"
#include "detail/blake3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace unravel
{
namespace
{
using json = nlohmann::json;
using id_set = std::set<std::string>;
constexpr uintmax_t MAX_JSON_BYTES = 256ULL * 1024ULL * 1024ULL;
constexpr size_t MAX_RECORDS = 2 * 1024 * 1024;

void require(bool condition, const std::string& message)
{
    if(!condition)
    {
        throw std::runtime_error(message);
    }
}

auto normalize(std::string text) -> std::string
{
    std::replace(text.begin(), text.end(), '\\', '/');
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character)
                   { return character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character; });
    return text;
}

auto is_safe_path(const std::string& text) -> bool
{
    if(text.empty() || text.front() == '/' || text.back() == '/')
    {
        return false;
    }
    std::istringstream stream(text);
    std::string part;
    while(std::getline(stream, part, '/'))
    {
        if(part.empty() || part == "." || part == "..")
        {
            return false;
        }
    }
    return std::none_of(text.begin(), text.end(), [](unsigned char character)
                        { return character < 32 || character == ':' || character == '\\'; });
}

auto is_hex(const std::string& text, size_t length) -> bool
{
    return text.size() == length && std::all_of(text.begin(), text.end(), [](char character)
        { return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'); });
}

auto shader_tokens(const std::string& bytes) -> std::vector<std::string>
{
    std::vector<std::string> tokens;
    for(size_t i = 0; i < bytes.size();)
    {
        const unsigned char c = bytes[i];
        if(c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
        if(c == ';' || (c == '/' && i + 1 < bytes.size() && bytes[i + 1] == '/'))
        { while(i < bytes.size() && bytes[i] != '\n') ++i; continue; }
        const size_t start = i++;
        if(c == '"') { while(i < bytes.size() && bytes[i++] != '"') {} }
        else
        {
            const auto word = [](unsigned char value)
            { return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                     (value >= '0' && value <= '9') || value == '_' || value == '.' || value == '\\'; };
            if(word(c)) while(i < bytes.size() && word(bytes[i])) ++i;
        }
        tokens.push_back(bytes.substr(start, i - start));
    }
    return tokens;
}

auto read_string(const json& object, const char* key) -> std::string
{
    const json& value = object.at(key);
    require(value.is_string(), std::string(key) + " must be a string");
    return value.get<std::string>();
}

auto read_count(const json& object, const char* key) -> uint64_t
{
    const json& value = object.at(key);
    require(value.is_number_integer() && (!value.is_number_unsigned() ? value.get<int64_t>() >= 0 : true),
            std::string(key) + " must be a nonnegative integer");
    const uint64_t number = value.get<uint64_t>();
    require(number <= std::numeric_limits<uint32_t>::max(), std::string(key) + " exceeds the supported range");
    return number;
}

auto read_number(const json& object, const char* key) -> double
{
    const json& value = object.at(key);
    require(value.is_number(), std::string(key) + " must be numeric");
    const double number = value.get<double>();
    require(std::isfinite(number), std::string(key) + " must be finite");
    return number;
}

auto read_array(const json& object, const char* key) -> const json&
{
    const json& value = object.at(key);
    require(value.is_array() && value.size() <= MAX_RECORDS, std::string(key) + " must be a bounded array");
    return value;
}

auto resolve_child(const fs::path& root, const std::string& relative) -> fs::path
{
    require(is_safe_path(relative), "Unsafe candidate path: " + relative);
    const fs::path target = fs::weakly_canonical(root / fs::path(relative));
    const fs::path normalized_root = fs::weakly_canonical(root);
    const fs::path difference = target.lexically_relative(normalized_root);
    require(!difference.empty() && !difference.is_absolute() && *difference.begin() != "..",
            "Candidate path escapes content root: " + relative);
    require(fs::is_regular_file(target) && fs::file_size(target) > 0, "Missing or empty candidate file: " + relative);
    return target;
}

auto read_document(const fs::path& root, const std::string& relative) -> json
{
    const fs::path path = resolve_child(root, relative);
    require(fs::file_size(path) <= MAX_JSON_BYTES, "Candidate JSON exceeds size limit: " + relative);
    std::ifstream stream(path, std::ios::binary);
    require(stream.is_open(), "Cannot open candidate JSON: " + relative);
    std::vector<id_set> object_keys;
    json::parser_callback_t callback = [&object_keys, &relative](int depth, json::parse_event_t event, json& parsed)
    {
        require(depth < 128, "Candidate JSON nesting exceeds limit: " + relative);
        if(event == json::parse_event_t::object_start)
        {
            object_keys.emplace_back();
        }
        else if(event == json::parse_event_t::key)
        {
            require(object_keys.back().insert(parsed.get<std::string>()).second, "Duplicate JSON key: " + relative);
        }
        else if(event == json::parse_event_t::object_end)
        {
            object_keys.pop_back();
        }
        return true;
    };
    json document = json::parse(stream, callback);
    require(document.is_object(), "Candidate JSON must be an object: " + relative);
    return document;
}

auto hash_file(const fs::path& path) -> std::string
{
    std::ifstream stream(path, std::ios::binary);
    require(stream.is_open(), "Cannot hash candidate output: " + path.generic_string());
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    std::array<char, 64 * 1024> buffer{};
    while(stream)
    {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = stream.gcount();
        if(count > 0)
        {
            blake3_hasher_update(&hasher, buffer.data(), static_cast<size_t>(count));
        }
    }
    require(stream.eof(), "Failed reading candidate output: " + path.generic_string());
    std::array<uint8_t, BLAKE3_OUT_LEN> digest{};
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    constexpr char HEX[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for(const uint8_t byte : digest)
    {
        result.push_back(HEX[byte >> 4]);
        result.push_back(HEX[byte & 15]);
    }
    return result;
}

auto make_id(const std::string& kind, uint64_t first, uint64_t second) -> std::string
{
    std::ostringstream stream;
    stream << kind << ':' << std::hex << std::setfill('0') << std::setw(8) << first << ':' << std::setw(8) << second;
    return stream.str();
}

auto is_canonical_id(const std::string& identifier, const std::string& prefix, size_t components) -> bool
{
    if(identifier.size() != prefix.size() + components * 9 || identifier.compare(0, prefix.size(), prefix) != 0)
    {
        return false;
    }
    for(size_t index = 0; index < components; ++index)
    {
        const size_t begin = prefix.size() + index * 9;
        if(identifier[begin] != ':' || !is_hex(identifier.substr(begin + 1, 8), 8))
        {
            return false;
        }
    }
    return true;
}

auto validate_accounting(const json& accounting, const json& identities, const std::string& prefix,
                         size_t components) -> id_set
{
    const uint64_t raw = read_count(accounting, "rawReferences");
    const uint64_t unique = read_count(accounting, "uniqueSources");
    const uint64_t duplicates = read_count(accounting, "duplicateReferences");
    const uint64_t emitted = read_count(accounting, "emitted");
    const uint64_t failed = read_count(accounting, "failed");
    const uint64_t deferred = read_count(accounting, "deferred");
    require(raw == unique + duplicates && unique == emitted + failed + deferred, prefix + " accounting contradiction");
    require(failed == 0 && deferred == 0, prefix + " contains failed or deferred mandatory instances");
    require(identities.is_array() && identities.size() == unique && identities.size() <= MAX_RECORDS,
            prefix + " identity inventory does not match accounting");
    id_set result;
    for(const json& value : identities)
    {
        require(value.is_string(), prefix + " identity must be a string");
        const std::string identifier = value.get<std::string>();
        require(is_canonical_id(identifier, prefix, components), "Malformed source identity: " + identifier);
        require(result.insert(identifier).second, "Duplicate source identity: " + identifier);
    }
    return result;
}

void validate_terrain(const json& terrain, const pw_map_manifest_request& request)
{
    require(terrain.is_object(), "Map terrain contract is missing");
    const json& native = terrain.at("native");
    const json& window = terrain.at("window");
    const json& emitted = terrain.at("emitted");
    const uint64_t cols = read_count(native, "blockCols");
    const uint64_t rows = read_count(native, "blockRows");
    const uint64_t grid = read_count(native, "blockGrid");
    const double size = read_number(native, "blockSizeM");
    require(cols > 0 && rows > 0 && grid > 0 && size > 0, "Terrain native grid must be positive");
    const double left = read_number(native, "leftM");
    const double top = read_number(native, "topM");
    require(read_number(native, "rightM") - left == cols * size &&
            top - read_number(native, "bottomM") == rows * size &&
            read_number(native, "widthM") == cols * size && read_number(native, "depthM") == rows * size &&
            read_count(native, "blockCount") == cols * rows &&
            read_count(native, "sampleWidth") == cols * grid + 1 &&
            read_count(native, "sampleHeight") == rows * grid + 1, "Terrain native bounds, samples or counts contradict grid");
    const uint64_t col_begin = read_count(window, "colBegin");
    const uint64_t col_end = read_count(window, "colEnd");
    const uint64_t row_begin = read_count(window, "rowBegin");
    const uint64_t row_end = read_count(window, "rowEnd");
    require(col_begin < col_end && col_end <= cols && row_begin < row_end && row_end <= rows,
            "Terrain window is outside the native grid");
    const uint64_t width = col_end - col_begin;
    const uint64_t height = row_end - row_begin;
    require(read_count(window, "cols") == width && read_count(window, "rows") == height &&
            read_count(window, "blockCount") == width * height &&
            read_number(emitted, "leftM") == left + col_begin * size &&
            read_number(emitted, "rightM") == left + col_end * size &&
            read_number(emitted, "topM") == top - row_begin * size &&
            read_number(emitted, "bottomM") == top - row_end * size &&
            read_count(emitted, "sampleWidth") == width * grid + 1 &&
            read_count(emitted, "sampleHeight") == height * grid + 1, "Terrain emitted bounds or samples contradict window");
    const bool is_full = col_begin == 0 && row_begin == 0 && col_end == cols && row_end == rows;
    require(terrain.at("expectedFull").is_boolean() && terrain.at("expectedFull").get<bool>() == is_full,
            "Terrain expectedFull contradicts integer window");
    require(read_number(terrain, "coverageRatio") == static_cast<double>(width * height) / static_cast<double>(cols * rows),
            "Terrain coverage contradicts integer window");
    const std::string mode = read_string(terrain, "mode");
    require(mode == "full" || mode == "region" || mode == "tiled", "Unknown terrain mode");
    require(mode != "full" || is_full, "Full terrain mode has a regional window");
    require(!request.require_full || (is_full && mode == "full"), "Full map request cannot use a regional or tiled candidate");
    require(read_array(terrain, "skipped").empty(), "Terrain has skipped mandatory content");
    const uint64_t source_layers = read_count(terrain, "sourceLayers");
    require(read_count(terrain, "emittedLayers") == source_layers, "Terrain emitted layers differ from source layers");
    read_count(terrain, "emittedSplats");
    read_count(terrain, "sourceMaskAreas");
    if(request.require_full && request.slug == "a61")
    {
        require(cols == 64 && rows == 48 && grid == 32 && size == 64 && left == -2048 && top == 1536 &&
                source_layers == 453 && read_count(terrain, "sourceMaskAreas") == 192,
                "Full a61 native geometry or source structure mismatch");
    }
}

struct manifest_validator
{
    const pw_map_manifest_request& request;
    const json& manifest;
    pw_map_manifest_result result;
    std::map<std::string, const json*> outputs;
    std::map<std::string, const json*> model_by_id;
    id_set building_ids;
    id_set water_ids;
    id_set surface_ids;
    std::map<std::string, json> glb_documents;
    std::map<std::string, uint8_t> effect_dependency_states;

    auto require_output(const std::string& path, const std::string& kind = {}) const -> const json&
    {
        require(is_safe_path(path), "Malformed asset reference: " + path);
        const std::map<std::string, const json*>::const_iterator found = outputs.find(normalize(path));
        require(found != outputs.end(), "Asset reference is absent from output inventory: " + path);
        require(kind.empty() || found->second->at("kind") == kind, "Asset reference kind mismatch: " + path);
        return *found->second;
    }

    void validate_identity()
    {
        require(manifest.is_object() && manifest.at("format") == "PW_MAP" && read_count(manifest, "schemaVersion") == 2,
                "Map requires PW_MAP schemaVersion 2");
        const std::string version = read_string(manifest, "converterVersion");
        result.legacy = version == "eds-converter-v4" || version == "eds-converter-v5" || version == "eds-converter-v6";
        require((version == "eds-converter-v7" || (result.legacy && request.allow_legacy && !request.require_full)) &&
                manifest.at("status") == "generated", "Converter v7 is required; v4-v6 are available only as explicit legacy compatibility");
        require(read_string(manifest, "slug") == request.slug, "Manifest slug differs from requested map");
        const std::string source_map = "maps/" + request.slug + "/" + request.slug + ".ecwld";
        require(normalize(read_string(manifest, "sourceMap")) == source_map, "Manifest sourceMap/slug mismatch");
        const uint64_t instance = read_count(manifest, "instanceId");
        require(request.instance_id < 0 || instance == static_cast<uint64_t>(request.instance_id), "Manifest instance differs from request");
        require((request.slug != "a61" || instance == 161) && (request.slug != "login" || instance == 0),
                "Manifest map/instance tuple mismatch");
        const std::string lineage = read_string(manifest, "lineage");
        require(!lineage.empty() && lineage.size() <= 127 && std::all_of(lineage.begin(), lineage.end(),
                [](unsigned char value) { return value >= 33 && value <= 126; }), "Manifest lineage must be a printable token");
        require(request.expected_lineage.empty() || request.expected_lineage == lineage, "Manifest lineage differs from request");
        id_set sources;
        bool has_required_map = false;
        for(const json& source : read_array(manifest, "sources"))
        {
            const std::string path = read_string(source, "path");
            require(is_safe_path(path) && sources.insert(normalize(path)).second, "Invalid or duplicate source path: " + path);
            require(is_hex(read_string(source, "contentHash"), 64), "Malformed source content hash: " + path);
            require(source.at("required").is_boolean(), "Source required must be boolean");
            has_required_map = has_required_map || (normalize(path) == source_map && source.at("required").get<bool>());
        }
        require(has_required_map, "Sources do not bind the required source map");
    }

    void validate_outputs()
    {
        const json& records = read_array(manifest, "outputs");
        require(!records.empty(), "Map output inventory is empty");
        for(const json& record : records)
        {
            const std::string path = read_string(record, "path");
            require(outputs.emplace(normalize(path), &record).second, "Duplicate output path: " + path);
            require(!read_string(record, "kind").empty(), "Output kind is empty: " + path);
            const std::string digest = read_string(record, "contentHash");
            require(is_hex(digest, 64), "Malformed output BLAKE3 hash: " + path);
            require(hash_file(resolve_child(request.content_root, path)) == digest, "Output BLAKE3 mismatch: " + path);
            result.required_outputs.push_back(path);
        }
    }

    void validate_source_metadata(const json& meta)
    {
        const json source_manifest = read_document(request.content_root, "_meta/source.manifest.json");
        require(source_manifest.at("format") == "EDS_SOURCE_MANIFEST" && read_count(source_manifest, "schemaVersion") == 2 &&
                source_manifest.at("converterVersion") == manifest.at("converterVersion"), "Source manifest version mismatch");
        std::map<std::string, std::string> source_hashes;
        for(const json& record : read_array(source_manifest, "records"))
        {
            const std::string path = read_string(record, "sourcePath");
            const std::string digest = read_string(record, "sourceHash");
            require(is_safe_path(path) && is_hex(digest, 64) && source_hashes.emplace(normalize(path), digest).second,
                    "Invalid source manifest record: " + path);
            require(record.at("converterVersion") == manifest.at("converterVersion") &&
                    record.at("optionsHash") == meta.at("conversionCache").at("optionsHash"), "Source conversion options differ");
        }
        require(source_hashes.size() == manifest.at("sources").size(), "Source manifest inventory differs from PW_MAP");
        for(const json& source : manifest.at("sources"))
        {
            const std::string path = normalize(read_string(source, "path"));
            require(source_hashes.count(path) != 0 && source_hashes.at(path) == read_string(source, "contentHash"),
                    "Source manifest hash differs from PW_MAP: " + path);
        }
    }

    void validate_documents()
    {
        if(!result.legacy)
        {
            require_output("maps/" + request.slug + "/scene.eds.json", "scene");
        }
        result.scene_json = read_document(request.content_root, "maps/" + request.slug + "/scene.eds.json");
        const json meta = read_document(request.content_root, "_meta/manifest.eds.json");
        for(const json* document : std::array<const json*, 2>{&result.scene_json, &meta})
        {
            require(document->at("format") == "EDS" && read_count(*document, "schemaVersion") == 1,
                    "Scene and EDS manifest require schemaVersion 1");
            require(normalize(read_string(*document, "sourceMap")) == normalize(read_string(manifest, "sourceMap")) &&
                    normalize(read_string(*document, "mapName")) == request.slug, "Scene or EDS manifest map tuple mismatch");
        }
        require(meta.at("status") == "generated" && meta.at("layout") == "content/maps" &&
                read_count(meta, "instanceId") == read_count(manifest, "instanceId"), "EDS manifest identity mismatch");
        require(meta.at("files").at("scene") == "maps/" + request.slug + "/scene.eds.json" &&
                meta.at("files").at("sourceManifest") == "_meta/source.manifest.json", "EDS manifest file pointers mismatch");
        const json& cache = meta.at("conversionCache");
        require(cache.at("converterVersion") == manifest.at("converterVersion") && cache.at("handedness") == "left-handed",
                "Unsupported PW mesh convention or converter version");
        require(meta.at("contentAddressing").at("hashAlgorithm") == "BLAKE3", "Unsupported PW output hash algorithm");
        validate_source_metadata(meta);
        const json& generated = meta.at("generatedResources");
        require(read_count(generated, "count") == outputs.size() && read_array(generated, "records").size() == outputs.size(),
                "EDS generated resource inventory differs from PW_MAP");
        id_set generated_paths;
        for(const json& record : generated.at("records"))
        {
            const std::string path = read_string(record, "path");
            require(generated_paths.insert(normalize(path)).second, "Duplicate EDS generated resource: " + path);
            require(require_output(path, read_string(record, "kind")).at("contentHash") == record.at("contentHash"),
                    "EDS generated resource hash differs from PW_MAP: " + path);
        }
        const json& accounting = manifest.at("scene").at("accounting");
        require(read_count(meta.at("counts"), "buildings") == read_count(accounting, "buildingInstances") &&
                read_count(meta.at("counts"), "nodes") == read_count(accounting, "sceneNodes"), "EDS source counts differ from PW_MAP");
    }

    void validate_inventories()
    {
        const json& scene = manifest.at("scene");
        const json& water = manifest.at("water");
        building_ids = validate_accounting(scene.at("accounting"), scene.at("identity").at("uniqueIds"), "ornament", 2);
        water_ids = validate_accounting(water.at("accounting").at("entities"), water.at("identity").at("uniqueIds"), "water", 2);
        surface_ids = validate_accounting(water.at("accounting").at("surfaces"), water.at("identity").at("surfaceIds"), "water-surface", 3);
        require(read_array(scene.at("accounting"), "failures").empty() && read_array(water.at("accounting"), "failures").empty(),
                "Candidate contains mandatory conversion failures");
        const json& textures = manifest.at("assets").at("textures");
        require(read_array(textures, "classified").empty(), "Candidate contains missing, corrupt or unsupported textures");
        for(const char* name : {"missing", "corrupt", "unsupported"})
        {
            require(read_count(textures.at("classifiedCounts"), name) == 0, "Texture failure counts contradict completeness");
        }
        if(request.require_full && request.slug == "a61")
        {
            require(!building_ids.empty() && !water_ids.empty() && !surface_ids.empty(), "Full a61 requires scene and water inventories");
        }
        result.building_ids.assign(building_ids.begin(), building_ids.end());
        result.water_ids.assign(water_ids.begin(), water_ids.end());
        result.water_surface_ids.assign(surface_ids.begin(), surface_ids.end());
    }

    void validate_texture_references(const json& value)
    {
        if(value.is_object())
        {
            for(json::const_iterator item = value.begin(); item != value.end(); ++item)
            {
                if((item.key() == "textureRef" || item.key() == "diffuseTextureRef" || item.key() == "normalTextureRef") &&
                    item.value().is_string() && !item.value().get<std::string>().empty())
                {
                    require_output(item.value().get<std::string>());
                }
                else
                {
                    validate_texture_references(item.value());
                }
            }
        }
        else if(value.is_array())
        {
            for(const json& item : value)
            {
                validate_texture_references(item);
            }
        }
    }

    void validate_models()
    {
        const json& models = manifest.at("assets").at("models");
        const json& entries = read_array(models, "entries");
        require(read_count(models, "uniqueAssets") == entries.size() &&
                read_count(manifest.at("scene").at("accounting"), "uniqueModelAssets") == entries.size() &&
                read_count(manifest.at("scene").at("accounting"), "uniqueMaterialAssets") == entries.size(), "Model asset counts differ");
        id_set source_paths;
        id_set identities;
        for(const json& entry : entries)
        {
            const std::string source = read_string(entry, "sourcePath");
            require(is_safe_path(source) && normalize(source) == source && source_paths.insert(source).second,
                    "Invalid or duplicate model source path: " + source);
            require(is_hex(read_string(entry, "contentHash"), 64) && is_hex(read_string(entry, "identityHash"), 64) &&
                    identities.insert(read_string(entry, "identityHash")).second, "Invalid or duplicate model asset identity");
            const std::string model = read_string(entry, "modelPath");
            require_output(model, "model");
            require(fs::path(model).extension() == ".glb", "PW model must reference a GLB output: " + model);
            const std::string material = read_string(entry, "materialPath");
            require_output(material, "material");
            const json document = read_document(request.content_root, material);
            require(document.at("format") == "EDS_MATERIAL" && read_count(document, "schemaVersion") == 1,
                    "PW model material format mismatch: " + material);
            validate_texture_references(document);
            const json& refs = read_array(entry, "referencedBy");
            require(!refs.empty(), "Model has no scene identity references: " + model);
            for(const json& reference : refs)
            {
                require(reference.is_string(), "Model referencedBy identity must be a string");
                const std::string identifier = reference.get<std::string>();
                require(building_ids.count(identifier) != 0 && model_by_id.emplace(identifier, &entry).second,
                        "Unknown or conflicting model referencedBy identity: " + identifier);
            }
        }
        require(model_by_id.size() == building_ids.size(), "Required building identity lacks a model asset");
    }

    auto get_placement(const json& record) -> json
    {
        json signature;
        for(const char* name : {"pos", "dir", "up"})
        {
            const json& values = read_array(record, name);
            require(values.size() == 3, std::string("Placement ") + name + " must contain three numbers");
            for(const json& value : values)
            {
                require(value.is_number() && std::isfinite(value.get<double>()), "Placement has nonfinite coordinates");
            }
            signature[name] = values;
        }
        return signature;
    }

    void validate_buildings()
    {
        const json& records = read_array(result.scene_json, "buildings");
        require(records.size() == read_count(manifest.at("scene").at("accounting"), "buildingInstances") &&
                records.size() == read_count(manifest.at("scene").at("accounting"), "rawReferences"), "Scene building counts differ");
        require(read_array(result.scene_json, "nodes").size() == read_count(manifest.at("scene").at("accounting"), "sceneNodes"),
                "Scene node count differs");
        std::map<std::string, json> seen;
        for(const json& record : records)
        {
            const std::string identifier = read_string(record, "sourceId");
            require(building_ids.count(identifier) != 0 && identifier == make_id("ornament", read_count(record, "exportId"),
                    read_count(record, "bsdOffset")), "Building has unknown or contradictory identity: " + identifier);
            const json& open = record.at("openFormat");
            require(open.at("accountedFor") == true && open.at("convertedToOpenFormat") == true,
                    "Required building is not converted: " + identifier);
            const json& asset = *model_by_id.at(identifier);
            require(open.at("model") == asset.at("modelPath") && open.at("material") == asset.at("materialPath") &&
                    open.at("logicalId") == "building_" + fs::path(read_string(asset, "modelPath")).stem().string(),
                    "Building references do not match its inventoried model: " + identifier);
            const json& refs = read_array(record, "sourceRefs");
            require(!refs.empty(), "Building sourceRefs is empty: " + identifier);
            for(const json& reference : refs)
            {
                read_count(reference, "block");
                read_count(reference, "localIndex");
            }
            json signature = get_placement(record);
            signature["openFormat"] = open;
            signature["sourceRefs"] = refs;
            signature["externalPath"] = normalize(read_string(record, "externalPath"));
            require(signature.at("externalPath") == asset.at("sourcePath"), "Building source path differs from model asset: " + identifier);
            const std::pair<std::map<std::string, json>::iterator, bool> insertion = seen.emplace(identifier, signature);
            require(insertion.second || insertion.first->second == signature, "Conflicting repeated building identity: " + identifier);
        }
        require(seen.size() == building_ids.size(), "Required building identity is missing from scene");
    }

    void validate_water()
    {
        std::map<std::string, json> seen;
        std::map<std::string, json> seen_surfaces;
        std::map<std::string, json> shared_documents;
        uint64_t count = 0;
        for(const json& record : result.scene_json.at("nodes"))
        {
            if(record.value("kind", "") != "Water")
            {
                continue;
            }
            ++count;
            const std::string identifier = read_string(record, "sourceId");
            require(water_ids.count(identifier) != 0 && identifier == make_id("water", read_count(record, "exportId"),
                    read_count(record, "bsdOffset")), "Water has unknown or contradictory identity: " + identifier);
            const json& open = record.at("openFormat");
            require(open.at("accountedFor") == true && open.at("convertedToOpenFormat") == true,
                    "Required water is not converted: " + identifier);
            const std::string payload = read_string(record, "payload");
            require(open.at("surface") == payload && is_safe_path(payload), "Water payload references differ: " + identifier);
            const std::string path = "maps/" + request.slug + "/" + payload;
            if(!result.legacy)
            {
                require_output(path, "water");
            }
            const json document = read_document(request.content_root, path);
            require(document.at("sourceId") == identifier && document.at("kind") == "Water", "Water payload identity differs: " + path);
            const json& surface = document.at("waterSurface");
            const std::string surface_id = read_string(surface, "sourceId");
            require(surface_ids.count(surface_id) != 0 && surface.at("format") == "EDS_WATER_SURFACE" &&
                    read_count(surface, "schemaVersion") == 1, "Water payload surface is not inventoried: " + path);
            const json& area = surface.at("area");
            std::ostringstream expected_surface;
            expected_surface << make_id("water-surface", read_count(area, "subTerrain"), read_count(area, "dataIndex"))
                             << ':' << std::hex << std::setfill('0') << std::setw(8) << read_count(area, "areaId");
            require(surface_id == expected_surface.str(), "Water surface identity contradicts area: " + path);
            require(record.at("water") == area && document.at("water") == area, "Water node and surface geometry differ: " + identifier);
            json signature = get_placement(record);
            require(get_placement(document) == signature, "Water payload placement differs from scene: " + path);
            signature["area"] = area;
            signature["surfaceId"] = surface_id;
            const std::pair<std::map<std::string, json>::iterator, bool> insertion = seen.emplace(identifier, signature);
            require(insertion.second || insertion.first->second == signature, "Conflicting repeated water identity: " + identifier);
            json surface_signature = surface;
            if(surface.contains("sharedSurface"))
            {
                const json& shared = surface.at("sharedSurface");
                const std::string shared_path = "maps/" + request.slug + "/" + read_string(shared, "payload");
                if(!result.legacy)
                {
                    require_output(shared_path, "water");
                }
                if(shared_documents.count(shared_path) == 0)
                {
                    shared_documents.emplace(shared_path, read_document(request.content_root, shared_path));
                }
                const json& target = shared_documents.at(shared_path).at("waterSurface");
                require(!target.contains("sharedSurface") && target.at("sourceId") == surface_id &&
                        target.at("logicalId") == shared.at("logicalId") && target.at("area") == area,
                        "Water shared surface target differs or forms a reference chain: " + path);
                surface_signature["mesh"] = target.at("mesh");
                surface_signature.erase("sharedSurface");
            }
            const json& mesh = surface_signature.at("mesh");
            const json& vertices = read_array(mesh, "vertices");
            const json& indices = read_array(mesh, "indices");
            require(!vertices.empty() && !indices.empty() && indices.size() % 3 == 0, "Water surface has no usable triangles: " + path);
            for(const json& vertex : vertices)
            {
                require(vertex.is_array() && vertex.size() == 4, "Water vertex format mismatch: " + path);
                for(size_t coordinate = 0; coordinate < 3; ++coordinate)
                {
                    require(vertex[coordinate].is_number() && std::isfinite(vertex[coordinate].get<double>()),
                            "Water vertex is not finite: " + path);
                }
            }
            for(const json& index : indices)
            {
                require(index.is_number_integer() && index.get<int64_t>() >= 0 && index.get<uint64_t>() < vertices.size(),
                        "Water triangle index is outside vertex buffer: " + path);
            }
            surface_signature.erase("logicalId");
            surface_signature.erase("sourceRefs");
            surface_signature.erase("contentHash");
            if(surface_signature.contains("material"))
            {
                surface_signature["material"].erase("logicalId");
            }
            const std::pair<std::map<std::string, json>::iterator, bool> surface_insertion = seen_surfaces.emplace(surface_id, surface_signature);
            require(surface_insertion.second || surface_insertion.first->second == surface_signature,
                    "Conflicting repeated water surface: " + surface_id);
            result.water_surface_by_id.emplace(identifier, surface_id);
            result.required_outputs.push_back(path);
        }
        const json& accounting = manifest.at("water").at("accounting");
        require(count == read_count(accounting, "records") && count == read_count(accounting.at("entities"), "rawReferences") &&
                count == read_count(accounting, "convertedSurfaces") && count == read_count(accounting.at("surfaces"), "rawReferences"),
                "Water raw accounting differs from scene");
        require(seen.size() == water_ids.size() && seen_surfaces.size() == surface_ids.size(), "Required water entity or surface is absent");
    }

    auto read_glb(const std::string& relative) -> const json&
    {
        auto found = glb_documents.find(relative);
        if(found != glb_documents.end()) return found->second;
        const fs::path path = resolve_child(request.content_root, relative);
        std::ifstream input(path, std::ios::binary);
        std::array<unsigned char, 20> header{};
        input.read(reinterpret_cast<char*>(header.data()), header.size());
        const auto u32 = [&header](size_t offset) -> uint32_t
        {
            return uint32_t(header[offset]) | uint32_t(header[offset + 1]) << 8 |
                   uint32_t(header[offset + 2]) << 16 | uint32_t(header[offset + 3]) << 24;
        };
        require(input.good() && u32(0) == 0x46546c67 && u32(4) == 2 && u32(8) == fs::file_size(path) &&
                u32(16) == 0x4e4f534a && u32(12) > 0 && u32(12) <= MAX_JSON_BYTES && u32(12) % 4 == 0 &&
                uint64_t(u32(12)) + 20 <= u32(8), "Invalid required GLB header: " + relative);
        std::string bytes(u32(12), '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        require(input.good(), "Truncated required GLB document: " + relative);
        json document = json::parse(bytes);
        require(document.is_object() && document.at("asset").at("version") == "2.0" &&
                document.at("asset").at("generator") == "A3DMapEditor EDS converter" &&
                !read_array(document, "meshes").empty(), "Required GLB is not an authored PW mesh: " + relative);
        return glb_documents.emplace(relative, std::move(document)).first->second;
    }

    auto validate_model_nodes(const char* section_name, const char* node_kind, const char* id_prefix,
                              size_t id_components) -> id_set
    {
        const bool grass = std::string(node_kind) == "Grass";
        const json& section = manifest.at(section_name);
        const json& accounting = section.at("accounting");
        const id_set identities = validate_accounting(accounting, section.at("identity").at("uniqueIds"), id_prefix, id_components);
        std::map<std::string, const json*> entries;
        uint64_t blades = 0;
        for(const json& entry : read_array(section, "entries"))
        {
            const std::string id = read_string(entry, "sourceId");
            require(identities.count(id) != 0 && entries.emplace(id, &entry).second, "Unknown or duplicate renderable entry: " + id);
            require(read_count(entry, "rawReferences") > 0 && entry.at("generateSdf") == false && read_count(entry, "lodCount") == 1,
                    "Renderable source or compiler policy mismatch: " + id);
            const std::string model_path = read_string(entry, "model");
            require_output(model_path, "model");
            require(fs::path(model_path).extension() == ".glb", "Required renderable must reference GLB: " + id);
            const std::string material_path = read_string(entry, "material");
            require_output(material_path, "material");
            const json material = read_document(request.content_root, material_path);
            require(material.at("format") == "EDS_MATERIAL" && read_count(material, "schemaVersion") == 1,
                    "Required renderable material is invalid: " + id);
            validate_texture_references(material);
            const json& glb = read_glb(model_path);
            if(grass)
            {
                const uint64_t count = read_count(entry, "bladeCount");
                require(count > 0 && glb.at("scenes").at(0).at("extras").at("pwMeshUsage") == "grass",
                        "Grass GLB lacks authored blades or importer policy: " + id);
                blades += count;
                read_count(entry, "typeId");
                require(read_array(entry, "position").size() == 3 && entry.at("position").at(1) == 0,
                        "Grass entity must use horizontal area center: " + id);
                const json& primitives = read_array(glb.at("meshes").at(0), "primitives");
                require(primitives.size() == 1, "Grass must contain one batched authored primitive: " + id);
                const json& primitive = primitives.at(0);
                const json& accessors = read_array(glb, "accessors");
                const json& positions = accessors.at(read_count(primitive.at("attributes"), "POSITION"));
                const json& indices = accessors.at(read_count(primitive, "indices"));
                require(read_count(positions, "count") >= count * 3 && read_count(positions, "count") % count == 0 &&
                        read_count(indices, "count") >= count * 3 && read_count(indices, "count") % (count * 3) == 0,
                        "Grass GLB vertex or triangle counts contradict blades: " + id);
            }
            else
            {
                const std::string animation = read_string(entry, "animation");
                require(is_safe_path(animation) && fs::path(animation).extension() == ".anim" &&
                        fs::path(animation).parent_path() == fs::path(model_path).parent_path() &&
                        read_string(entry, "animationName") == "idle" && entry.at("loop").is_boolean() &&
                        read_number(entry, "animationDurationSeconds") > 0 && read_count(entry, "samples") >= 2 &&
                        read_count(entry, "joints") > 0, "ECModel animation contract is invalid: " + id);
                const json& animations = read_array(glb, "animations");
                require(animations.size() == 1 && animations.at(0).at("name") == "idle" &&
                        !read_array(animations.at(0), "channels").empty() &&
                        glb.at("skins").size() == 1 && glb.at("skins").at(0).at("joints").size() == read_count(entry, "joints"),
                        "ECModel GLB lacks required authored skeleton or idle animation: " + id);
            }
        }
        require(entries.size() == identities.size(), "Required renderable entry inventory is incomplete");
        std::map<std::string, uint64_t> raw;
        std::map<std::string, json> signatures;
        for(const json& record : result.scene_json.at("nodes"))
        {
            if(record.value("kind", "") != node_kind) continue;
            const std::string id = read_string(record, "sourceId");
            const std::string expected = grass ? make_id(id_prefix, read_count(record, "exportId"), read_count(record, "bsdOffset")) :
                make_id(id_prefix, read_count(record, "exportId"), 0).substr(0, std::string(id_prefix).size() + 9);
            require(identities.count(id) != 0 && id == expected, "Unknown or contradictory renderable source ID: " + id);
            const json& entry = *entries.at(id);
            const json& open = record.at("openFormat");
            require(open.at("accountedFor") == true && open.at("convertedToOpenFormat") == true && open.at("sourceId") == id,
                    "Required renderable is not converted: " + id);
            for(const char* field : {"model", "material", "generateSdf", "lodCount"})
                require(open.at(field) == entry.at(field), "Renderable scene reference differs from inventory: " + id);
            if(grass)
            {
                for(const char* field : {"position", "bladeCount", "typeId"})
                    require(open.at(field) == entry.at(field), "Grass scene fields differ from inventory: " + id);
                require(read_number(open, "alphaCutoff") == 84.0 / 255.0 && open.at("windAnimation") == "static-base-pose" &&
                        open.at("position").at(0) == record.at("pos").at(0) && open.at("position").at(2) == record.at("pos").at(2),
                        "Grass material or local center differs from source: " + id);
            }
            else
            {
                for(const char* field : {"animation", "animationName", "animationDurationSeconds", "loop", "joints", "samples"})
                    require(open.at(field) == entry.at(field), "ECModel scene animation differs from inventory: " + id);
            }
            const std::string payload_path = "maps/" + request.slug + "/" + read_string(record, "payload");
            require_output(payload_path, id_prefix);
            const json payload = read_document(request.content_root, payload_path);
            json signature = get_placement(record);
            require(payload.at("sourceId") == id && payload.at("kind") == node_kind && payload.at("openFormat") == open &&
                    get_placement(payload) == signature, "Required renderable payload differs from scene: " + id);
            signature["openFormat"] = open;
            signature["externalPath"] = normalize(read_string(record, "externalPath"));
            const auto inserted = signatures.emplace(id, signature);
            require(inserted.second || inserted.first->second == signature, "Conflicting repeated renderable source: " + id);
            ++raw[id];
        }
        uint64_t total = 0;
        for(const auto& entry : entries)
        {
            require(raw[entry.first] == read_count(*entry.second, "rawReferences"), "Renderable raw references differ: " + entry.first);
            total += raw[entry.first];
        }
        require(total == read_count(accounting, "rawReferences"), "Renderable scene count differs from accounting");
        if(grass)
        {
            require(blades == read_count(accounting, "bladeCount"), "Grass blade accounting differs from authored entries");
            result.grass_blade_count = blades;
        }
        return identities;
    }

    void validate_effect_shader(const json& dependency)
    {
        const std::string path = read_string(dependency, "shaderRef");
        const json& output = require_output(path, "gfxShader");
        const json shader = read_document(request.content_root, path);
        require(shader.at("format") == "EDS_GFX_SHADER" && read_count(shader, "schemaVersion") == 1 &&
                shader.at("implementation") == "angelica-fluid-v1" && dependency.at("implementation") == shader.at("implementation") &&
                dependency.at("converted") == true && path == "fx/shaders/" + read_string(output, "contentHash") + ".eds.shader.json",
                "Required GFX shader implementation or identity mismatch: " + path);
        const auto source_bytes = [&](const json& source)
        {
            const std::string source_path = normalize(read_string(source, "sourcePathUtf8"));
            const std::string digest = read_string(source, "contentHash");
            const std::string hex = read_string(source, "bytesHex");
            require(is_safe_path(source_path) && is_hex(digest, 64) && hex.size() <= 2 * 1024 * 1024 &&
                    hex.size() % 2 == 0 && is_hex(hex, hex.size()), "Required GFX shader raw source record is invalid");
            std::string bytes(hex.size() / 2, '\0');
            const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
            for(size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>((digit(hex[2 * i]) << 4) | digit(hex[2 * i + 1]));
            blake3_hasher hasher;
            blake3_hasher_init(&hasher);
            blake3_hasher_update(&hasher, bytes.data(), bytes.size());
            std::array<uint8_t, BLAKE3_OUT_LEN> hash{};
            blake3_hasher_finalize(&hasher, hash.data(), hash.size());
            std::string actual;
            for(uint8_t byte : hash) { actual.push_back("0123456789abcdef"[byte >> 4]); actual.push_back("0123456789abcdef"[byte & 15]); }
            require(actual == digest, "Required GFX shader raw source hash mismatch");
            bool bound = false;
            for(const json& record : manifest.at("sources"))
                if(normalize(read_string(record, "path")) == source_path)
                    bound = record.at("required") == true && read_string(record, "contentHash") == digest;
            require(bound, "Required GFX shader source is absent from source inventory: " + source_path);
            return bytes;
        };
        const json& source = shader.at("source");
        require(normalize(read_string(dependency, "sourcePathUtf8")) == normalize(read_string(source, "sourcePathUtf8")) &&
                read_string(dependency, "sourceHash") == read_string(source, "contentHash"), "GFX shader dependency source differs from descriptor");
        static const std::string descriptor =
            "version 5 \"Fluid\" { pixelshader Shaders\\ps\\fluid.txt { } { filter TEXF_LINEAR address TADDR_WRAP } }";
        static const std::string program =
            "ps.1.4\ndef c6, 0, 0, 0, 1\ntexld r0, t0\nadd r2, c0, r0\nphase\n"
            "texld r0, t0\ntexld r1, r2\nlrp r0, c1.r, r1, r0\n";
        require(shader_tokens(source_bytes(source)) == shader_tokens(descriptor) &&
                shader_tokens(source_bytes(shader.at("pixelShader"))) == shader_tokens(program) &&
                normalize(read_string(shader.at("pixelShader"), "sourcePathUtf8")) == "shaders/ps/fluid.txt",
                "GFX shader source does not match the supported native Fluid implementation");
    }

    void validate_effect_dependencies(const json& document, const std::string& path, size_t depth = 0)
    {
        require(depth < 64 && effect_dependency_states.size() < MAX_RECORDS, "Effect dependency graph exceeds limits");
        auto& state = effect_dependency_states[path];
        require(state != 1, "Cyclic required effect dependency: " + path);
        if(state == 2) return;
        state = 1;
        const json& elements = read_array(document, "elements");
        for(const json& dependency : read_array(document, "dependencies"))
        {
            require(dependency.at("required").is_boolean(), "Effect dependency required flag is invalid: " + path);
            const bool mandatory = dependency.at("required").get<bool>();
            const std::string kind = read_string(dependency, "kind");
            require(read_count(dependency, "elementIndex") < elements.size(), "Effect dependency owner is outside elements: " + path);
            if(kind == "texture")
            {
                const std::string output = dependency.value("textureRef", std::string{});
                require(!mandatory || !output.empty(), "Required effect texture has no converted output: " + path);
                if(!output.empty()) require_output(output);
            }
            else if(kind == "model")
            {
                const std::string model = dependency.value("modelRef", std::string{});
                require(!mandatory || !model.empty(), "Required effect model has no converted output: " + path);
                if(model.empty()) continue;
                require_output(model, "model");
                const std::string material_path = read_string(dependency, "materialRef");
                require_output(material_path, "material");
                const json material = read_document(request.content_root, material_path);
                require(material.at("format") == "EDS_MATERIAL" && read_count(material, "schemaVersion") == 1,
                        "Effect model material is invalid: " + path);
                validate_texture_references(material);
                const json& glb = read_glb(model);
                const uint64_t joints = read_count(dependency, "jointCount");
                const uint64_t frames = read_count(dependency, "frameCount");
                require(joints > 0 && glb.at("skins").size() == 1 && glb.at("skins").at(0).at("joints").size() == joints &&
                        dependency.at("generateSdf") == false && read_count(dependency, "lodCount") == 1,
                        "Effect model skeleton or compilation policy mismatch: " + path);
                const std::string action = read_string(dependency, "actionNameUtf8");
                if(!action.empty())
                {
                    const std::string animation = read_string(dependency, "animationRef");
                    require(is_safe_path(animation) && fs::path(animation).extension() == ".anim" &&
                            fs::path(animation).parent_path() == fs::path(model).parent_path() &&
                            read_string(dependency, "animationName") == "idle" &&
                            read_number(dependency, "animationDurationSeconds") > 0 && frames >= 2 &&
                            read_array(glb, "animations").size() == 1 && glb.at("animations").at(0).at("name") == "idle",
                            "Effect model lacks its authored action: " + path);
                }
                else
                    require(frames == 0 && !dependency.contains("animationRef"), "Rest-pose effect model invents an action: " + path);
            }
            else if(kind == "shader")
            {
                const std::string output = dependency.value("shaderRef", std::string{});
                require(!mandatory || !output.empty(), "Required effect shader has no converted output: " + path);
                if(!output.empty()) validate_effect_shader(dependency);
            }
            else if(kind == "gfx")
            {
                const std::string output = dependency.value("effectRef", std::string{});
                require(!mandatory || !output.empty(), "Required nested effect has no converted output: " + path);
                if(output.empty()) continue;
                require_output(output, "effect");
                const json child = read_document(request.content_root, output);
                const std::string id = read_string(child, "sourceId");
                require(id.size() == 68 && id.compare(0, 4, "gfx:") == 0 && is_hex(id.substr(4), 64) &&
                        output == "fx/dependencies/" + id.substr(4) + ".eds.effect.json" &&
                        child.at("format") == "EDS_EFFECT" && read_count(child, "schemaVersion") == 1 &&
                        child.at("complete") == true && child.at("sourceReadable") == true &&
                        read_array(child, "elements").size() == read_count(child, "declaredElementCount"),
                        "Nested effect identity or completeness mismatch: " + output);
                validate_effect_dependencies(child, output, depth + 1);
            }
            else
                require(!mandatory, "Unsupported required effect dependency kind: " + kind);
        }
        state = 2;
    }

    void validate_effects()
    {
        const json& section = manifest.at("effects");
        const id_set identities = validate_accounting(section.at("accounting"), section.at("identity").at("uniqueIds"), "effect", 1);
        std::map<std::string, const json*> entries;
        std::map<std::string, std::string> references;
        uint64_t declared = 0;
        for(const json& entry : read_array(section, "entries"))
        {
            const std::string id = read_string(entry, "sourceId");
            require(identities.count(id) != 0 && entries.emplace(id, &entry).second && entry.at("status") == "emitted",
                    "Unknown, duplicate or incomplete effect entry: " + id);
            const json& refs = read_array(entry, "references");
            require(!refs.empty(), "Effect source has no references: " + id);
            for(const json& ref : refs)
            {
                const std::string path = read_string(ref, "effect");
                require(ref.at("complete") == true && read_string(ref, "error").empty() && references.emplace(path, id).second,
                        "Effect output reference is incomplete or duplicated: " + id);
                require_output(path, "effect");
                const json document = read_document(request.content_root, path);
                require(document.at("format") == "EDS_EFFECT" && read_count(document, "schemaVersion") == 1 &&
                        document.at("sourceId") == id && document.at("complete") == true && document.at("sourceReadable") == true &&
                        normalize(read_string(document, "sourcePath")) == normalize(read_string(entry, "sourcePath")),
                        "Required effect output differs from inventory: " + path);
                require(read_array(document, "elements").size() == read_count(document, "declaredElementCount"),
                        "Required effect omits authored elements: " + path);
                validate_texture_references(document);
                validate_effect_dependencies(document, path);
                ++declared;
            }
        }
        require(entries.size() == identities.size(), "Required effect entry inventory is incomplete");
        std::map<std::string, json> signatures;
        id_set paths;
        uint64_t raw = 0;
        for(const json& record : result.scene_json.at("nodes"))
        {
            if(record.value("kind", "") != "Effect") continue;
            ++raw;
            const std::string id = read_string(record, "sourceId");
            require(identities.count(id) != 0 && id == make_id("effect", read_count(record, "exportId"), 0).substr(0, 15),
                    "Unknown or contradictory effect source ID: " + id);
            const json& open = record.at("openFormat");
            require(open.at("accountedFor") == true && open.at("convertedToOpenFormat") == true,
                    "Required effect is not converted: " + id);
            const std::string path = read_string(open.at("effectRef"), "effect");
            require(references.count(path) != 0 && references.at(path) == id && paths.insert(path).second,
                    "Scene effect reference is missing, duplicated or contradictory: " + id);
            require(normalize(read_string(record, "externalPath")) == normalize(read_string(*entries.at(id), "sourcePath")),
                    "Effect source path differs from inventory: " + id);
            const json& parameters = record.at("effectParameters");
            require(parameters.at("present") == true && read_number(parameters, "scale") > 0 &&
                    read_number(parameters, "playSpeed") >= 0 && read_number(parameters, "alpha") >= 0 &&
                    read_number(parameters, "alpha") <= 1 && parameters.at("validTime").is_number_integer(),
                    "Required authored effect parameters are invalid: " + id);
            json signature = get_placement(record);
            signature["parameters"] = parameters;
            const auto inserted = signatures.emplace(id, signature);
            require(inserted.second || inserted.first->second == signature, "Conflicting repeated effect source: " + id);
        }
        require(raw == declared && raw == read_count(section.at("accounting"), "rawReferences") &&
                signatures.size() == identities.size(), "Required effect reference closure is incomplete");
        result.effect_ids.assign(identities.begin(), identities.end());
    }

    void validate_terrain_document()
    {
        const json& terrain = manifest.at("terrain");
        const bool present = result.scene_json.at("terrainPresent").get<bool>();
        require(present, "Map scene has no terrain");
        validate_terrain(terrain, request);
        const json& open = result.scene_json.at("terrain").at("openFormat");
        require(open.at("accountedFor") == true && open.at("convertedToOpenFormat") == true, "Scene terrain is not converted");
        require_output(read_string(open, "heightmap"), "terrainHeightmap");
        require_output(read_string(open, "material"), "material");
        const std::string layers_path = read_string(open, "layers");
        require_output(layers_path, "terrainLayers");
        const json layers = read_document(request.content_root, layers_path);
        require(layers.at("format") == "EDS_TERRAIN" && read_count(layers, "schemaVersion") == 1, "Terrain layers format mismatch");
        const json& heightmap = layers.at("heightmap");
        require(heightmap.at("path") == open.at("heightmap") &&
                read_count(heightmap, "width") == read_count(terrain.at("emitted"), "sampleWidth") &&
                read_count(heightmap, "height") == read_count(terrain.at("emitted"), "sampleHeight"), "Terrain heightmap dimensions differ");
        require(require_output(read_string(heightmap, "path")).at("contentHash") == heightmap.at("contentHash"),
                "Terrain heightmap hash differs from output inventory");
        require(read_count(terrain, "emittedLayers") == read_array(layers, "layers").size() &&
                read_count(terrain, "emittedSplats") == read_array(layers, "splats").size(), "Terrain emitted layer or splat counts differ");
        const json& emitted = terrain.at("emitted");
        require(read_number(layers.at("world"), "widthM") == read_number(emitted, "rightM") - read_number(emitted, "leftM") &&
                read_number(layers.at("world"), "depthM") == read_number(emitted, "topM") - read_number(emitted, "bottomM"),
                "Terrain layers world extents differ from emitted window");
        for(const json& splat : layers.at("splats"))
        {
            require(require_output(read_string(splat, "path"), "terrainSplat").at("contentHash") == splat.at("contentHash"),
                    "Terrain splat hash differs from output inventory");
        }
        for(const json& layer : layers.at("layers"))
        {
            require_output(read_string(layer.at("splat"), "path"), "terrainSplat");
            if(layer.contains("albedo") && layer.at("albedo").contains("path"))
            {
                require_output(read_string(layer.at("albedo"), "path"));
            }
        }
        if(request.require_full)
            require(layers.contains("bakedAlbedo") && layers.at("bakedAlbedo").is_object(),
                    "Full map requires the composed terrain albedo; raw layers are not a complete material");
        if(layers.contains("bakedAlbedo"))
        {
            const json& baked = layers.at("bakedAlbedo");
            require(require_output(read_string(baked, "path"), "terrainBakedAlbedo").at("contentHash") == baked.at("contentHash"),
                    "Terrain baked albedo hash differs from output inventory");
        }
    }

    auto validate() -> pw_map_manifest_result
    {
        validate_identity();
        validate_outputs();
        validate_documents();
        validate_inventories();
        validate_models();
        validate_buildings();
        validate_water();
        if(!result.legacy)
        {
            const id_set grass = validate_model_nodes("grass", "Grass", "grass", 2);
            const id_set ecmodels = validate_model_nodes("ecmodels", "ECModel", "ecmodel", 1);
            result.grass_ids.assign(grass.begin(), grass.end());
            result.ecmodel_ids.assign(ecmodels.begin(), ecmodels.end());
            validate_effects();
            if(request.require_full && request.slug == "a61")
                require(!grass.empty() && !ecmodels.empty() && !result.effect_ids.empty(),
                        "Full a61 requires grass, ECModel and effect inventories");
        }
        if(manifest.contains("terrain") && !manifest.at("terrain").is_null())
        {
            validate_terrain_document();
        }
        else
        {
            require(!request.require_full && !result.scene_json.value("terrainPresent", false), "Required terrain contract is missing");
        }
        result.manifest_json = manifest;
        std::sort(result.required_outputs.begin(), result.required_outputs.end());
        result.required_outputs.erase(std::unique(result.required_outputs.begin(), result.required_outputs.end()), result.required_outputs.end());
        result.valid = true;
        return std::move(result);
    }
};

void validate_request(const pw_map_manifest_request& request)
{
    require(!request.slug.empty() && request.slug.size() <= 64 && std::all_of(request.slug.begin(), request.slug.end(),
            [](char value) { return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '_' || value == '-'; }),
            "Requested map slug must be a normalized token");
    require(fs::is_directory(request.content_root), "Candidate content root does not exist");
}
} // namespace

auto validate_pw_map_manifest(const pw_map_manifest_request& request, const nlohmann::json& manifest) -> pw_map_manifest_result
{
    try
    {
        validate_request(request);
        return manifest_validator{request, manifest}.validate();
    }
    catch(const std::exception& exception)
    {
        pw_map_manifest_result result;
        result.error = exception.what();
        return result;
    }
}

auto validate_pw_map_manifest(const pw_map_manifest_request& request) -> pw_map_manifest_result
{
    try
    {
        validate_request(request);
        const std::string relative = "maps/" + request.slug + "/map.manifest.json";
        if(!fs::exists(request.content_root / relative) && request.allow_legacy && !request.require_full)
        {
            pw_map_manifest_result result;
            result.valid = true;
            result.legacy = true;
            return result;
        }
        const json manifest = read_document(request.content_root, relative);
        return validate_pw_map_manifest(request, manifest);
    }
    catch(const std::exception& exception)
    {
        pw_map_manifest_result result;
        result.error = exception.what();
        return result;
    }
}
} // namespace unravel
