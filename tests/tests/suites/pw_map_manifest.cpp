#include "../tests.h"

#include <engine/pw/pw_map_manifest.h>
#include <engine/pw/detail/blake3.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>

using namespace unravel;

namespace
{
using json = nlohmann::json;
int g_checks = 0;
int g_failures = 0;
const std::string FIRST_BUILDING = "ornament:00000001:00000002";
const std::string SECOND_BUILDING = "ornament:00000003:00000004";
const std::string FIRST_WATER = "water:00000001:00000002";
const std::string SECOND_WATER = "water:00000003:00000004";
const std::string WATER_SURFACE = "water-surface:00000000:00000000:00000001";
const std::string PLACEHOLDER_HASH(64, 'a');

void check(bool condition, const std::string& message)
{
    ++g_checks;
    if(!condition)
    {
        ++g_failures;
        std::printf("  FAIL: %s\n", message.c_str());
    }
}

auto hash_text(const std::string& text) -> std::string
{
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, text.data(), text.size());
    std::array<uint8_t, BLAKE3_OUT_LEN> bytes{};
    blake3_hasher_finalize(&hasher, bytes.data(), bytes.size());
    const char* digits = "0123456789abcdef";
    std::string output;
    for(const uint8_t byte : bytes)
    {
        output.push_back(digits[byte >> 4]);
        output.push_back(digits[byte & 15]);
    }
    return output;
}

auto make_accounting(int raw, int unique) -> json
{
    return {{"rawReferences", raw}, {"uniqueSources", unique}, {"duplicateReferences", raw - unique},
            {"emitted", unique}, {"failed", 0}, {"deferred", 0}};
}

struct candidate_fixture
{
    fs::path root;
    pw_map_manifest_request request;
    json manifest;
    json scene;
    json meta;
    json sources;
    std::map<std::string, std::string> payloads;
    std::map<std::string, std::string> kinds;

    candidate_fixture()
    {
        static uint64_t sequence = 0;
        const int64_t timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("unravel-pw-manifest-" + std::to_string(timestamp) + "-" + std::to_string(++sequence));
        fs::create_directories(root);
        request.content_root = root;
        request.slug = "fixture";
        request.instance_id = 7;
        request.expected_lineage = "fixture-lineage";
        initialize();
    }

    ~candidate_fixture()
    {
        std::error_code error;
        fs::remove_all(root, error);
    }

    void write(const std::string& relative, const std::string& text)
    {
        const fs::path path = root / relative;
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    void add(const std::string& path, const std::string& kind, const std::string& bytes)
    {
        payloads[path] = bytes;
        kinds[path] = kind;
    }

    auto make_building(const std::string& id, int first, int second) -> json
    {
        return {{"sourceId", id}, {"exportId", first}, {"bsdOffset", second},
                {"pos", {1, 2, 3}}, {"dir", {0, 0, 1}}, {"up", {0, 1, 0}},
                {"externalPath", "LitModels\\fixture.bmd"},
                {"sourceRefs", json::array({{{"block", 0}, {"localIndex", 0}}})},
                {"openFormat", {{"accountedFor", true}, {"convertedToOpenFormat", true},
                    {"model", "models/buildings/000000.glb"}, {"material", "materials/buildings/000000.eds.mat.json"},
                    {"logicalId", "building_000000"}}}};
    }

    auto make_water(const std::string& id, int first, int second, const std::string& payload) -> json
    {
        const json area = {{"subTerrain", 0}, {"dataIndex", 0}, {"areaId", 1}, {"waterHeight", 1},
                           {"center", {0, 1, 0}}, {"width", 1}, {"height", 1}, {"hasSurface", true}};
        return {{"kind", "Water"}, {"sourceId", id}, {"exportId", first}, {"bsdOffset", second},
                {"pos", {0, 1, 0}}, {"dir", {0, 0, 1}}, {"up", {0, 1, 0}}, {"water", area},
                {"payload", payload}, {"openFormat", {{"accountedFor", true}, {"convertedToOpenFormat", true}, {"surface", payload}}}};
    }

    auto make_water_payload(const json& node) -> json
    {
        json document = node;
        document["waterSurface"] = {{"format", "EDS_WATER_SURFACE"}, {"schemaVersion", 1}, {"sourceId", WATER_SURFACE},
                                    {"logicalId", "water_000000"}, {"area", node.at("water")},
                                    {"mesh", {{"vertices", json::array({{0, 1, 0, 0}, {1, 1, 0, 0}, {0, 1, 1, 0}})},
                                               {"indices", {0, 1, 2}}}}, {"material", {{"logicalId", "water_000000"}}}};
        return document;
    }

    void initialize()
    {
        json accounting = make_accounting(3, 2);
        accounting.update({{"buildingInstances", 3}, {"sceneNodes", 3}, {"uniqueModelAssets", 1},
                           {"uniqueMaterialAssets", 1}, {"failures", json::array()}});
        manifest = {{"format", "PW_MAP"}, {"schemaVersion", 2}, {"converterVersion", "eds-converter-v7"},
                    {"status", "generated"}, {"slug", "fixture"}, {"instanceId", 7}, {"lineage", "fixture-lineage"},
                    {"sourceMap", "maps/fixture/fixture.ecwld"},
                    {"sources", json::array({{{"path", "maps/fixture/fixture.ecwld"}, {"contentHash", PLACEHOLDER_HASH}, {"required", true}}})},
                    {"scene", {{"accounting", accounting}, {"identity", {{"uniqueIds", {FIRST_BUILDING, SECOND_BUILDING}}}}}},
                    {"water", {{"accounting", {{"entities", make_accounting(3, 2)}, {"surfaces", make_accounting(3, 1)},
                                              {"records", 3}, {"convertedSurfaces", 3}, {"failures", json::array()}}},
                               {"identity", {{"uniqueIds", {FIRST_WATER, SECOND_WATER}}, {"surfaceIds", {WATER_SURFACE}}}}}},
                    {"assets", {{"models", {{"uniqueAssets", 1}, {"entries", json::array({{
                            {"sourcePath", "litmodels/fixture.bmd"}, {"contentHash", PLACEHOLDER_HASH}, {"identityHash", PLACEHOLDER_HASH},
                            {"modelPath", "models/buildings/000000.glb"}, {"materialPath", "materials/buildings/000000.eds.mat.json"},
                            {"referencedBy", {FIRST_BUILDING, SECOND_BUILDING}}}})}}},
                                {"textures", {{"emitted", 1}, {"classified", json::array()},
                                              {"classifiedCounts", {{"missing", 0}, {"corrupt", 0}, {"unsupported", 0}}}}}}}};
        for(const char* section : {"grass", "ecmodels", "effects"})
            manifest[section] = {{"accounting", make_accounting(0, 0)}, {"identity", {{"uniqueIds", json::array()}}}, {"entries", json::array()}};
        manifest["grass"]["accounting"]["bladeCount"] = 0;
        const json first = make_building(FIRST_BUILDING, 1, 2);
        const json second = make_building(SECOND_BUILDING, 3, 4);
        const json water_first = make_water(FIRST_WATER, 1, 2, "water/000000.eds.water.json");
        const json water_second = make_water(SECOND_WATER, 3, 4, "water/000001.eds.water.json");
        scene = {{"format", "EDS"}, {"schemaVersion", 1}, {"mapName", "fixture"}, {"sourceMap", "maps/fixture/fixture.ecwld"},
                 {"terrainPresent", false}, {"buildings", json::array({first, first, second})},
                 {"nodes", json::array({water_first, water_first, water_second})}};
        meta = {{"format", "EDS"}, {"schemaVersion", 1}, {"status", "generated"}, {"layout", "content/maps"},
                {"mapName", "fixture"}, {"sourceMap", "maps/fixture/fixture.ecwld"}, {"instanceId", 7},
                {"files", {{"scene", "maps/fixture/scene.eds.json"}, {"sourceManifest", "_meta/source.manifest.json"}}},
                {"counts", {{"buildings", 3}, {"nodes", 3}}}, {"contentAddressing", {{"hashAlgorithm", "BLAKE3"}}},
                {"conversionCache", {{"converterVersion", "eds-converter-v7"}, {"handedness", "left-handed"}, {"optionsHash", PLACEHOLDER_HASH}}}};
        sources = {{"format", "EDS_SOURCE_MANIFEST"}, {"schemaVersion", 2}, {"converterVersion", "eds-converter-v7"},
                   {"records", json::array({{{"sourcePath", "maps/fixture/fixture.ecwld"}, {"sourceHash", PLACEHOLDER_HASH},
                                            {"converterVersion", "eds-converter-v7"}, {"optionsHash", PLACEHOLDER_HASH}}})}};
        const json material = {{"format", "EDS_MATERIAL"}, {"schemaVersion", 1}, {"textureRef", "textures/pixel.ktx2"}};
        add("models/buildings/000000.glb", "model", "synthetic model payload");
        add("materials/buildings/000000.eds.mat.json", "material", material.dump());
        add("textures/pixel.ktx2", "texture", "abc");
        add("maps/fixture/water/000000.eds.water.json", "water", make_water_payload(water_first).dump());
        add("maps/fixture/water/000001.eds.water.json", "water", make_water_payload(water_second).dump());
        publish();
    }

    void add_effect()
    {
        const std::string id = "effect:00000009";
        const std::string path = "fx/000000.eds.effect.json";
        manifest["effects"] = {{"identity", {{"uniqueIds", {id}}}}, {"accounting", make_accounting(1, 1)},
            {"entries", json::array({{{"sourceId", id}, {"sourcePath", "gfx/fixture.gfx"}, {"status", "emitted"},
                {"references", json::array({{{"effect", path}, {"complete", true}, {"error", ""}}})}}})}};
        scene["nodes"].push_back({{"kind", "Effect"}, {"sourceId", id}, {"exportId", 9},
            {"pos", {0, 0, 0}}, {"dir", {0, 0, 1}}, {"up", {0, 1, 0}}, {"externalPath", "gfx/fixture.gfx"},
            {"effectParameters", {{"present", true}, {"scale", 1}, {"playSpeed", 1}, {"alpha", 1}, {"validTime", 0}}},
            {"openFormat", {{"accountedFor", true}, {"convertedToOpenFormat", true}, {"effectRef", {{"effect", path}}}}}});
        manifest["scene"]["accounting"]["sceneNodes"] = scene["nodes"].size();
        meta["counts"]["nodes"] = scene["nodes"].size();
        add(path, "effect", json({{"format", "EDS_EFFECT"}, {"schemaVersion", 1}, {"sourceId", id},
            {"sourcePath", "gfx/fixture.gfx"}, {"complete", true}, {"sourceReadable", true},
            {"declaredElementCount", 1}, {"elements", json::array({{{"type", 100}}})},
            {"dependencies", json::array()}}).dump());
        publish();
    }

    void add_full_terrain()
    {
        manifest["terrain"] = {{"mode", "full"}, {"expectedFull", true}, {"coverageRatio", 1.0},
            {"sourceLayers", 0}, {"emittedLayers", 0}, {"sourceMaskAreas", 0}, {"emittedSplats", 0}, {"skipped", json::array()},
            {"native", {{"blockCols", 1}, {"blockRows", 1}, {"blockGrid", 1}, {"blockSizeM", 4},
                        {"blockCount", 1}, {"widthM", 4}, {"depthM", 4}, {"leftM", -2}, {"rightM", 2},
                        {"topM", 2}, {"bottomM", -2}, {"sampleWidth", 2}, {"sampleHeight", 2}}},
            {"window", {{"colBegin", 0}, {"colEnd", 1}, {"rowBegin", 0}, {"rowEnd", 1}, {"cols", 1}, {"rows", 1}, {"blockCount", 1}}},
            {"emitted", {{"leftM", -2}, {"rightM", 2}, {"topM", 2}, {"bottomM", -2}, {"sampleWidth", 2}, {"sampleHeight", 2}}}};
        scene["terrainPresent"] = true;
        scene["terrain"] = {{"openFormat", {{"accountedFor", true}, {"convertedToOpenFormat", true},
             {"heightmap", "terrain/height.png"}, {"layers", "terrain/layers.json"}, {"material", "terrain/material.json"}}}};
        const json layers = {{"format", "EDS_TERRAIN"}, {"schemaVersion", 1},
             {"heightmap", {{"path", "terrain/height.png"}, {"contentHash", hash_text("height bytes")}, {"width", 2}, {"height", 2}}},
             {"world", {{"widthM", 4}, {"depthM", 4}}}, {"layers", json::array()}, {"splats", json::array()},
             {"bakedAlbedo", {{"path", "terrain/albedo.ktx2"}, {"contentHash", hash_text("albedo bytes")}}}};
        add("terrain/albedo.ktx2", "terrainBakedAlbedo", "albedo bytes");
        add("terrain/height.png", "terrainHeightmap", "height bytes");
        add("terrain/layers.json", "terrainLayers", layers.dump());
        add("terrain/material.json", "material", "{}");
        request.require_full = true;
        publish();
    }

    void add_grass()
    {
        const std::string id = "grass:00000005:00000006";
        const std::string model = "models/grass/fixture.glb";
        const std::string material = "materials/grass/fixture.eds.mat.json";
        const json entry = {{"sourceId", id}, {"rawReferences", 2}, {"model", model}, {"material", material},
            {"position", {10, 0, 20}}, {"bladeCount", 2}, {"typeId", 0}, {"generateSdf", false}, {"lodCount", 1}};
        manifest["grass"] = {{"identity", {{"uniqueIds", {id}}}}, {"accounting", make_accounting(2, 1)}, {"entries", json::array({entry})}};
        manifest["grass"]["accounting"]["bladeCount"] = 2;
        json open = entry;
        open.update({{"accountedFor", true}, {"convertedToOpenFormat", true}, {"logicalId", "grass_fixture"},
                     {"alphaCutoff", 84.0 / 255.0}, {"windAnimation", "static-base-pose"}});
        json node = {{"sourceId", id}, {"kind", "Grass"}, {"exportId", 5}, {"bsdOffset", 6},
                     {"pos", {10, 4, 20}}, {"dir", {0, 0, 1}}, {"up", {0, 1, 0}},
                     {"externalPath", "maps/fixture/BSData0.dat"}, {"payload", "grass/000000.eds.grass.json"}, {"openFormat", open}};
        scene["nodes"].push_back(node);
        scene["nodes"].push_back(node);
        manifest["scene"]["accounting"]["sceneNodes"] = scene["nodes"].size();
        meta["counts"]["nodes"] = scene["nodes"].size();
        add("maps/fixture/grass/000000.eds.grass.json", "grass", node.dump());
        add(material, "material", json({{"format", "EDS_MATERIAL"}, {"schemaVersion", 1}, {"textureRef", "textures/pixel.ktx2"}}).dump());
        const json glb = {{"asset", {{"version", "2.0"}, {"generator", "A3DMapEditor EDS converter"}}},
            {"scene", 0}, {"scenes", json::array({{{"nodes", {0}}, {"extras", {{"pwMeshUsage", "grass"}}}}})},
            {"nodes", json::array({{{"mesh", 0}}})},
            {"meshes", json::array({{{"primitives", json::array({{{"attributes", {{"POSITION", 0}}}, {"indices", 1}, {"mode", 4}}})}}})},
            {"buffers", json::array({{{"byteLength", 96}}})},
            {"bufferViews", json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 72}},
                                         {{"buffer", 0}, {"byteOffset", 72}, {"byteLength", 24}}})},
            {"accessors", json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", 6}, {"type", "VEC3"}},
                                       {{"bufferView", 1}, {"componentType", 5125}, {"count", 6}, {"type", "SCALAR"}}})}};
        std::string text = glb.dump();
        while(text.size() % 4 != 0) text.push_back(' ');
        std::string bytes;
        const auto append = [&bytes](uint32_t value)
        {
            for(int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<char>(value >> shift));
        };
        append(0x46546c67); append(2); append(static_cast<uint32_t>(20 + text.size() + 8 + 96));
        append(static_cast<uint32_t>(text.size())); append(0x4e4f534a); bytes += text;
        append(96); append(0x004e4942);
        for(uint32_t vertex = 0; vertex < 6; ++vertex)
        {
            append(vertex % 3 == 1 ? 0x3f800000 : 0);
            append(vertex % 3 == 2 ? 0x3f800000 : 0);
            append(vertex < 3 ? 0 : 0x3f800000);
        }
        for(uint32_t index = 0; index < 6; ++index) append(index);
        add(model, "model", bytes);
        publish();
    }

    void publish()
    {
        add("maps/fixture/scene.eds.json", "scene", scene.dump());
        manifest["outputs"] = json::array();
        for(const std::pair<const std::string, std::string>& item : payloads)
        {
            write(item.first, item.second);
            manifest["outputs"].push_back({{"path", item.first}, {"kind", kinds.at(item.first)}, {"contentHash", hash_text(item.second)}});
        }
        meta["generatedResources"] = {{"count", manifest.at("outputs").size()}, {"records", manifest.at("outputs")}};
        write("_meta/manifest.eds.json", meta.dump());
        write("_meta/source.manifest.json", sources.dump());
        write("maps/fixture/map.manifest.json", manifest.dump());
    }

    auto validate() -> pw_map_manifest_result
    {
        return validate_pw_map_manifest(request, manifest);
    }
};

void expect_rejection(const std::string& name, const std::function<void(candidate_fixture&)>& change)
{
    candidate_fixture fixture;
    change(fixture);
    const pw_map_manifest_result result = fixture.validate();
    check(!result.valid && !result.error.empty(), name + " rejected");
}

void test_source_and_output_contract()
{
    check(hash_text("") == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262", "BLAKE3 empty reference vector");
    check(hash_text("abc") == "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85", "BLAKE3 abc reference vector");
    candidate_fixture fixture;
    const pw_map_manifest_result result = fixture.validate();
    check(result.valid, "valid v7 fixture: " + result.error);
    check(result.building_ids.size() == 2, "distinct colocated buildings sharing a resource survive");
    check(result.water_ids.size() == 2 && result.water_surface_ids.size() == 1, "distinct water entities share one surface");
    check(result.water_surface_by_id.size() == 2, "water entity to surface relation exported");
    check(validate_pw_map_manifest(fixture.request).valid, "file-reading manifest overload accepts fixture");
    fixture.manifest["sourceMap"] = "Maps/Fixture/Fixture.ecwld";
    check(fixture.validate().valid, "source map identity matches normalized VFS spelling");
    expect_rejection("schema mismatch", [](candidate_fixture& candidate) { candidate.manifest["schemaVersion"] = 1; });
    expect_rejection("converter mismatch", [](candidate_fixture& candidate) { candidate.manifest["converterVersion"] = "unknown"; });
    expect_rejection("wrong source map", [](candidate_fixture& candidate) { candidate.manifest["sourceMap"] = "maps/other/other.ecwld"; });
    expect_rejection("wrong instance", [](candidate_fixture& candidate) { candidate.manifest["instanceId"] = 8; });
    expect_rejection("wrong lineage", [](candidate_fixture& candidate) { candidate.manifest["lineage"] = "another-lineage"; });
    expect_rejection("missing required source binding", [](candidate_fixture& candidate) { candidate.manifest["sources"][0]["required"] = false; });
    expect_rejection("source digest malformed", [](candidate_fixture& candidate) { candidate.manifest["sources"][0]["contentHash"] = "broken"; });
    expect_rejection("output digest malformed", [](candidate_fixture& candidate) { candidate.manifest["outputs"][0]["contentHash"] = "broken"; });
    expect_rejection("missing physical output", [](candidate_fixture& candidate) { fs::remove(candidate.root / "models/buildings/000000.glb"); });
    expect_rejection("changed physical output", [](candidate_fixture& candidate) { candidate.write("textures/pixel.ktx2", "abd"); });
    expect_rejection("duplicate output", [](candidate_fixture& candidate) { candidate.manifest["outputs"].push_back(candidate.manifest["outputs"][0]); });
    expect_rejection("parent traversal", [](candidate_fixture& candidate) { candidate.manifest["outputs"][0]["path"] = "../escape"; });
    expect_rejection("absolute output", [](candidate_fixture& candidate) { candidate.manifest["outputs"][0]["path"] = "C:/escape"; });
    expect_rejection("omitted scene hash", [](candidate_fixture& candidate)
    {
        json& outputs = candidate.manifest["outputs"];
        outputs.erase(std::remove_if(outputs.begin(), outputs.end(), [](const json& output) { return output.at("kind") == "scene"; }), outputs.end());
    });
    expect_rejection("source manifest disagreement", [](candidate_fixture& candidate)
    {
        candidate.sources["records"][0]["sourceHash"] = std::string(64, 'b');
        candidate.publish();
    });
    fixture.write("maps/fixture/map.manifest.json", "{\"format\":\"PW_MAP\",\"format\":\"PW_MAP\"}");
    const pw_map_manifest_result duplicate_keys = validate_pw_map_manifest(fixture.request);
    check(!duplicate_keys.valid && duplicate_keys.error.find("Duplicate JSON key") != std::string::npos, "duplicate JSON keys rejected before use");
}

void test_identity_and_reference_closure()
{
    candidate_fixture shared;
    json alias = json::parse(shared.payloads.at("maps/fixture/water/000001.eds.water.json"));
    alias["waterSurface"].erase("mesh");
    alias["waterSurface"]["logicalId"] = "water_000001";
    alias["waterSurface"]["sharedSurface"] = {{"payload", "water/000000.eds.water.json"}, {"logicalId", "water_000000"}};
    shared.payloads["maps/fixture/water/000001.eds.water.json"] = alias.dump();
    shared.publish();
    const pw_map_manifest_result shared_result = shared.validate();
    check(shared_result.valid, "shared water payload resolves canonical mesh: " + shared_result.error);
    expect_rejection("changed raw accounting", [](candidate_fixture& candidate) { candidate.manifest["scene"]["accounting"]["rawReferences"] = 4; });
    expect_rejection("duplicate inventoried identity", [](candidate_fixture& candidate) { candidate.manifest["scene"]["identity"]["uniqueIds"][1] = FIRST_BUILDING; });
    expect_rejection("unaccounted required identity", [](candidate_fixture& candidate) { candidate.manifest["scene"]["identity"]["uniqueIds"][1] = "ornament:00000005:00000006"; });
    expect_rejection("missing model reference", [](candidate_fixture& candidate) { candidate.manifest["assets"]["models"]["entries"][0]["referencedBy"].erase(1); });
    expect_rejection("conflicting model reference", [](candidate_fixture& candidate) { candidate.manifest["assets"]["models"]["entries"][0]["referencedBy"].push_back(FIRST_BUILDING); });
    expect_rejection("missing required scene identity", [](candidate_fixture& candidate)
    {
        candidate.scene["buildings"][2] = candidate.scene["buildings"][0];
        candidate.publish();
    });
    expect_rejection("same ID conflicting placement", [](candidate_fixture& candidate)
    {
        candidate.scene["buildings"][1]["pos"][0] = 100;
        candidate.publish();
    });
    expect_rejection("nonfinite placement", [](candidate_fixture& candidate)
    {
        candidate.scene["buildings"][1]["pos"][0] = nullptr;
        candidate.publish();
    });
    expect_rejection("scene model mismatch", [](candidate_fixture& candidate)
    {
        candidate.scene["buildings"][0]["openFormat"]["model"] = "models/unrelated.glb";
        candidate.publish();
    });
    expect_rejection("mandatory deferred building", [](candidate_fixture& candidate)
    {
        candidate.manifest["scene"]["accounting"]["emitted"] = 1;
        candidate.manifest["scene"]["accounting"]["deferred"] = 1;
    });
    expect_rejection("material dangling texture", [](candidate_fixture& candidate)
    {
        json material = json::parse(candidate.payloads.at("materials/buildings/000000.eds.mat.json"));
        material["textureRef"] = "textures/missing.ktx2";
        candidate.payloads["materials/buildings/000000.eds.mat.json"] = material.dump();
        candidate.publish();
    });
    expect_rejection("water payload digest corruption", [](candidate_fixture& candidate) { candidate.write("maps/fixture/water/000000.eds.water.json", "{}"); });
    expect_rejection("water surface identity disagreement", [](candidate_fixture& candidate)
    {
        json water = json::parse(candidate.payloads.at("maps/fixture/water/000000.eds.water.json"));
        water["waterSurface"]["sourceId"] = "water-surface:00000000:00000000:00000002";
        candidate.payloads["maps/fixture/water/000000.eds.water.json"] = water.dump();
        candidate.publish();
    });
    expect_rejection("water vertex buffer corruption", [](candidate_fixture& candidate)
    {
        json water = json::parse(candidate.payloads.at("maps/fixture/water/000000.eds.water.json"));
        water["waterSurface"]["mesh"]["indices"][0] = 100;
        candidate.payloads["maps/fixture/water/000000.eds.water.json"] = water.dump();
        candidate.publish();
    });
    expect_rejection("water shared reference cycle", [](candidate_fixture& candidate)
    {
        json water = json::parse(candidate.payloads.at("maps/fixture/water/000000.eds.water.json"));
        water["waterSurface"].erase("mesh");
        water["waterSurface"]["sharedSurface"] = {{"payload", "water/000000.eds.water.json"}, {"logicalId", "water_000000"}};
        candidate.payloads["maps/fixture/water/000000.eds.water.json"] = water.dump();
        candidate.publish();
    });
}

void test_grass_contract()
{
    candidate_fixture valid;
    valid.add_grass();
    const auto result = valid.validate();
    check(result.valid && result.grass_ids.size() == 1 && result.grass_blade_count == 2,
          "v7 grass GLB, repeated source and payload closure: " + result.error);
    const auto reject = [](const char* name, const std::function<void(candidate_fixture&)>& mutate)
    {
        candidate_fixture fixture;
        fixture.add_grass();
        mutate(fixture);
        check(!fixture.validate().valid, name);
    };
    reject("missing mandatory grass inventory rejected", [](candidate_fixture& fixture) { fixture.manifest.erase("grass"); });
    reject("missing mandatory ECModel inventory rejected", [](candidate_fixture& fixture) { fixture.manifest.erase("ecmodels"); });
    reject("missing mandatory effect inventory rejected", [](candidate_fixture& fixture) { fixture.manifest.erase("effects"); });
    reject("grass blades accounting mismatch rejected", [](candidate_fixture& fixture)
    {
        fixture.manifest["grass"]["accounting"]["bladeCount"] = 3;
    });
    reject("grass raw source accounting mismatch rejected", [](candidate_fixture& fixture)
    {
        fixture.manifest["grass"]["entries"][0]["rawReferences"] = 1;
    });
    reject("grass compiler policy contradiction rejected", [](candidate_fixture& fixture)
    {
        fixture.manifest["grass"]["entries"][0]["generateSdf"] = true;
    });
    reject("grass source identity contradiction rejected", [](candidate_fixture& fixture)
    {
        fixture.scene["nodes"][3]["bsdOffset"] = 7;
        fixture.publish();
    });
    reject("grass center mismatch rejected", [](candidate_fixture& fixture)
    {
        fixture.scene["nodes"][3]["openFormat"]["position"][1] = 4;
        fixture.publish();
    });
    reject("grass payload open-format mismatch rejected", [](candidate_fixture& fixture)
    {
        json document = json::parse(fixture.payloads.at("maps/fixture/grass/000000.eds.grass.json"));
        document["openFormat"]["bladeCount"] = 3;
        fixture.payloads["maps/fixture/grass/000000.eds.grass.json"] = document.dump();
        fixture.publish();
    });
    reject("required grass GLB header malformed rejected", [](candidate_fixture& fixture)
    {
        fixture.payloads["models/grass/fixture.glb"][0] = 'x';
        fixture.publish();
    });
    reject("required grass importer policy absent rejected", [](candidate_fixture& fixture)
    {
        auto& bytes = fixture.payloads["models/grass/fixture.glb"];
        bytes.replace(bytes.find("pwMeshUsage"), 11, "notTheUsage");
        fixture.publish();
    });
    reject("required grass output omitted rejected", [](candidate_fixture& fixture)
    {
        fixture.payloads.erase("maps/fixture/grass/000000.eds.grass.json");
        fixture.publish();
    });
}

void test_full_geometry_and_legacy_policy()
{
    candidate_fixture full;
    full.add_full_terrain();
    const pw_map_manifest_result result = full.validate();
    check(result.valid, "full generic map accepted: " + result.error);
    expect_rejection("full request without terrain", [](candidate_fixture& candidate) { candidate.request.require_full = true; });
    expect_rejection("full terrain cannot substitute a raw layer for its composed albedo", [](candidate_fixture& candidate)
    {
        candidate.add_full_terrain();
        auto layers = json::parse(candidate.payloads.at("terrain/layers.json"));
        layers.erase("bakedAlbedo");
        candidate.payloads["terrain/layers.json"] = layers.dump();
    });
    expect_rejection("full window with false expectedFull", [](candidate_fixture& candidate)
    {
        candidate.add_full_terrain();
        candidate.manifest["terrain"]["expectedFull"] = false;
    });
    expect_rejection("full request for regional mode", [](candidate_fixture& candidate)
    {
        candidate.add_full_terrain();
        candidate.manifest["terrain"]["mode"] = "region";
    });
    expect_rejection("emitted samples contradict grid", [](candidate_fixture& candidate)
    {
        candidate.add_full_terrain();
        candidate.manifest["terrain"]["emitted"]["sampleWidth"] = 3;
    });
    expect_rejection("fractional integer window", [](candidate_fixture& candidate)
    {
        candidate.add_full_terrain();
        candidate.manifest["terrain"]["window"]["colEnd"] = 1.0;
    });
    expect_rejection("changed native extent", [](candidate_fixture& candidate)
    {
        candidate.add_full_terrain();
        candidate.manifest["terrain"]["native"]["rightM"] = 3;
    });
    expect_rejection("implicit v4 candidate", [](candidate_fixture& candidate) { candidate.manifest["converterVersion"] = "eds-converter-v4"; });
    candidate_fixture version_four;
    version_four.request.allow_legacy = true;
    version_four.manifest["converterVersion"] = "eds-converter-v4";
    version_four.meta["conversionCache"]["converterVersion"] = "eds-converter-v4";
    version_four.sources["converterVersion"] = "eds-converter-v4";
    version_four.sources["records"][0]["converterVersion"] = "eds-converter-v4";
    version_four.publish();
    const pw_map_manifest_result legacy_version = version_four.validate();
    check(legacy_version.valid && legacy_version.legacy, "v4 explicit compatibility is reported as legacy: " + legacy_version.error);
    version_four.request.require_full = true;
    check(!version_four.validate().valid, "v4 compatibility cannot satisfy full map request");
    candidate_fixture version_five;
    version_five.manifest["converterVersion"] = "eds-converter-v5";
    version_five.meta["conversionCache"]["converterVersion"] = "eds-converter-v5";
    version_five.sources["converterVersion"] = "eds-converter-v5";
    version_five.sources["records"][0]["converterVersion"] = "eds-converter-v5";
    version_five.publish();
    check(!version_five.validate().valid, "v5 missing full renderable inventory needs explicit compatibility");
    version_five.request.allow_legacy = true;
    check(version_five.validate().valid && version_five.validate().legacy, "v5 compatibility reports legacy scope");
    version_five.request.require_full = true;
    check(!version_five.validate().valid, "v5 compatibility cannot satisfy full map request");
    candidate_fixture version_six;
    version_six.manifest["converterVersion"] = "eds-converter-v6";
    version_six.meta["conversionCache"]["converterVersion"] = "eds-converter-v6";
    version_six.sources["converterVersion"] = "eds-converter-v6";
    version_six.sources["records"][0]["converterVersion"] = "eds-converter-v6";
    version_six.publish();
    check(!version_six.validate().valid, "v6 cannot implicitly bypass required GFX dependency closure");
    version_six.request.allow_legacy = true;
    const auto legacy_six = version_six.validate();
    check(legacy_six.valid && legacy_six.legacy, "v6 compatibility reports historical scope");
    version_six.request.require_full = true;
    check(!version_six.validate().valid, "v6 compatibility cannot satisfy full map request");
    candidate_fixture legacy;
    legacy.request.allow_legacy = true;
    fs::remove(legacy.root / "maps/fixture/map.manifest.json");
    const pw_map_manifest_result compatibility = validate_pw_map_manifest(legacy.request);
    check(compatibility.valid && compatibility.legacy, "missing manifest requires and reports explicit legacy compatibility");
    legacy.request.require_full = true;
    check(!validate_pw_map_manifest(legacy.request).valid, "legacy bypass cannot satisfy full map request");
}

void test_effect_dependency_closure()
{
    candidate_fixture fixture;
    fixture.add_effect();
    check(fixture.validate().valid, "v7 effect without external dependencies is valid");
    const std::string root = "fx/000000.eds.effect.json";
    const std::string child_path = "fx/dependencies/" + PLACEHOLDER_HASH + ".eds.effect.json";
    json root_doc = json::parse(fixture.payloads.at(root));
    json child = root_doc;
    child["sourceId"] = "gfx:" + PLACEHOLDER_HASH;
    child["dependencies"] = json::array({{{"kind", "texture"}, {"required", true}, {"elementIndex", 0}, {"textureRef", "textures/pixel.ktx2"}}});
    const json ref = {{"kind", "gfx"}, {"required", true}, {"elementIndex", 0}, {"effectRef", child_path}};
    root_doc["dependencies"] = json::array({ref, ref});
    fixture.add(root, "effect", root_doc.dump());
    fixture.add(child_path, "effect", child.dump());
    fixture.publish();
    check(fixture.validate().valid, "shared nested effect DAG closes over hash-bound texture output");
    child["dependencies"][0].erase("textureRef");
    fixture.add(child_path, "effect", child.dump()); fixture.publish();
    check(!fixture.validate().valid, "nested required texture cannot omit converted reference");
    child["dependencies"] = json::array({ref});
    fixture.add(child_path, "effect", child.dump()); fixture.publish();
    const auto cyclic = fixture.validate();
    check(!cyclic.valid && cyclic.error.find("Cyclic") != std::string::npos, "required nested effect cycle rejected");
    child["dependencies"] = json::array({{{"kind", "model"}, {"required", true}, {"elementIndex", 0}}});
    fixture.add(child_path, "effect", child.dump()); fixture.publish();
    check(!fixture.validate().valid, "required native effect model cannot omit converted geometry");
    child["dependencies"] = json::array();
    fixture.add(child_path, "effect", child.dump()); fixture.publish();
    check(fixture.validate().valid, "dependency validation recovers for corrected candidate");
    fixture.payloads.erase(child_path); fixture.kinds.erase(child_path); fixture.publish();
    check(!fixture.validate().valid, "nested output outside hash inventory rejected even if physical file remains");
}

void test_effect_shader_contract()
{
    candidate_fixture fixture;
    fixture.add_effect();
    const std::string source_path = "shaders/fluid.sdr";
    const std::string pixel_path = "shaders/ps/fluid.txt";
    const std::string descriptor = "version 5 \"Fluid\" { pixelshader Shaders\\ps\\fluid.txt { } { filter TEXF_LINEAR address TADDR_WRAP } }";
    const std::string program = "ps.1.4\ndef c6, 0, 0, 0, 1\ntexld r0, t0\nadd r2, c0, r0\nphase\ntexld r0, t0\ntexld r1, r2\nlrp r0, c1.r, r1, r0\n";
    const auto encode = [](const std::string& bytes)
    {
        std::string hex;
        for(unsigned char byte : bytes) { hex += "0123456789abcdef"[byte >> 4]; hex += "0123456789abcdef"[byte & 15]; }
        return hex;
    };
    auto publish_shader = [&](const std::string& pixel_bytes, bool source_bound)
    {
        fixture.manifest["sources"].erase(fixture.manifest["sources"].begin() + 1, fixture.manifest["sources"].end());
        fixture.sources["records"].erase(fixture.sources["records"].begin() + 1, fixture.sources["records"].end());
        json shader = {{"format", "EDS_GFX_SHADER"}, {"schemaVersion", 1}, {"implementation", "angelica-fluid-v1"},
            {"source", {{"sourcePathUtf8", source_path}, {"contentHash", hash_text(descriptor)}, {"bytesHex", encode(descriptor)}}},
            {"pixelShader", {{"sourcePathUtf8", pixel_path}, {"contentHash", hash_text(pixel_bytes)}, {"bytesHex", encode(pixel_bytes)}}}};
        for(const char* member : {"source", "pixelShader"})
        {
            if(!source_bound && std::string(member) == "pixelShader") continue;
            const auto& record = shader.at(member);
            const std::string source = record.at("sourcePathUtf8").get<std::string>();
            const std::string digest = record.at("contentHash").get<std::string>();
            fixture.manifest["sources"].push_back({{"path", source}, {"contentHash", digest}, {"required", true}});
            json metadata = fixture.sources["records"][0];
            metadata["sourcePath"] = source; metadata["sourceHash"] = digest;
            fixture.sources["records"].push_back(metadata);
        }
        const std::string bytes = shader.dump();
        const std::string path = "fx/shaders/" + hash_text(bytes) + ".eds.shader.json";
        fixture.add(path, "gfxShader", bytes);
        const std::string effect = "fx/000000.eds.effect.json";
        json effect_doc = json::parse(fixture.payloads.at(effect));
        effect_doc["dependencies"] = json::array({{{"kind", "shader"}, {"required", true}, {"elementIndex", 0},
            {"shaderRef", path}, {"implementation", "angelica-fluid-v1"}, {"sourcePathUtf8", source_path},
            {"sourceHash", hash_text(descriptor)}, {"converted", true}}});
        fixture.add(effect, "effect", effect_doc.dump());
        fixture.publish();
    };
    publish_shader(program, true);
    const auto accepted = fixture.validate();
    check(accepted.valid, "known Fluid shader closes over exact raw program and both source hashes: " + accepted.error);
    publish_shader("// comment\n" + program, true);
    check(fixture.validate().valid, "known Fluid shader tolerates source comments with new valid lineage");
    std::string changed = program;
    changed.replace(changed.find("add r2"), 3, "mul");
    publish_shader(changed, true);
    check(!fixture.validate().valid, "different PS opcode rejected even with internally consistent output/source hashes");
    changed = program;
    changed.replace(changed.find("texld"), 5, "tex ld");
    publish_shader(changed, true);
    check(!fixture.validate().valid, "shader token boundaries cannot be changed by splitting opcodes");
    publish_shader(program, false);
    check(!fixture.validate().valid, "required pixel shader must belong to map source inventory");
}

void test_external_candidate()
{
    const char* root = std::getenv("PW_MAP_TEST_ROOT");
    if(root == nullptr || root[0] == '\0')
        return;
    pw_map_manifest_request request;
    request.content_root = fs::path(root);
    const char* slug = std::getenv("PW_MAP_TEST_SLUG");
    if(slug != nullptr && slug[0] != '\0')
        request.slug = slug;
    else
    {
        const fs::path maps = request.content_root / "maps";
        if(fs::is_directory(maps))
        {
            for(const fs::directory_entry& entry : fs::directory_iterator(maps))
            {
                if(entry.is_directory() && fs::is_regular_file(entry.path() / "map.manifest.json"))
                {
                    check(request.slug.empty(), "external fixture identifies exactly one map (otherwise specify PW_MAP_TEST_SLUG)");
                    request.slug = entry.path().filename().string();
                }
            }
        }
    }
    const char* legacy = std::getenv("PW_MAP_TEST_LEGACY");
    request.allow_legacy = legacy != nullptr && std::string(legacy) == "1";
    request.require_full = !request.allow_legacy;
    const pw_map_manifest_result result = validate_pw_map_manifest(request);
    check(result.valid && (request.allow_legacy || !result.legacy), "external candidate " + request.slug + ": " + result.error);
    std::printf("PW external candidate: slug=%s valid=%d legacy=%d buildings=%zu water=%zu surfaces=%zu grass=%zu blades=%llu ecmodels=%zu effects=%zu outputs=%zu error=%s\n",
                request.slug.c_str(), result.valid ? 1 : 0, result.legacy ? 1 : 0, result.building_ids.size(), result.water_ids.size(),
                result.water_surface_ids.size(), result.grass_ids.size(), static_cast<unsigned long long>(result.grass_blade_count),
                result.ecmodel_ids.size(), result.effect_ids.size(), result.required_outputs.size(), result.error.c_str());
}

auto run_pw_map_manifest(rtti::context&) -> int
{
    g_checks = 0;
    g_failures = 0;
    test_source_and_output_contract();
    test_identity_and_reference_closure();
    test_grass_contract();
    test_effect_dependency_closure();
    test_effect_shader_contract();
    test_full_geometry_and_legacy_policy();
    test_external_candidate();
    std::printf("PW map manifest: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
} // namespace

REGISTER_TEST_SUITE("pw map manifest / identity / completeness", run_pw_map_manifest)
