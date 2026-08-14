#include "asset_compiler.h"
#include "asset_dependencies.h"
#include "asset_writer.h"
#include "asset_manifest.h"
#include "bimg/bimg.h"
#include "importers/mesh_importer.h"

#include <bx/error.h>
#include <bx/process.h>
#include <bx/string.h>

#include <graphics/shader.h>
#include <graphics/texture.h>
#include <graphics/utils/bgfx_utils.h>
#include <logging/logging.h>
#include <uuid/uuid.h>

#include <serialization/associative_archive.h>
#include <serialization/binary_archive.h>

#include <engine/assets/impl/asset_extensions.h>
#include <engine/engine.h>
#include <engine/settings/settings.h>
#include <engine/meta/animation/animation.hpp>
#include <engine/meta/assets/asset_database.hpp>
#include <engine/meta/assets/asset_importer_meta.hpp>
#include <engine/meta/audio/audio_clip.hpp>
#include <engine/meta/ecs/entity.hpp>
#include <engine/meta/physics/physics_material.hpp>
#include <engine/meta/ui/ui_tree.hpp>
#include <engine/meta/ui/style_sheet.hpp>
#include <engine/meta/rendering/font.hpp>
#include <engine/meta/rendering/material.hpp>
#include <engine/meta/rendering/mesh.hpp>

#include <engine/meta/scripting/script.hpp>

#include <engine/scripting/ecs/systems/script_system.h>
#include <engine/profiler/profiler.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <dotnetpp/dotnetpp.h>
#include <iterator>
#include <optional>
#include <regex>
#include <sstream>
#include <hpp/string_view.hpp>
#include <subprocess/subprocess.hpp>
#include <string_utils/utils.h>

#include <core/base/platform/config.hpp>

namespace unravel::asset_compiler
{

namespace
{

auto resolve_path(const std::string& key) -> fs::path
{
    return fs::absolute(fs::resolve_protocol(key));
}

auto resolve_input_file(const fs::path& key) -> fs::path
{
    fs::path absolute_path = fs::convert_to_protocol(key);
    absolute_path = fs::resolve_protocol(fs::replace(absolute_path, ex::get_meta_directory(), ex::get_data_directory()));
    if(absolute_path.extension() == ".meta")
    {
        absolute_path.replace_extension();
    }
    return absolute_path;
}

auto escape_str(const std::string& str) -> std::string
{
    return "\"" + str + "\"";
}

auto run_process(const std::string& process,
                 const std::vector<std::string>& args_array,
                 bool check_retcode,
                 std::string& err) -> bool
{
    auto result = subprocess::call(process, args_array);
    err = result.out_output;

    if(!result.err_output.empty())
    {
        if(!err.empty())
        {
            err += "\n";
        }

        err += result.err_output;
    }

    if(err.find("error") != std::string::npos)
    {
        return false;
    }

    return result.retcode == 0;
}

struct input_texture_info
{
    gfx::texture_format format{gfx::texture_format::RGBA8};
    uint32_t width{};
    uint32_t height{};
    bool fits_max_size{true};
};

/// Lat-long HDRIs are authored at 2:1. Allow a small tolerance for odd sizes.
constexpr float k_equirect_aspect_ratio = 2.0f;
constexpr float k_equirect_aspect_tolerance = 0.05f;

auto texture_size_to_pixel_limit(texture_importer_meta::texture_size size) -> uint32_t
{
    switch(size)
    {
        case texture_importer_meta::texture_size::size_32:
            return 32;
        case texture_importer_meta::texture_size::size_64:
            return 64;
        case texture_importer_meta::texture_size::size_128:
            return 128;
        case texture_importer_meta::texture_size::size_256:
            return 256;
        case texture_importer_meta::texture_size::size_512:
            return 512;
        case texture_importer_meta::texture_size::size_1024:
            return 1024;
        case texture_importer_meta::texture_size::size_2048:
            return 2048;
        case texture_importer_meta::texture_size::size_4096:
            return 4096;
        case texture_importer_meta::texture_size::size_8192:
            return 8192;
        case texture_importer_meta::texture_size::size_16384:
            return 16384;
        case texture_importer_meta::texture_size::project_default:
        default:
            return 0;
    }
}

auto append_texture_max_size_args(std::vector<std::string>& args, texture_importer_meta::texture_size max_size) -> void
{
    const uint32_t limit = texture_size_to_pixel_limit(max_size);
    if(limit > 0)
    {
        args.emplace_back("--max");
        args.emplace_back(std::to_string(limit));
    }
}

auto fill_input_texture_info_from_container(const bimg::ImageContainer& info, input_texture_info& out) -> void
{
    out.format = static_cast<gfx::texture_format>(info.m_format);
    out.width = info.m_width;
    out.height = info.m_height;
}

auto get_input_texture_info(const fs::path& input_path, texture_importer_meta::texture_size max_size) -> input_texture_info
{
    input_texture_info result{};
    const bx::FilePath file_path(input_path.string().c_str());

    bimg::ImageContainer header{};
    if(imageParseInfo(file_path, header))
    {
        fill_input_texture_info_from_container(header, result);
    }
    else
    {
        bimg::ImageContainer* image = imageLoad(file_path, bgfx::TextureFormat::Count);
        if(image == nullptr)
        {
            return result;
        }

        fill_input_texture_info_from_container(*image, result);
        bimg::imageFree(image);
    }

    const uint32_t limit = texture_size_to_pixel_limit(max_size);
    if(limit > 0)
    {
        const uint32_t largest_dimension = std::max(result.width, result.height);
        result.fits_max_size = largest_dimension <= limit;
    }

    return result;
}

auto is_approximately_equirect_aspect(uint32_t width, uint32_t height) -> bool
{
    if(width == 0 || height == 0)
    {
        return false;
    }
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    return std::fabs(aspect - k_equirect_aspect_ratio) <= k_equirect_aspect_tolerance;
}

/**
 * @brief Builds default texture importer settings for a newly discovered source.
 *
 * Radiance HDR panoramas almost always feed sky/IBL cubemaps (Unreal-like), so
 * `.hdr` defaults to equirect. EXR is also used for regular HDR maps, so only
 * promote when the image is a ~2:1 lat-long panorama.
 */
auto create_default_texture_importer_meta(const fs::path& input_path) -> std::shared_ptr<texture_importer_meta>
{
    auto importer = std::make_shared<texture_importer_meta>();
    const auto extension = string_utils::to_lower(input_path.extension().string());
    if(extension == ".hdr")
    {
        importer->type = texture_importer_meta::texture_type::equirect;
        return importer;
    }
    if(extension == ".exr")
    {
        const auto info = get_input_texture_info(input_path, texture_importer_meta::texture_size::project_default);
        if(is_approximately_equirect_aspect(info.width, info.height))
        {
            importer->type = texture_importer_meta::texture_type::equirect;
        }
    }
    return importer;
}

// auto run_process(const std::string& process, const std::vector<std::string>& args_array, bool check_retcode, std::string& err) -> bool
// {
//     auto now = std::chrono::high_resolution_clock::now();

//     std::string args;
//     size_t i = 0;
//     for(const auto& arg : args_array)
//     {
//         if(arg.front() == '-')
//         {
//             args += arg;
//         }
//         else
//         {
//             args += escape_str(arg);
//         }

//         if(i++ != args_array.size() - 1)
//             args += " ";
//     }

//     bx::Error error;
//     bx::ProcessReader process_reader;

// #if UNRAVEL_PLATFORM_WINDOWS
//     process_reader.open((process + " " + args).c_str(), "", &error);
// #else
//     process_reader.open(process.c_str(), args.c_str(), &error);
// #endif

//     bool ok = true;
//     if(!error.isOk())
//     {
//         err = std::string(error.getMessage().getCPtr());
//         ok = false;
//     }
//     else
//     {
//         std::array<char, 2048 * 32> buffer;
//         buffer.fill(0);
//         int32_t sz = process_reader.read(buffer.data(), static_cast<std::int32_t>(buffer.size()), &error);

//         process_reader.close();
//         int32_t result = process_reader.getExitCode();

        
//         if(0 != result)
//         {
//             err = std::string(error.getMessage().getCPtr());
//             if(sz > 0)
//             {
//                 err += " " + std::string(buffer.data());
//             }
//             ok = false;
//         }

//     }

    
//     auto end = std::chrono::high_resolution_clock::now();
//     auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - now);
//     APPLOG_TRACE("Process {} took {} ", process, duration);
//     return ok;
// }

bool copy_compiled_file(const fs::path& from, const fs::path& to)
{
    fs::error_code err;
    asset_writer::atomic_copy_file(from, to, err);

    if(err)
    {
        APPLOG_ERROR("Failed compilation of {0} -> {1} with error: {2}", from.string(), to.filename().string(), err.message());
    }

    return !err;
}

auto is_passthrough_texture_container(const fs::path& input_path) -> bool
{
    const auto extension = input_path.extension().generic_string();
    return extension == ".dds" || extension == ".ktx";
}

auto read_u32_le(const std::vector<uint8_t>& data, size_t offset) -> uint32_t
{
    if(offset + 4 > data.size())
    {
        return 0;
    }

    return uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8) | (uint32_t(data[offset + 2]) << 16) |
           (uint32_t(data[offset + 3]) << 24);
}

auto read_u64_le(const std::vector<uint8_t>& data, size_t offset) -> uint64_t
{
    if(offset + 8 > data.size())
    {
        return 0;
    }

    uint64_t value = 0;
    for(size_t i = 0; i < 8; ++i)
    {
        value |= uint64_t(data[offset + i]) << (i * 8);
    }
    return value;
}

void write_u32_le(std::ostream& output, uint32_t value)
{
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xffu),
        static_cast<char>((value >> 8) & 0xffu),
        static_cast<char>((value >> 16) & 0xffu),
        static_cast<char>((value >> 24) & 0xffu),
    };
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

struct ktx2_dds_format
{
    uint32_t dxgi_format = 0;
    uint32_t unit_bytes = 0;
    bool block_compressed = true;
};

auto get_ktx2_bc_dds_format(uint32_t vk_format) -> std::optional<ktx2_dds_format>
{
    // Vulkan KTX2 format ids mapped to DDS DX10 ids accepted by bimg.
    switch(vk_format)
    {
        case 37: return ktx2_dds_format{28, 4, false}; // VK_FORMAT_R8G8B8A8_UNORM
        case 43: return ktx2_dds_format{29, 4, false}; // VK_FORMAT_R8G8B8A8_SRGB
        case 44: return ktx2_dds_format{87, 4, false}; // VK_FORMAT_B8G8R8A8_UNORM
        case 50: return ktx2_dds_format{91, 4, false}; // VK_FORMAT_B8G8R8A8_SRGB
        case 131: return ktx2_dds_format{71, 8, true};  // VK_FORMAT_BC1_RGB_UNORM_BLOCK
        case 132: return ktx2_dds_format{72, 8, true};  // VK_FORMAT_BC1_RGB_SRGB_BLOCK
        case 133: return ktx2_dds_format{71, 8, true};  // VK_FORMAT_BC1_RGBA_UNORM_BLOCK
        case 134: return ktx2_dds_format{72, 8, true};  // VK_FORMAT_BC1_RGBA_SRGB_BLOCK
        case 135: return ktx2_dds_format{74, 16, true}; // VK_FORMAT_BC2_UNORM_BLOCK
        case 136: return ktx2_dds_format{75, 16, true}; // VK_FORMAT_BC2_SRGB_BLOCK
        case 137: return ktx2_dds_format{77, 16, true}; // VK_FORMAT_BC3_UNORM_BLOCK
        case 138: return ktx2_dds_format{78, 16, true}; // VK_FORMAT_BC3_SRGB_BLOCK
        case 139: return ktx2_dds_format{80, 8, true};  // VK_FORMAT_BC4_UNORM_BLOCK
        case 141: return ktx2_dds_format{83, 16, true}; // VK_FORMAT_BC5_UNORM_BLOCK
        case 143: return ktx2_dds_format{95, 16, true}; // VK_FORMAT_BC6H_UFLOAT_BLOCK
        case 144: return ktx2_dds_format{96, 16, true}; // VK_FORMAT_BC6H_SFLOAT_BLOCK
        case 145: return ktx2_dds_format{98, 16, true}; // VK_FORMAT_BC7_UNORM_BLOCK
        case 146: return ktx2_dds_format{99, 16, true}; // VK_FORMAT_BC7_SRGB_BLOCK
        default: return std::nullopt;
    }
}

auto get_ktx2_mip_size(uint32_t width, uint32_t height, const ktx2_dds_format& format) -> uint64_t
{
    if(format.block_compressed)
    {
        const uint64_t blocks_w = std::max<uint32_t>(1, (width + 3) / 4);
        const uint64_t blocks_h = std::max<uint32_t>(1, (height + 3) / 4);
        return blocks_w * blocks_h * format.unit_bytes;
    }

    return uint64_t(width) * uint64_t(height) * uint64_t(format.unit_bytes);
}

auto wrap_ktx2_bc_as_dds(const fs::path& input_path, const fs::path& output_path) -> bool
{
    if(input_path.extension().generic_string() != ".ktx2")
    {
        return false;
    }

    std::ifstream input(input_path, std::ios::binary);
    if(!input)
    {
        return false;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    static constexpr std::array<uint8_t, 12> ktx2_identifier{
        0xab, 0x4b, 0x54, 0x58, 0x20, 0x32, 0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a};
    if(data.size() < 80 || !std::equal(ktx2_identifier.begin(), ktx2_identifier.end(), data.begin()))
    {
        return false;
    }

    const uint32_t vk_format = read_u32_le(data, 12);
    const uint32_t width = read_u32_le(data, 20);
    const uint32_t height = read_u32_le(data, 24);
    const uint32_t depth = read_u32_le(data, 28);
    const uint32_t layer_count = read_u32_le(data, 32);
    const uint32_t face_count = read_u32_le(data, 36);
    const uint32_t level_count = read_u32_le(data, 40);
    const uint32_t supercompression_scheme = read_u32_le(data, 44);
    const auto format = get_ktx2_bc_dds_format(vk_format);

    if(!format || width == 0 || height == 0 || depth != 0 || layer_count > 1 || face_count != 1 ||
       level_count == 0 || supercompression_scheme != 0 || data.size() < 80ull + uint64_t(level_count) * 24ull)
    {
        return false;
    }

    struct level_entry
    {
        uint64_t offset = 0;
        uint64_t length = 0;
    };

    std::vector<level_entry> levels;
    levels.reserve(level_count);
    for(uint32_t i = 0; i < level_count; ++i)
    {
        const size_t table_offset = 80ull + uint64_t(i) * 24ull;
        const uint64_t offset = read_u64_le(data, table_offset);
        const uint64_t length = read_u64_le(data, table_offset + 8);
        const uint32_t mip_width = std::max<uint32_t>(1, width >> i);
        const uint32_t mip_height = std::max<uint32_t>(1, height >> i);
        const uint64_t expected_length = get_ktx2_mip_size(mip_width, mip_height, *format);

        if(length < expected_length || offset > data.size() || length > data.size() - offset)
        {
            return false;
        }
        levels.push_back({offset, expected_length});
    }

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if(!output)
    {
        return false;
    }

    constexpr uint32_t dds_magic = 0x20534444;              // "DDS "
    constexpr uint32_t dds_header_size = 124;
    constexpr uint32_t ddsd_caps = 0x00000001;
    constexpr uint32_t ddsd_height = 0x00000002;
    constexpr uint32_t ddsd_width = 0x00000004;
    constexpr uint32_t ddsd_pixelformat = 0x00001000;
    constexpr uint32_t ddsd_mipmapcount = 0x00020000;
    constexpr uint32_t ddsd_pitch = 0x00000008;
    constexpr uint32_t ddsd_linearsize = 0x00080000;
    constexpr uint32_t ddpf_fourcc = 0x00000004;
    constexpr uint32_t dds_dx10 = 0x30315844;               // "DX10"
    constexpr uint32_t ddscaps_complex = 0x00000008;
    constexpr uint32_t ddscaps_texture = 0x00001000;
    constexpr uint32_t ddscaps_mipmap = 0x00400000;
    constexpr uint32_t dds_dimension_texture2d = 3;
    constexpr uint32_t array_size = 1;

    write_u32_le(output, dds_magic);
    write_u32_le(output, dds_header_size);
    write_u32_le(output,
                 ddsd_caps | ddsd_height | ddsd_width | ddsd_pixelformat |
                     (format->block_compressed ? ddsd_linearsize : ddsd_pitch) |
                      (level_count > 1 ? ddsd_mipmapcount : 0u));
    write_u32_le(output, height);
    write_u32_le(output, width);
    write_u32_le(output,
                 format->block_compressed ? static_cast<uint32_t>(levels.front().length)
                                          : width * format->unit_bytes);
    write_u32_le(output, 1); // depth
    write_u32_le(output, level_count);
    for(uint32_t i = 0; i < 11; ++i)
    {
        write_u32_le(output, 0);
    }

    write_u32_le(output, 32); // DDS_PIXELFORMAT size
    write_u32_le(output, ddpf_fourcc);
    write_u32_le(output, dds_dx10);
    write_u32_le(output, 0); // RGBBitCount
    write_u32_le(output, 0); // RBitMask
    write_u32_le(output, 0); // GBitMask
    write_u32_le(output, 0); // BBitMask
    write_u32_le(output, 0); // ABitMask

    write_u32_le(output, ddscaps_texture | (level_count > 1 ? (ddscaps_complex | ddscaps_mipmap) : 0u));
    write_u32_le(output, 0); // caps2
    write_u32_le(output, 0); // caps3
    write_u32_le(output, 0); // caps4
    write_u32_le(output, 0); // reserved2

    write_u32_le(output, format->dxgi_format);
    write_u32_le(output, dds_dimension_texture2d);
    write_u32_le(output, 0); // miscFlag
    write_u32_le(output, array_size);
    write_u32_le(output, 0); // miscFlags2

    for(const auto& level : levels)
    {
        output.write(reinterpret_cast<const char*>(data.data() + level.offset), static_cast<std::streamsize>(level.length));
    }

    if(!output)
    {
        fs::error_code err;
        fs::remove(output_path, err);
        return false;
    }

    APPLOG_INFO("Wrapped KTX2 texture as DDS: {0} -> {1}",
                input_path.filename().string(),
                output_path.filename().string());
    return true;
}

auto select_compressed_format(gfx::texture_format input_format,
                              const fs::path& extension,
                              texture_importer_meta::compression_quality quality) -> gfx::texture_format
{
    if(quality == texture_importer_meta::compression_quality::none)
    {
        return input_format;
    }

    if(input_format == gfx::texture_format::BC1)
    {
        return gfx::texture_format::BC3;
    }

    if(gfx::is_compressed_format(input_format))
    {
        return input_format;
    }

    auto info = gfx::get_format_info(input_format);

    if(extension == ".hdr" || extension == ".exr")
    {
        info.is_hdr = true;
    }

    // 1) HDR? Use BC6H for color data, ignoring alpha (HDR with alpha is non-trivial).
    if(info.is_hdr)
    {
        // BC6H: color (RGB) 16F
        // No standard BC format for HDR alpha in the block-compression range.
        return gfx::texture_format::BC6H;
    }

    // 2) Single channel => BC4
    //    e.g., for grayscale height map or single-channel mask
    if(info.num_channels == 1)
    {
        return gfx::texture_format::BC4;
    }

    // 3) Two channel => BC5
    //    e.g., typical for 2D vector data, normal map XY
    if(info.num_channels == 2)
    {
        return gfx::texture_format::BC5;
    }

    // 4) If we reach here, we have 3 or 4 channels in LDR.

    // 4a) No alpha needed => choose BC1 or BC7, etc.
    if(!info.has_alpha_channel)
    {
        switch(quality)
        {
            case texture_importer_meta::compression_quality::low_quality:
                // BC1 is cheap and has no alpha
                return gfx::texture_format::BC1;
            case texture_importer_meta::compression_quality::normal_quality:
                // BC1 is standard for color w/out alpha
                return gfx::texture_format::BC1;
            case texture_importer_meta::compression_quality::high_quality:
                // BC7 is higher quality for color, also supports alpha but not needed here.
                // It is also really slow for encoding so don't use it for now.
                return gfx::texture_format::BC1;
            default:
                break;
        }
        // fallback
        return gfx::texture_format::BC1;
    }
    else
    {
        // 4b) We do have alpha => choose BC2, BC3, or BC7.
        // BC2 (DXT3) is old and rarely used except for sharp alpha transitions.
        // BC3 (DXT5) is the typical solution for alpha textures if BC7 is not an option.
        // BC7 is better (but bigger decode cost).
        switch(quality)
        {
            case texture_importer_meta::compression_quality::low_quality:
                return gfx::texture_format::BC3;
            case texture_importer_meta::compression_quality::normal_quality:
                return gfx::texture_format::BC3; // DXT5
            case texture_importer_meta::compression_quality::high_quality:
                //  BC7 is best BC for RGBA
                // It is also really slow for encoding so don't use it for now.
                return gfx::texture_format::BC3;
            default:
                break;
        }
        // fallback
        return gfx::texture_format::BC3;
    }

    return input_format;
}

auto bake_normal_map_input_if_needed(const fs::path& input_path,
                                     const texture_importer_meta& importer,
                                     fs::path& temp_baked_path) -> fs::path
{
    temp_baked_path.clear();

    if(!importer.invert_normal_y)
    {
        return input_path;
    }

    bimg::ImageContainer* image = imageLoad(bx::FilePath(input_path.string().c_str()));
    if(nullptr == image)
    {
        APPLOG_ERROR("Failed to load texture for normal Y bake: {0}", input_path.string());
        return input_path;
    }

    if(!imageFlipTangentSpaceNormalY(image))
    {
        bimg::imageFree(image);
        APPLOG_ERROR("Failed to flip normal map Y for: {0}", input_path.string());
        return input_path;
    }

    if(!imagePrepareNormalMapBakePng(image))
    {
        bimg::imageFree(image);
        APPLOG_ERROR("Failed to prepare baked normal map PNG for: {0}", input_path.string());
        return input_path;
    }

    fs::error_code err;
    // Always feed texturec a lossless RGBA8 PNG (avoids broken BC re-encode and zero-alpha PNG previews).
    temp_baked_path = fs::temp_directory_path(err) / (input_path.stem().string() + "_normal_y.png");
    if(err || temp_baked_path.empty())
    {
        bimg::imageFree(image);
        APPLOG_ERROR("Failed to resolve temp path for normal Y bake: {0}", input_path.string());
        return input_path;
    }

    if(!imageSave(temp_baked_path.string().c_str(), image))
    {
        bimg::imageFree(image);
        fs::remove(temp_baked_path, err);
        temp_baked_path.clear();
        APPLOG_ERROR("Failed to write baked normal map: {0}", input_path.string());
        return input_path;
    }

    bimg::imageFree(image);
    APPLOG_INFO("Baked invert normal Y for {0} -> {1}", input_path.filename().string(), temp_baked_path.filename().string());
    return temp_baked_path;
}

auto compile_texture_to_file(const fs::path& input_path, 
                            const fs::path& output_path,
                            const texture_importer_meta& importer,
                            const std::string& protocol) -> bool
{
    fs::path temp_baked_path;
    const fs::path compile_input = bake_normal_map_input_if_needed(input_path, importer, temp_baked_path);
    const bool using_temp_input = !temp_baked_path.empty();

    std::string str_input = compile_input.string();
    std::string str_output = output_path.string();
    
    bool try_compress = protocol == "app";

    if(!using_temp_input && is_passthrough_texture_container(compile_input))
    {
        return copy_compiled_file(compile_input, output_path);
    }

    const bool is_ktx2_input = compile_input.extension().generic_string() == ".ktx2";
    if(is_ktx2_input && wrap_ktx2_bc_as_dds(compile_input, output_path))
    {
        if(using_temp_input)
        {
            fs::error_code remove_err;
            fs::remove(temp_baked_path, remove_err);
        }

        return true;
    }
    
    auto quality = importer.quality;
    if(quality.compression == texture_importer_meta::compression_quality::project_default)
    {
        auto& ctx = engine::context();
        if(ctx.has<settings>())
        {
            auto& ss = ctx.get<settings>();
            quality.compression = ss.assets.texture.default_compression;
        }
    }

    if(quality.max_size == texture_importer_meta::texture_size::project_default)
    {
        auto& ctx = engine::context();
        if(ctx.has<settings>())
        {
            auto& ss = ctx.get<settings>();
            quality.max_size = ss.assets.texture.default_max_size;
        }
    }

    // If still default, set to normal quality
    if(quality.compression == texture_importer_meta::compression_quality::project_default)
    {
        quality.compression = texture_importer_meta::compression_quality::normal_quality;
    }

    // If still default, set to 2048
    if(quality.max_size == texture_importer_meta::texture_size::project_default)
    {
        quality.max_size = texture_importer_meta::texture_size::size_2048;
    }

    const auto input_info = get_input_texture_info(compile_input, quality.max_size);
    auto format = select_compressed_format(input_info.format, compile_input.extension(), quality.compression);

    const bool needs_format_conversion = input_info.format != format;
    const bool needs_downscale = !input_info.fits_max_size;
    // Equirect must always run through texturec; a raw copy cannot produce a cubemap.
    const bool needs_equirect_projection = importer.type == texture_importer_meta::texture_type::equirect;

    if(needs_format_conversion || needs_downscale || is_ktx2_input || needs_equirect_projection)
    {
        if(needs_downscale && !needs_format_conversion)
        {
            APPLOG_INFO("Downscaling {0} ({1}x{2}) to fit max size {3}",
                        compile_input.filename().string(),
                        input_info.width,
                        input_info.height,
                        texture_size_to_pixel_limit(quality.max_size));
        }

        std::vector<std::string> args_array = {
            "-f",
            str_input,
            "-o",
            str_output,
            "--as",
            "dds",
        };
        
        if(try_compress)
        {
            args_array.emplace_back("-t");
            args_array.emplace_back(gfx::to_string(format));

            if(format == gfx::texture_format::BC7 || format == gfx::texture_format::BC6H)
            {
                APPLOG_INFO("Compressing to {0}. May take a while.", gfx::to_string(format));

                args_array.emplace_back("-q");
                args_array.emplace_back("fastest");
            }
            else if(needs_format_conversion
                    && quality.compression == texture_importer_meta::compression_quality::high_quality)
            {
                args_array.emplace_back("-q");
                args_array.emplace_back("highest");
            }
        }

        if(importer.generate_mipmaps)
        {
            args_array.emplace_back("-m");
        }

        append_texture_max_size_args(args_array, quality.max_size);

        switch(importer.type)
        {
            case texture_importer_meta::texture_type::equirect:
            {
                args_array.emplace_back("--equirect");
                break;
            }

            case texture_importer_meta::texture_type::normal_map:
            {
                args_array.emplace_back("--normalmap");
                break;
            }

            default:
                break;
        }

        std::string error;
        
        // Create an empty file at the output location so the process can write to it
        {
            std::ofstream output_file(str_output);
            (void)output_file;
        }
        
        auto texturec = fs::resolve_protocol("binary:/texturec");
        
        // Run the texture compiler directly to the temporary output location
        bool compiled = run_process(texturec.string(), args_array, false, error);
        if(!compiled)
        {
            fs::remove(str_output);
            if(is_ktx2_input && wrap_ktx2_bc_as_dds(compile_input, output_path))
            {
                return true;
            }

            APPLOG_ERROR("Failed compilation of {0} with error: {1}", str_input, error);
            return false;
        }
    }
    else
    {
        if(!copy_compiled_file(compile_input, output_path))
        {
            return false;
        }
    }

    if(using_temp_input)
    {
        fs::error_code remove_err;
        fs::remove(temp_baked_path, remove_err);
    }

    
    return true;
}

auto compile_shader_to_file(const fs::path& input_path, 
                           const fs::path& output_path,
                           gfx::renderer_type renderer) -> bool
{

    std::string str_input = input_path.string();
    std::string str_output = output_path.string();
    
    std::string file = input_path.stem().string();
    fs::path dir = input_path.parent_path();

    fs::path include = fs::resolve_protocol("engine:/data/shaders");
    std::string str_include = include.string();
    
    fs::path varying = dir / (file + ".io");

    fs::error_code err;
    if(!fs::exists(varying, err))
    {
        varying = dir / "varying.def.io";
    }
    if(!fs::exists(varying, err))
    {
        varying = dir / "varying.def.sc";
    }
    
    std::string str_varying = varying.string();
    
    std::string str_platform;
    std::string str_profile;
    std::string str_type;

    bool optimize = true;
// #if UNRAVEL_DEBUG
//     optimize = false;
// #endif

    std::string str_opt = optimize ? "3" : "0";

    bool vs = hpp::string_view(file).starts_with("vs_");
    bool fs = hpp::string_view(file).starts_with("fs_");
    bool cs = hpp::string_view(file).starts_with("cs_");

    // Vertex/fragment shaders that reference compute-style read/write buffers
    // (BUFFER_RO / BUFFER_RW / BUFFER_WO) need SSBO support, which requires a
    // higher GLSL profile. Detect that up-front by scanning the shader source
    // so the correct OpenGL profile is selected below.
    bool needs_compute_buffers = false;
    if(vs || fs)
    {
        std::ifstream shader_file(str_input);
        if(shader_file.is_open())
        {
            std::stringstream buffer;
            buffer << shader_file.rdbuf();
            const std::string source = buffer.str();
            needs_compute_buffers = source.find("BUFFER_RO(") != std::string::npos
                                 || source.find("BUFFER_RW(") != std::string::npos
                                 || source.find("BUFFER_WO(") != std::string::npos;
        }
    }

    if(renderer == gfx::renderer_type::Vulkan)
    {
        str_platform = "windows";
        str_profile = "spirv";
    }

    if(renderer == gfx::renderer_type::Direct3D11 || renderer == gfx::renderer_type::Direct3D12)
    {
        str_platform = "windows";

        if(vs || fs)
        {
            str_profile = "s_5_0";
            if(renderer == gfx::renderer_type::Direct3D12)
            {
                str_profile = "s_6_0";
            }
        }
        else if(cs)
        {
            str_profile = "s_5_0";
            if(renderer == gfx::renderer_type::Direct3D12)
            {
                str_profile = "s_6_0";
            }
            str_opt = optimize ? "1" : "0";
        }
    }
    else if(renderer == gfx::renderer_type::OpenGLES)
    {
        str_platform = "android";
        str_profile = "100_es";
    }
    else if(renderer == gfx::renderer_type::OpenGL)
    {
        str_platform = "linux";

        if(vs || fs)
        {
            // GLSL 4.30 is needed to expose SSBOs (compute-style buffers) in
            // vertex/fragment stages. Otherwise stick with the more portable 1.40.
            str_profile = needs_compute_buffers ? "430" : "140";
        }
        else if(cs)
        {
            str_profile = "430";
        }
    }
    else if(renderer == gfx::renderer_type::Metal)
    {
        str_platform = "osx";
        str_profile = "metal";
    }

    if(vs)
        str_type = "vertex";
    else if(fs)
        str_type = "fragment";
    else if(cs)
        str_type = "compute";
    else
        str_type = "unknown";

    std::vector<std::string> args_array = {
        "-f",
        str_input,
        "-o",
        str_output,
        "-i",
        str_include,
        "--varyingdef",
        str_varying,
        "--type",
        str_type,
        "--define",
        "BGFX_CONFIG_MAX_BONES=" + std::to_string(gfx::get_max_blend_transforms())
        //        "--Werror"
    };

    if(!str_platform.empty())
    {
        args_array.emplace_back("--platform");
        args_array.emplace_back(str_platform);
    }

    if(!str_profile.empty())
    {
        args_array.emplace_back("-p");
        args_array.emplace_back(str_profile);
    }

    if(!str_opt.empty())
    {
        args_array.emplace_back("-O");
        args_array.emplace_back(str_opt);
    }

    std::string error;

    // Create an empty file at the output location
    {
        std::ofstream output_file(str_output);
        (void)output_file;
    }

    auto shaderc = fs::resolve_protocol("binary:/shaderc");

    if(!run_process(shaderc.string(), args_array, true, error))
    {
        APPLOG_ERROR("Failed compilation of {0} -> {1} with error: {2}", str_input, output_path.filename().string(), error);
        fs::remove(str_output);
        return false;
    }
    return true;
}

template<typename T>
auto write_manifest_file(const fs::path& input_path, const fs::path& output_path) -> bool
{
    APP_SCOPE_PERF("Write Manifest File");
    std::vector<fs::path> deps;
    resolve_dependencies<T>(input_path, deps);

    asset_manifest manifest(input_path);
    manifest.compute_source_fingerprint(input_path, deps);
    auto manifest_path = get_manifest_path(output_path);

    bool ok = false;

    int attempts = 0;
    while(!ok && attempts < 3)
    {
        fs::error_code err;
        bool success = true;
        asset_writer::atomic_write_file(manifest_path, [&](const fs::path& temp_manifest_path) 
        {
            success = save_manifest(temp_manifest_path, manifest);
        }, err);
        ok = !err && success;
        attempts++;
    }

    return ok;
}

auto write_minified_file(const fs::path& input_path, const fs::path& output_path) -> bool
{
    
#if SER20_ASSOCIATIVE_ARCHIVE == SER20_ASSOCIATIVE_ARCHIVE_SIMDJSON
    
    std::string str_input = input_path.string();
    simdjson::dom::parser parser;
    auto doc = parser.load(str_input);
    if(doc.error())
    {
        APPLOG_ERROR("Failed to parse {0}: {1}", input_path.string(), simdjson::error_message(doc.error()));
        return false;
    }

    auto minified = simdjson::minify(doc);


    fs::error_code err;
    bool success = true;
    asset_writer::atomic_write_file(output_path, [&](const fs::path& temp_manifest_path) 
    {
        std::ofstream file(temp_manifest_path);
        if(file.is_open())
        {
            file << minified;
            file.close();
        }
    }, err);
    return !err;
#else

    copy_compiled_file(input_path, output_path);

    return true;
#endif

}

} // namespace

template<>
auto compile<gfx::shader>(asset_manager& am, const fs::path& key, const fs::path& output, uint32_t flags) -> bool
{
    auto absolute_path = resolve_input_file(key);
    std::string str_input = absolute_path.string();
    
    auto extension = output.extension();
    auto renderer = gfx::get_renderer_based_on_filename_extension(extension.string());
    
    fs::error_code err;
    // Use atomic_write_file to handle the temporary file creation and atomic rename
    asset_writer::atomic_write_file(output, [&](const fs::path& temp_output) -> void
    {
        compile_shader_to_file(
            absolute_path,
            temp_output,
            renderer
        );
    }, err);
    
    if(err)
    {
        APPLOG_ERROR("Failed compilation of {0} -> {1} with error: {2}", 
            str_input, output.filename().string(), err.message());
        return false;
    }

    if(!write_manifest_file<gfx::shader>(absolute_path, output))
    {
        APPLOG_ERROR("Failed to write manifest for compiled shader: {0}", output.string());
        return false;
    }
    
    return true;
}

template<>
auto read_importer<gfx::texture>(asset_manager& am, const fs::path& key) -> std::shared_ptr<asset_importer_meta>
{
    auto absolute = fs::resolve_protocol(key).string();
    asset_meta meta;
    if(load_from_file(absolute, meta))
    {
        if(!meta.importer)
        {
            const fs::path input_path = resolve_input_file(key);
            meta.importer = create_default_texture_importer_meta(input_path);

            meta.uid = am.add_asset_info_for_path(input_path, meta, true);

            fs::error_code err;
            asset_writer::atomic_write_file(absolute, [&](const fs::path& temp) -> void
            {
                save_to_file(temp.string(), meta);
            }, err);

            return nullptr;
        }
    }

    return meta.importer;
}

template<>
auto compile<gfx::texture>(asset_manager& am, const fs::path& key, const fs::path& output, uint32_t flags) -> bool
{
    APP_SCOPE_PERF("Compile Texture");
    auto base_importer = read_importer<gfx::texture>(am, key);

    if(!base_importer)
    {
        return true;
    }
    auto importer = std::static_pointer_cast<texture_importer_meta>(base_importer);

    auto protocol = fs::extract_protocol(fs::convert_to_protocol(key)).generic_string();
    auto absolute_path = resolve_input_file(key);
    std::string str_input = absolute_path.string();

    fs::error_code err;
    bool compiled = false;
    
    asset_writer::atomic_write_file(output, [&](const fs::path& temp_output) -> void
    {
        compiled = compile_texture_to_file(
            absolute_path, 
            temp_output, 
            *importer, 
            protocol
        );

        if(!compiled)
        {
            fs::error_code remove_err;
            fs::remove(temp_output, remove_err);
        }
    }, err);
    
    if(err || !compiled)
    {
        APPLOG_ERROR("Failed compilation of {0} -> {1} with error: {2}", 
            str_input, output.filename().string(), err ? err.message() : "texture compiler returned false");
        return false;
    }

    if(!write_manifest_file<gfx::texture>(absolute_path, output))
    {
        APPLOG_ERROR("Failed to write manifest for compiled texture: {0}", output.string());
        return false;
    }
    
    return true;
}

template<>
auto compile<material>(asset_manager& am, const fs::path& key, const fs::path& output, uint32_t flags) -> bool
{
    APP_SCOPE_PERF("Compile Material");
    auto absolute_path = resolve_input_file(key);

    std::string str_input = absolute_path.string();

    fs::error_code err;

    std::shared_ptr<material> material;
    {
        load_from_file(str_input, material);

        asset_writer::atomic_write_file(output, [&](const fs::path& temp) -> void
        {
            save_to_file_bin(temp.string(), material);
        }, err);
    }

    if(err)
    {
        APPLOG_ERROR("Failed compilation of {0} -> {1} with error: {2}", 
            str_input, output.filename().string(), err.message());
        return false;
    }

    if(!write_manifest_file<unravel::material>(absolute_path, output))
    {
        APPLOG_ERROR("Failed to write manifest for compiled material: {0}", output.string());
        return false;
    }

    return true;
}

template<>
auto read_importer<mesh>(asset_manager& am, const fs::path& key) -> std::shared_ptr<asset_importer_meta>
{
    APP_SCOPE_PERF("Read Mesh Importer");
    auto absolute = fs::resolve_protocol(key).string();
    asset_meta meta;
    if(load_from_file(absolute, meta))
    {
        if(!meta.importer)
        {
            meta.importer = std::make_shared<mesh_importer_meta>();

            meta.uid = am.add_asset_info_for_path(resolve_input_file(key), meta, true);

            fs::error_code err;
            asset_writer::atomic_write_file(absolute, [&](const fs::path& temp) -> void
            {
                save_to_file(temp.string(), meta);
            }, err);

            return nullptr;
        }
    }

    return meta.importer;
}

template<>
auto compile<mesh>(asset_manager& am, const fs::path& key, const fs::path& output, uint32_t flags) -> bool
{
    APP_SCOPE_PERF("Compile Mesh");
    // Try to import first.
    auto base_importer = read_importer<mesh>(am, key);

    if(!base_importer)
    {
        return true;
    }

    auto importer = std::static_pointer_cast<mesh_importer_meta>(base_importer);

    auto absolute_path = resolve_input_file(key);

    std::string str_input = absolute_path.string();

    fs::error_code err;

    fs::path file = absolute_path.stem();
    fs::path dir = absolute_path.parent_path();

    mesh::load_data data;
    std::vector<animation_clip> animations;
    std::vector<importer::imported_material> materials;
    std::vector<importer::imported_texture> textures;
    // load_mesh_data_from_file waits for multi-file companions (.gltf/.bin, .obj/.mtl)
    // before Assimp runs, so incomplete copies cannot produce empty mesh buffers.
    if(!importer::load_mesh_data_from_file(am, absolute_path, *importer, data, animations, materials, textures))
    {
        APPLOG_ERROR("Failed compilation of {0}", str_input);
        return false;
    }
    // Never write a manifest / "successful" compile for an empty mesh. That happens when a
    // multi-file source (.gltf + .bin) was observed before companions finished copying.
    if(data.vertex_data.empty())
    {
        APPLOG_ERROR("Failed compilation of {0}: imported mesh has no vertex data "
                     "(source companions may still be incomplete)",
                     str_input);
        return false;
    }
    // IMPORTANT:
    // For skinned meshes, the skin binding step can duplicate vertices and rewrite triangle indices
    // to ensure a consistent bone palette per submesh. LODs must be generated AFTER this rewrite,
    // otherwise the stored LOD index buffers will reference the wrong vertices at runtime.
    if(data.skin_data.has_bones())
    {
        APP_SCOPE_PERF("Apply Skin to Load Data");
        if(!mesh::apply_skin_to_load_data(data))
        {
            APPLOG_ERROR("Failed to apply skinning data before generating LODs for {0}", str_input);
            return false;
        }
    }
    // Generate LODs offline during compilation (no GPU buffers created)
    if(importer->model.generate_lods)
    {
        // Use custom LOD configs if provided, otherwise use defaults
        auto lod_configs = mesh::generate_default_lod_configs(data, importer->model.lod_target_error);
        if(!lod_configs.empty())
        {
            APP_SCOPE_PERF("Generate LODs for Load Data");
            mesh::generate_lods_for_load_data(data, lod_configs);
        }
    }
    // Save materials and register their UIDs before writing the mesh binary
    data.default_material_uids.reserve(materials.size());
    APPLOG_INFO("Adding default material UIDs for {0}", str_input);
    for(const auto& material : materials)
    {
        fs::path mat_output;
        if(material.name.empty())
        {
            mat_output = (dir / file).string() + ".mat";
        }
        else
        {
            mat_output = dir / (material.name + ".mat");
        }
        auto uid = am.add_asset_for_path(mat_output, false);
        data.default_material_uids.push_back(uid);
        asset_writer::atomic_write_file(mat_output, [&](const fs::path& temp) -> void
        {
            save_to_file(temp.string(), material.mat);
        }, err);
    }
    asset_writer::atomic_write_file(output, [&](const fs::path& temp) -> void
    {
        save_to_file_bin(temp.string(), data);
    }, err);

    {
        APP_SCOPE_PERF("Write Animations");
        for(const auto& animation : animations)
        {
           fs::path anim_output;
           if(animation.name.empty())
           {
               anim_output = (dir / file).string() + ".anim";
           }
           else
           {
               anim_output = dir / (animation.name + ".anim");
           }

           asset_writer::atomic_write_file(anim_output, [&](const fs::path& temp) -> void
           {
                save_to_file(temp.string(), animation);
           }, err);
        }
    }

    if(err)
    {
        APPLOG_ERROR("Failed compilation of {0} -> {1} with error: {2}", 
            str_input, output.filename().string(), err.message());
        return false;
    }

    if(!write_manifest_file<mesh>(absolute_path, output))
    {
        APPLOG_ERROR("Failed to write manifest for compiled mesh: {0}", output.string());
        return false;
    }

    return true;
}

template<>
auto read_importer<animation_clip>(asset_manager& am, const fs::path& key) -> std::shared_ptr<asset_importer_meta>
{
    APP_SCOPE_PERF("Read Animation Clip Importer");
    auto absolute = fs::resolve_protocol(key).string();
    asset_meta meta;
    if(load_from_file(absolute, meta))
    {
        if(!meta.importer)
        {
            meta.importer = std::make_shared<animation_importer_meta>();

            meta.uid = am.add_asset_info_for_path(resolve_input_file(key), meta, true);

            fs::error_code err;
            asset_writer::atomic_write_file(absolute,   [&](const fs::path& temp) -> void
            {
                save_to_file(temp.string(), meta);
            }, err);

            return nullptr;
        }
    }

    return meta.importer;
}

template<>
auto compile<animation_clip>(asset_manager& am, const fs::path& key, const fs::path& output, uint32_t flags) -> bool
{
    APP_SCOPE_PERF("Compile Animation Clip");
    // Try to import first.
    auto base_importer = read_importer<animation_clip>(am, key);

    if(!base_importer)
    {
        return true;
    }

    auto importer = std::static_pointer_cast<animation_importer_meta>(base_importer);

    auto absolute_path = resolve_input_file(key);

    std::string str_input = absolute_path.string();

    fs::error_code err;

    animation_clip anim;
    {
        load_from_file(str_input, anim);

        anim.root_motion.keep_position_y = importer->root_motion.keep_position_y;
        anim.root_motion.keep_position_xz = importer->root_motion.keep_position_xz;
        anim.root_motion.keep_rotation = importer->root_motion.keep_rotation;
        anim.root_motion.keep_in_place = importer->root_motion.keep_in_place;

        fs::error_code err;
        asset_writer::atomic_write_file(output, [&](const fs::path& temp) -> void
        {
            save_to_file_bin(temp.string(), anim);
        }, err);
    }

    if(err)
    {
        APPLOG_ERROR("Failed compilation of {0} -> {1} with error: {2}", 
            str_input, output.filename().string(), err.message());
        return false;
    }

    if(!write_manifest_file<animation_clip>(absolute_path, output))
    {
        APPLOG_ERROR("Failed to write manifest for compiled animation: {0}", output.string());
        return false;
    }

    return true;
}

template<>
auto compile<font>(asset_manager& am, const fs::path& key, const fs::path& output, uint32_t flags) -> bool
{
    APP_SCOPE_PERF("Compile Font");
    auto absolute_path = resolve_input_file(key);

    copy_compiled_file(absolute_path, output);

    if(!write_manifest_file<font>(absolute_path, output))
    {
        APPLOG_ERROR("Failed to write manifest for compiled font: {0}", output.string());
        return false;
    }

    return true;
}

template<>
auto compile<prefab>(asset_manager& am, const fs::path& key, const fs::path& output, uint32_t flags) -> bool
{
    APP_SCOPE_PERF("Compile Prefab");
    auto absolute_path = resolve_input_file(key);

    if(!write_minified_file(absolute_path, output))
    {
        APPLOG_ERROR("Failed to write minified file for compiled prefab: {0}", output.string());
        return false;
    }

    if(!write_manifest_file<prefab>(absolute_path, output))
    {
        APPLOG_ERROR("Failed to write manifest for compiled prefab: {0}", output.string());
        return false;
    }

    return true;
}

template<>
auto compile<scene_prefab>(asset_manager& am, const fs::path& key, const fs::path& output, uint32_t flags) -> bool
{
    APP_SCOPE_PERF("Compile Scene Prefab");
    auto absolute_path = resolve_input_file(key);
   
    if(!write_minified_file(absolute_path, output))
    {
        APPLOG_ERROR("Failed to write minified file for compiled scene: {0}", output.string());
        return false;
    }

    if(!write_manifest_file<scene_prefab>(absolute_path, output))
    {
        APPLOG_ERROR("Failed to write manifest for compiled scene_prefab: {0}", output.string());
        return false;
    }

    return true;
}

template<>
auto compile<physics_material>(asset_manager& am, const fs::path& key, const fs::path& output, uint32_t flags) -> bool
{
    APP_SCOPE_PERF("Compile Physics Material");
    auto absolute_path = resolve_input_file(key);

    std::string str_input = absolute_path.string();

    fs::error_code err;

    auto material = std::make_shared<physics_material>();
    {
        load_from_file(str_input, material);

        asset_writer::atomic_write_file(output, [&](const fs::path& temp) -> void
        {
            save_to_file_bin(temp.string(), material);
        }, err);
    }

    if(err)
    {
        APPLOG_ERROR("Failed compilation of {0} -> {1} with error: {2}", 
            str_input, output.filename().string(), err.message());
        return false;
    }

    if(!write_manifest_file<physics_material>(absolute_path, output))
    {
        APPLOG_ERROR("Failed to write manifest for compiled physics_material: {0}", output.string());
        return false;
    }

    return true;
}

template<>
auto compile<ui_tree>(asset_manager& am, const fs::path& key, const fs::path& output, uint32_t flags) -> bool
{
    APP_SCOPE_PERF("Compile UI Tree");
    auto absolute_path = resolve_input_file(key);
    std::string str_input = absolute_path.string();
    fs::error_code err;

    auto tree = std::make_shared<ui_tree>();
    {
        // For ui_tree, we can load the HTML/RML content directly from file
        std::ifstream file(absolute_path);
        if (file.is_open())
        {
            std::stringstream buffer;
            buffer << file.rdbuf();
            tree->content = buffer.str();
            file.close();
        }

        asset_writer::atomic_write_file(output, [&](const fs::path& temp) -> void
        {
            save_to_file(temp.string(), tree);
        }, err);
    }

    if(err)
    {
        APPLOG_ERROR("Failed compilation of {0} -> {1} with error: {2}", 
            str_input, output.filename().string(), err.message());
        return false;
    }

    if(!write_manifest_file<ui_tree>(absolute_path, output))
    {
        APPLOG_ERROR("Failed to write manifest for compiled ui_tree: {0}", output.string());
        return false;
    }

    return true;
}

template<>
auto compile<style_sheet>(asset_manager& am, const fs::path& key, const fs::path& output, uint32_t flags) -> bool
{
    APP_SCOPE_PERF("Compile Style Sheet");
    auto absolute_path = resolve_input_file(key);
    std::string str_input = absolute_path.string();
    fs::error_code err;

    auto sheet = std::make_shared<style_sheet>();
    {
        // For style_sheet, we can load the CSS/RCSS content directly from file
        std::ifstream file(absolute_path);
        if (file.is_open())
        {
            std::stringstream buffer;
            buffer << file.rdbuf();
            sheet->content = buffer.str();
            file.close();
        }

        asset_writer::atomic_write_file(output, [&](const fs::path& temp) -> void
        {
            save_to_file(temp.string(), sheet);
        }, err);
    }

    if(err)
    {
        APPLOG_ERROR("Failed compilation of {0} -> {1} with error: {2}", 
            str_input, output.filename().string(), err.message());
        return false;
    }

    if(!write_manifest_file<style_sheet>(absolute_path, output))
    {
        APPLOG_ERROR("Failed to write manifest for compiled style_sheet: {0}", output.string());
        return false;
    }

    return true;
}

template<>
auto read_importer<audio_clip>(asset_manager& am, const fs::path& key) -> std::shared_ptr<asset_importer_meta>
{
    APP_SCOPE_PERF("Read Audio Clip Importer");
    auto absolute = fs::resolve_protocol(key).string();
    asset_meta meta;
    if(load_from_file(absolute, meta))
    {
        if(!meta.importer)
        {
            meta.importer = std::make_shared<audio_importer_meta>();

            meta.uid = am.add_asset_info_for_path(resolve_input_file(key), meta, true);

            fs::error_code err;
            asset_writer::atomic_write_file(absolute, [&](const fs::path& temp) -> void
            {
                save_to_file(temp.string(), meta);
            }, err);

            return nullptr;
        }
    }

    return meta.importer;
}

template<>
auto compile<audio_clip>(asset_manager& am, const fs::path& key, const fs::path& output, uint32_t flags) -> bool
{
    APP_SCOPE_PERF("Compile Audio Clip");
    // Try to import first.
    auto base_importer = read_importer<audio_clip>(am, key);

    if(!base_importer)
    {
        return true;
    }

    auto importer = std::static_pointer_cast<audio_importer_meta>(base_importer);

    auto absolute_path = resolve_input_file(key);

    std::string str_input = absolute_path.string();

    fs::error_code err;

    audio::sound_data clip;
    {
        std::string error;
        if(!load_from_file(str_input, clip, error))
        {
            APPLOG_ERROR("Failed compilation of {0} with error: {1}", str_input, error);
            return false;
        }

        if(importer->force_to_mono)
        {
            clip.convert_to_mono();
        }

        asset_writer::atomic_write_file(output, [&](const fs::path& temp) -> void
        {
            save_to_file_bin(temp.string(), clip);
        }, err);
    }

    if(err)
    {
        APPLOG_ERROR("Failed compilation of {0} -> {1} with error: {2}", 
            str_input, output.filename().string(), err.message());
        return false;
    }

    if(!write_manifest_file<audio_clip>(absolute_path, output))
    {
        APPLOG_ERROR("Failed to write manifest for compiled audio_clip: {0}", output.string());
        return false;
    }

    return true;
}
// Struct to hold the parsed error details
struct script_compilation_entry
{
    std::string file{}; // Path to the file
    int line{};         // Line number of the error
    std::string msg{};  // Full error line
};

/**
 * Parse csc/dotnet diagnostics line-by-line.
 * Avoid greedy .* over the full compiler log: that pattern triggers
 * std::regex_error(error_complexity) on large outputs (MSVC STL).
 * Expected line form: path(line,col): error|warning CS....: message
 */
auto parse_compilation_entries(const std::string& log, hpp::string_view severity)
    -> std::vector<script_compilation_entry>
{
    // Path cannot contain '(' or newlines; keeps matching linear.
    const std::string pattern =
        std::string(R"(^([^\n(]+)\((\d+),\d+\):\s*)") + std::string(severity) + R"(\b.*)";
    const std::regex entry_regex(pattern, std::regex::ECMAScript | std::regex::optimize);

    std::vector<script_compilation_entry> entries;
    std::istringstream stream(log);
    std::string line;
    while(std::getline(stream, line))
    {
        if(!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        std::smatch match;
        if(!std::regex_match(line, match, entry_regex) || match.size() < 3)
        {
            continue;
        }
        script_compilation_entry entry;
        entry.file = match[1].str();
        entry.line = std::stoi(match[2].str());
        entry.msg = match[0].str();
        entries.emplace_back(std::move(entry));
    }
    return entries;
}

auto parse_compilation_errors(const std::string& log) -> std::vector<script_compilation_entry>
{
    return parse_compilation_entries(log, "error");
}

auto parse_compilation_warnings(const std::string& log) -> std::vector<script_compilation_entry>
{
    return parse_compilation_entries(log, "warning");
}

template<>
auto compile<script_library>(asset_manager& am, const fs::path& key, const fs::path& output, uint32_t flags) -> bool
{
    bool result = true;
    fs::error_code err;
    fs::path temp = fs::temp_directory_path(err);

    dotnet::compiler_params params;

    auto protocol = fs::extract_protocol(fs::convert_to_protocol(key)).generic_string();

    if(protocol != "engine")
    {
        auto lib_compiled_key = fs::resolve_protocol(script_system::get_lib_compiled_key("engine"));

        params.references.emplace_back(lib_compiled_key.filename().string());

        params.references_locations.emplace_back(lib_compiled_key.parent_path().string());
    }

    auto assets = am.get_assets<script>(protocol);
    for(const auto& asset : assets)
    {
        if(asset)
        {
            params.files.emplace_back(fs::resolve_protocol(asset.id()).string());
        }
    }

    temp /= script_system::get_lib_name(protocol);

    auto temp_xml = temp;
    temp_xml.replace_extension(".xml");
    auto output_xml = output;
    output_xml.replace_extension(".xml");

    auto temp_mdb = temp;
    temp_mdb.concat(".mdb");
    auto output_mdb = output;
    output_mdb.concat(".mdb");

    std::string str_output = temp.string();

    params.output_name = str_output;
    params.output_doc_name = temp_xml.string();
    // Project/app scripts: keep /doc for IntelliSense, skip doc/comment noise and
    // CS0649 for fields assigned from the inspector/native rather than C#.
    const bool is_app_scripts = (protocol != "engine");
    params.suppress_doc_warnings = is_app_scripts;
    params.suppress_unassigned_field_warnings = is_app_scripts;
    if(params.files.empty())
    {
        fs::remove(output, err);
        fs::remove(output_mdb, err);

        if(protocol == "engine")
        {
            APPLOG_ERROR("No scripts to compile for engine");
            return false;
        }

        return result;
    }

    params.debug = flags & script_library::compilation_flags::debug;

    std::string error;
    // auto cmd = dotnet::create_compile_command_detailed(params);
    auto cmd = dotnet::create_compile_command_detailed_rsp(params, temp.string() + ".rsp");

    // APPLOG_TRACE("Script Compile : \n {0} {1}", cmd.cmd, cmd.args);

    fs::remove(temp, err);
    fs::remove(temp_mdb, err);
    fs::remove(temp_xml, err);

    if(!run_process(cmd.cmd, cmd.args, true, error))
    {
        auto parsed_errors = parse_compilation_errors(error);

        if(!parsed_errors.empty())
        {
            for(const auto& error : parsed_errors)
            {
                APPLOG_ERROR_LOC(error.file.c_str(), error.line, "", error.msg);
            }
        }
        else
        {
            APPLOG_ERROR("Failed compilation of {0} with error: {1}", output.string(), error);
        }
        result = false;
    }
    else
    {
        if(!params.debug)
        {
            fs::remove(output_mdb, err);
        }

        fs::create_directories(output.parent_path(), err);

        if(protocol != "engine")
        {
            auto parsed_warnings = parse_compilation_warnings(error);

            for(const auto& warning : parsed_warnings)
            {
                APPLOG_WARNING_LOC(warning.file.c_str(), warning.line, "", warning.msg);
            }
        }

        // dotnet::compile_cmd aot_cmd;
        // aot_cmd.cmd = "mono";
        // aot_cmd.args.emplace_back("--aot=full");
        // aot_cmd.args.emplace_back(temp.string());
        // error = {};
        // bool ok = run_process(aot_cmd.cmd, aot_cmd.args, true, error);

        //APPLOG_INFO("Successful compilation of {0}", fs::replace(output, "temp-", "").string());

        // Part of script compilation: rewrite mono-style [InternalCall]
        // externs with real bodies (coreclr backend; no-op on mono).
        if(!dotnet::weave_assembly(str_output))
        {
            APPLOG_ERROR("Failed internal call weaving of {0}", output.string());
            return false;
        }

        script_system::copy_compiled_lib(temp, output);
    }

    return result;
}

template<>
auto compile<script>(asset_manager& am, const fs::path& key, const fs::path& output, uint32_t flags) -> bool
{
    APP_SCOPE_PERF("Compile Script");
    auto absolute_path = resolve_input_file(key);

    fs::error_code er;
    asset_writer::atomic_copy_file(absolute_path, output, er);

    if(er)
    {
        APPLOG_ERROR("Failed compilation of {0} -> {1} with error: {2}", 
            absolute_path.string(), output.filename().string(), er.message());
        return false;
    }

    if(!write_manifest_file<script>(absolute_path, output))
    {
        APPLOG_ERROR("Failed to write manifest for compiled script: {0}", output.string());
        return false;
    }


    return true;
}

} // namespace unravel::asset_compiler
