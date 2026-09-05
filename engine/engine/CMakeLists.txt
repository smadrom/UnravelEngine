file(GLOB_RECURSE libsrc *.h *.cpp *.hpp *.c *.cc)

set(target_name engine)

add_library(${target_name} ${libsrc} ${shader_files})

# PW_MAP v2 output hashes use BLAKE3. Keep the vendored C implementation
# portable and independent of the engine's C++ precompiled header.
set_source_files_properties(
    pw/detail/blake3.c pw/detail/blake3_dispatch.c pw/detail/blake3_portable.c
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON
    COMPILE_DEFINITIONS "BLAKE3_NO_SSE2;BLAKE3_NO_SSE41;BLAKE3_NO_AVX2;BLAKE3_NO_AVX512;BLAKE3_USE_NEON=0"
)

include(GenerateExportHeader)
generate_export_header(${target_name})

target_include_directories(${target_name}
    PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_include_directories(${target_name}
    PUBLIC
        ${CMAKE_CURRENT_BINARY_DIR}/..
)

target_link_libraries (${target_name}
    PUBLIC
    EnTT::EnTT
    Bullet3::Bullet3
    RmlUi::RmlUi
    service
    context
    logging
    cmd_line
    concurrency
    memory
    ospp
    audiopp
    graphics
    simulation
    serialization
    reflection
    filesystem
    math
    uuidpp
    threadpp
    assimp
    string_utils
    dotnetpp
    seq
    libunibreak
    engine_version
    crash
)

target_precompile_headers(${target_name}
    PUBLIC
    pch.h
)

set_target_properties(${target_name} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    INTERPROCEDURAL_OPTIMIZATION FALSE
)

if(UNRAVEL_UNITY_BUILD AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_target_properties(${target_name} PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_UNIQUE_ID "ANONYMOUS"
        )
endif()
