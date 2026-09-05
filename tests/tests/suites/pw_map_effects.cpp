#include "../tests.h"
#include <engine/pw/pw_map_effects.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <cmath>
#include <stdexcept>

using namespace unravel;
namespace
{
using json = nlohmann::json;
int failures = 0;
int checks = 0;

void check(bool value, const char* text)
{
    ++checks;
    if(!value)
    {
        ++failures;
        std::printf("  FAIL: %s\n", text);
    }
}

auto record(const std::string& line) -> json
{
    const char* digits = "0123456789abcdef";
    std::string hex;
    for(const unsigned char byte : line)
    {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 15]);
    }
    return {{"line", line}, {"bytesHex", hex}};
}

auto make_document() -> json
{
    json records = json::array();
    for(const std::string& line : {
            "GFXELEMENTID: 123", "Name: Unnamed", "TexSpeed: 0.125", "TexSpeed: -0.25",
            "EmissionRate: 17.125", "AreaSize: 2, 4, 6", "AffectorCount: 1", "CtrlType: 105",
            "StartTime: 0.25", "EndTime: -1", "ColorDelta: 0, 0, 0, -1000",
            "StartTime: 350", "KEYPOINTCOUNT: 2", "InterpolateMode: 2", "TimeSpan: 1200",
            "Position: 1, 2, 3", "Color: -1", "Scale: 0.125", "Direction: 0, 0, 0, 2",
            "Rad_2D: 0.5", "CtrlMethodCount: 0", "InterpolateMode: 0", "TimeSpan: -1",
            "Position: 4, 5, 6", "Color: -2130706433", "Scale: 2.25", "Direction: 0, 0, 1, 0",
            "Rad_2D: -0.25", "CtrlMethodCount: 1", "CtrlType: 100", "StartTime: 0.5", "EndTime: 2.5",
            "Dir: 0, 1, 0", "Vel: 2.25", "Acc: -0.5"})
        records.push_back(record(line));
    json element = {{"typeId", 123}, {"name", "Unnamed"}, {"keyPointCount", 2},
        {"authored", {{"recordEncoding", "legacy-bytes-hex"}, {"typePayloadRecordOffset", 4},
            {"keyPointCountRecordOffset", 12}, {"records", records}}}};
    return {{"format", "EDS_EFFECT"}, {"schemaVersion", 1}, {"sourceId", "effect:00000001"},
        {"gfxFormat", "text"}, {"sourceReadable", true}, {"complete", true}, {"version", 103},
        {"defaultScale", 0.375}, {"defaultPlaySpeed", 0.75}, {"defaultAlpha", 0.625},
        {"declaredElementCount", 2}, {"elements", json::array({element, element})},
        {"authoredHeader", json::array({record("Version: 103"), record("GFXELEMENTCOUNT: 2")})},
        {"dependencies", json::array({{{"elementIndex", 0}, {"elementType", 123}, {"elementName", "Unnamed"},
            {"kind", "texture"}, {"sourcePath", "first.dds"}, {"textureRef", "textures/first.png"}, {"required", true}},
            {{"elementIndex", 1}, {"elementType", 123}, {"elementName", "Unnamed"}, {"kind", "texture"},
            {"sourcePath", "second.dds"}, {"textureRef", "textures/second.png"}, {"required", true}}})}};
}

void test_native_records()
{
    const auto parsed = parse_pw_effect_document(make_document());
    check(parsed.valid, parsed.error.c_str());
    if(!parsed.valid) return;
    check(parsed.elements.size() == 2 && parsed.elements[0].dependencies[0].output == "textures/first.png" &&
          parsed.elements[1].dependencies[0].output == "textures/second.png",
          "duplicate display names bind dependencies by native element ordinal");
    const auto& element = parsed.elements[0];
    check(element.start_ms == 350 && element.particle_affectors.size() == 1 &&
          element.particle_affectors[0].start_seconds == 0.25 && element.keypoints[0].duration_ms == 1200,
          "particle affector StartTime is distinct from keypoint-set StartTime");
    check(element.keypoints[1].duration_ms == UINT32_MAX && element.keypoints[0].color_argb == UINT32_MAX &&
          element.keypoints[0].direction[3] == 1.0f,
          "native signed ARGB and infinite duration retain bits; quaternion normalized");
    check(element.keypoints[1].controllers.size() == 1 && element.keypoints[1].controllers[0].type == 100 &&
          element.keypoints[1].controllers[0].start_seconds == 0.5 &&
          read_pw_effect_numbers(element.keypoints[1].controllers[0].fields, "Vel", 0, 1)[0] == 2.25,
          "authored controller timing and velocity are retained without arbitrary defaults");
    check(read_pw_effect_numbers(element.base_fields, "TexSpeed", 0, 1)[0] == 0.125 &&
          read_pw_effect_numbers(element.base_fields, "TexSpeed", 1, 1)[0] == -0.25 &&
          read_pw_effect_numbers(element.type_fields, "AreaSize", 0, 3)[2] == 6.0,
          "repeated fields and asymmetric geometry preserve source values");
}

void test_rejections()
{
    auto source = make_document();
    source["elements"][0].erase("authored");
    check(!parse_pw_effect_document(source).valid, "v5 metadata-only effect is not accepted as authored data");
    source = make_document();
    source["elements"][0]["authored"]["keyPointCountRecordOffset"] = 8;
    check(!parse_pw_effect_document(source).valid, "affector start cannot masquerade as timeline");
    source = make_document();
    source["elements"][0]["authored"]["records"][18] = record("Direction: 0, 0, 0, 0");
    check(!parse_pw_effect_document(source).valid, "zero native quaternion is rejected");
    source = make_document();
    source["dependencies"][0]["textureRef"] = "../outside.png";
    check(!parse_pw_effect_document(source).valid, "dependency cannot escape the candidate root");
    source = make_document();
    source["dependencies"][0]["elementIndex"] = 2;
    check(!parse_pw_effect_document(source).valid, "dependency cannot bind an absent element");
    source = make_document();
    source["elements"][0]["authored"]["records"][29] = record("CtrlType: 999");
    check(!parse_pw_effect_document(source).valid, "unknown controller is an explicit error");
    bool rejected = false;
    try { read_pw_effect_numbers({{"Speed", "2 garbage"}}, "Speed", 0, 1); }
    catch(const std::exception&) { rejected = true; }
    check(rejected, "malformed authored numbers never receive guessed defaults");
}

void test_source_instances()
{
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("unravel-pw-effects-" + std::to_string(timestamp));
    fs::create_directories(root / "fx");
    const auto write = [&](const std::string& file, const std::string& id)
    {
        auto document = make_document();
        document["sourceId"] = id;
        std::ofstream output(root / file);
        output << document.dump();
    };
    write("fx/first.json", "effect:00000001");
    write("fx/second.json", "effect:00000002");
    const auto node = [](const std::string& id, const std::string& file, float x) -> json
    {
        return {{"type", 14}, {"sourceId", id}, {"externalPath", "scene/shared.gfx"},
            {"pos", {x, 2.0f, 3.0f}}, {"dir", {0.0f, 0.0f, 1.0f}}, {"up", {0.0f, 1.0f, 0.0f}},
            {"effectParameters", {{"present", true}, {"scale", 0.32012345f}, {"playSpeed", 0.725125f},
                {"alpha", 0.8125125f}, {"validTime", 2}}},
            {"openFormat", {{"effectRef", {{"effect", file}}}}}};
    };
    const auto first = node("effect:00000001", "fx/first.json", 1.0f);
    json scene = {{"nodes", json::array({first, first, node("effect:00000002", "fx/second.json", 9.0f)})}};
    const auto prepared = prepare_pw_map_effects(root, scene);
    check(prepared.valid && prepared.raw_references == 3 && prepared.instances.size() == 2 &&
          prepared.duplicate_references == 1,
          "exact source duplicates deduplicate; placements sharing an asset stay distinct");
    if(prepared.valid)
        check(prepared.instances[0].scale == 0.32012345f && prepared.instances[1].position[0] == 9.0f,
              "effect placement values retain native float precision");
    scene["nodes"][1]["pos"][0] = 6.0f;
    check(!prepare_pw_map_effects(root, scene).valid, "same source identity with contradictory transform fails");
    fs::remove_all(root);
}

void test_prepared_native_noise_and_curve()
{
    const auto document_for = [](const std::vector<std::string>& controller) -> json
    {
        auto document = make_document();
        json records = json::array();
        for(const std::string& line : {"GFXELEMENTID: 101", "Name: Curve", "Width: 1", "Height: 1",
                "StartTime: 0", "KEYPOINTCOUNT: 1", "InterpolateMode: 0", "TimeSpan: 2000",
                "Position: 0, 0, 0", "Color: -1", "Scale: 1", "Direction: 0, 0, 0, 1",
                "Rad_2D: 0", "CtrlMethodCount: 1"}) records.push_back(record(line));
        for(const auto& line : controller) records.push_back(record(line));
        document["elements"] = json::array({{{"typeId", 101}, {"name", "Curve"}, {"keyPointCount", 1},
            {"authored", {{"recordEncoding", "legacy-bytes-hex"}, {"typePayloadRecordOffset", 2},
                {"keyPointCountRecordOffset", 5}, {"records", records}}}}});
        document["declaredElementCount"] = 1;
        document["dependencies"] = json::array();
        return document;
    };
    const auto curve_source = document_for({"CtrlType: 110", "StartTime: 0", "EndTime: -1", "CalcDir: 1",
        "Count: 3", "Pos: 0, 0, 0", "Pos: 0, 6, 0", "Pos: 0, 12, 0"});
    const auto curve = parse_pw_effect_document(curve_source);
    check(curve.valid, curve.error.c_str());
    if(curve.valid)
    {
        const auto& samples = curve.elements[0].keypoints[0].controllers[0].curve_samples;
        check(samples.size() == 13 && samples.front()[1] == 0 && samples.back()[1] == 12,
              "native GenPath retains endpoints and six subdivisions per authored segment");
        if(samples.size() == 13)
            check(std::abs(samples[6][1] - 6) < 0.00001f && std::abs(samples[6][3] - 0.5f) < 0.00001f,
                  "native curve parameter uses normalized arc length at the middle control point");
    }
    const auto noise_source = document_for({"CtrlType: 109", "StartTime: 0", "EndTime: -1", "BufLen: 8",
        "Amplitude: 2", "WaveLen: 8", "Persistence: 0.5", "OctaveNum: 3"});
    const auto noise = parse_pw_effect_document(noise_source);
    check(noise.valid, noise.error.c_str());
    if(noise.valid)
    {
        const auto& prepared = noise.elements[0].keypoints[0].controllers[0];
        float sum = 0;
        for(const auto& octave : prepared.noise_octaves) sum += octave[2];
        check(prepared.noise_values.size() == 8 && prepared.noise_octaves.size() == 3 &&
              prepared.noise_octaves[0][1] == 8 && prepared.noise_octaves[2][1] == 2 && std::abs(sum - 2) < 0.00001f,
              "native noise halves wavelength and normalizes total amplitude");
        const auto again = parse_pw_effect_document(noise_source);
        check(again.valid && again.elements[0].keypoints[0].controllers[0].noise_values == prepared.noise_values,
              "noise realization is stable across reloads without per-frame random regeneration");
    }
    auto invalid = noise_source;
    invalid["elements"][0]["authored"]["records"][19] = record("WaveLen: 0");
    check(!parse_pw_effect_document(invalid).valid, "zero native wavelength fails before rendering");
}

void test_native_login_shapes()
{
    const auto ring = make_pw_effect_ring(2, 10, 0, 0, true);
    check(ring.size() == 74 && ring[0] == std::array<float, 3>{2, -5, 0} &&
          ring[1] == std::array<float, 3>{2, 5, 0} && ring[72] == ring[0] && ring[73] == ring[1],
          "Login ring uses native default 36 sectors, centered height, and exact seam closure");
    const auto tapered = make_pw_effect_ring(2, 10, 1, 4, false);
    check(tapered[0] == std::array<float, 3>{4, 0, 0} && tapered[1] == std::array<float, 3>{0, 10, 0} &&
          std::abs(tapered[2][2] - 4) < 0.00001f,
          "native ring pitch clamps radii and traverses positive X toward positive Z");
    const auto volume = sample_pw_effect_box({150, 50, 150}, false, {0, 0.25f, 0.75f, 0.5f});
    check(volume == std::array<float, 3>{-37.5f, 12.5f, 0}, "box volume retains asymmetric authored half extents");
    const auto surface_z = sample_pw_effect_box({150, 50, 150}, true, {0.1f, 0.25f, 0.75f, 0.5f});
    const auto surface_y = sample_pw_effect_box({150, 50, 150}, true, {0.5f, 0.25f, 0.75f, 0.5f});
    check(surface_z == std::array<float, 3>{-37.5f, 12.5f, 75} &&
          surface_y == std::array<float, 3>{-37.5f, 25, 0},
          "Login ship emitters follow native surface face thresholds, not an ellipsoid distribution");
    bool rejected = false;
    try { make_pw_effect_ring(2, 10, 0, 2, true); }
    catch(const std::exception&) { rejected = true; }
    check(rejected, "invalid ring sector count fails on CPU before resource staging");
}

void test_native_login_runtime_contract()
{
    pw_effect_element ring;
    ring.type = 140;
    ring.base_fields = {{"Name", "Ring"}, {"SrcBlend", "5"}, {"DestBlend", "2"}};
    ring.type_fields = {{"Radius", "2"}, {"Height", "10"}, {"Pitch", "0"}, {"Sects", "0"},
        {"NoRadScale", "0"}, {"NoHeiScale", "0"}, {"OrgAtCenter", "1"}};
    pw_effect_keypoint point;
    point.duration_ms = UINT32_MAX;
    point.direction = {0, 0, 0, 1};
    point.scale = 1;
    point.color_argb = UINT32_MAX;
    ring.keypoints.push_back(point);
    pw_effect_element trail;
    trail.type = 110;
    trail.base_fields = {{"Name", "Trail"}, {"SrcBlend", "5"}, {"DestBlend", "2"}};
    trail.type_fields = {{"OrgPos1", "0.1, 0, 0"}, {"OrgPos2", "-0.1, 0, 0"}, {"SegLife", "1200"},
        {"EnableMat", "0"}, {"EnableOrgPos1", "0"}, {"EnableOrgPos2", "0"}, {"Bind", "0"}, {"Spline", "0"}};
    trail.keypoints.push_back(point);
    pw_effect_element box;
    box.type = 121;
    box.base_fields = {{"Name", "Box"}, {"SrcBlend", "5"}, {"DestBlend", "2"}, {"DummyEle", "Child"}};
    box.type_fields = {{"ParticleWidth", "1"}, {"ParticleHeight", "1"}, {"EmissionRate", "0.3"},
        {"3DParticle", "0"}, {"Facing", "0"}, {"TTL", "20"}, {"Quota", "-1"}, {"Angle", "0"},
        {"Speed", "0"}, {"Acc", "0"}, {"AccDir", "0, 0, 0"}, {"IsBind", "1"}, {"IsSurface", "1"},
        {"ScaleMin", "1"}, {"ScaleMax", "2.5"}, {"ColorMin", "-1"}, {"ColorMax", "-1"}, {"AreaSize", "150, 50, 150"}};
    box.keypoints.push_back(point);
    pw_effect_element child;
    child.type = 200;
    child.base_fields = {{"Name", "Child"}, {"SrcBlend", "5"}, {"DestBlend", "2"}, {"IsDummy", "1"}};
    child.keypoints.push_back(point);
    pw_effect_dependency dependency;
    dependency.kind = "gfx";
    dependency.nested = std::make_shared<pw_effect_document>();
    dependency.nested->valid = true;
    dependency.nested->elements = {ring, trail};
    child.dependencies.push_back(dependency);
    pw_effect_preparation prepared;
    prepared.valid = true;
    prepared.instances.emplace_back();
    prepared.instances[0].document.elements = {box, child};
    pw_map_effects_runtime runtime;
    runtime.begin(prepared, "app:/data/login", 1);
    check(runtime.error().empty(), "native Login box/dummy/nested ring/trail compile without GPU or service access");
    dependency.nested->elements[1].type_fields.back().value = "1";
    prepared.instances[0].document.elements[1].dependencies[0] = dependency;
    pw_map_effects_runtime unsupported;
    unsupported.begin(std::move(prepared), "app:/data/login", 2);
    check(!unsupported.error().empty(), "unimplemented spline trail mode fails explicitly instead of drawing a line fallback");
}

void test_external_native_candidate()
{
    const char* configured_root = std::getenv("PW_MAP_TEST_ROOT");
    if(configured_root == nullptr || configured_root[0] == '\0')
    {
        std::printf("  SKIP: real native effect candidate (PW_MAP_TEST_ROOT is unset)\n");
        return;
    }
    try
    {
        const fs::path root(configured_root);
        const char* configured_slug = std::getenv("PW_MAP_TEST_SLUG");
        std::string slug = configured_slug ? configured_slug : "";
        if(slug.empty())
        {
            for(const auto& entry : fs::directory_iterator(root / "maps"))
            {
                if(!entry.is_directory() || !fs::is_regular_file(entry.path() / "scene.eds.json")) continue;
                if(!slug.empty()) throw std::runtime_error("multiple maps: specify PW_MAP_TEST_SLUG");
                slug = entry.path().filename().string();
            }
        }
        if(slug.empty()) throw std::runtime_error("native candidate map slug is absent");
        std::ifstream scene_input(root / "maps" / slug / "scene.eds.json", std::ios::binary);
        if(!scene_input) throw std::runtime_error("native candidate scene JSON is unreadable");
        const auto scene = json::parse(scene_input);
        auto prepared = prepare_pw_map_effects(root, scene);
        check(prepared.valid, ("real native candidate prepare " + slug + ": " + prepared.error).c_str());
        if(!prepared.valid) return;
        std::vector<std::string> ids;
        size_t root_elements = 0;
        for(const auto& instance : prepared.instances)
        {
            ids.push_back(instance.source_id);
            root_elements += instance.document.elements.size();
        }
        std::sort(ids.begin(), ids.end());
        check(std::adjacent_find(ids.begin(), ids.end()) == ids.end(), "real candidate preparation has unique source IDs");
        std::ifstream manifest_input(root / "maps" / slug / "map.manifest.json", std::ios::binary);
        if(manifest_input)
        {
            const auto manifest = json::parse(manifest_input);
            if(manifest.contains("effects") && manifest.at("effects").contains("identity"))
            {
                auto expected = manifest.at("effects").at("identity").at("uniqueIds").get<std::vector<std::string>>();
                std::sort(expected.begin(), expected.end());
                check(ids == expected, "real native prepared IDs exactly match the manifest effect inventory");
            }
        }
        const size_t raw = prepared.raw_references, duplicates = prepared.duplicate_references;
        pw_map_effects_runtime runtime;
        runtime.begin(std::move(prepared), "app:/data/" + slug, 1);
        check(runtime.error().empty(), ("real native candidate programs " + slug + ": " + runtime.error()).c_str());
        std::printf("PW native effect candidate: slug=%s raw=%zu unique=%zu duplicates=%zu rootElements=%zu programsValid=%d error=%s\n",
                    slug.c_str(), raw, ids.size(), duplicates, root_elements, runtime.error().empty() ? 1 : 0, runtime.error().c_str());
    }
    catch(const std::exception& error)
    {
        check(false, (std::string("real native candidate exception: ") + error.what()).c_str());
    }
}
} // namespace

auto run_pw_map_effects_suite(rtti::context&) -> int
{
    failures = 0;
    checks = 0;
    test_native_records();
    test_rejections();
    test_source_instances();
    test_prepared_native_noise_and_curve();
    test_native_login_shapes();
    test_native_login_runtime_contract();
    test_external_native_candidate();
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures;
}
REGISTER_TEST_SUITE("PW map effects authored records", run_pw_map_effects_suite)
