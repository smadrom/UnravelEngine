add_library(pw_runtime_api INTERFACE)
target_include_directories(pw_runtime_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(WIN32 AND UNRAVEL_BUILD_PW_RUNTIME_BRIDGE)
    add_library(pw_runtime_bridge SHARED src/pw_runtime_bridge.cpp)
    target_link_libraries(pw_runtime_bridge PRIVATE pw_runtime_api)
    target_compile_definitions(pw_runtime_bridge PRIVATE PW_RUNTIME_BRIDGE_EXPORTS)
    set_target_properties(pw_runtime_bridge PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        OUTPUT_NAME "PWRuntimeBridge"
    )

    add_executable(pw_runtime_fake_server EXCLUDE_FROM_ALL tests/pw_runtime_fake_server.cpp)
    set_target_properties(pw_runtime_fake_server PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )

    add_executable(pw_runtime_bridge_smoke EXCLUDE_FROM_ALL tests/pw_runtime_bridge_smoke.cpp)
    target_link_libraries(pw_runtime_bridge_smoke PRIVATE pw_runtime_api)
    set_target_properties(pw_runtime_bridge_smoke PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )

    add_executable(pw_session_controller_test EXCLUDE_FROM_ALL
        tests/pw_session_controller_test.cpp
        ${CMAKE_SOURCE_DIR}/editor/editor/mcp/pw_session_controller.cpp
        ${CMAKE_SOURCE_DIR}/editor/editor/mcp/pw_runtime_client.cpp
    )
    target_include_directories(pw_session_controller_test PRIVATE ${CMAKE_SOURCE_DIR}/editor)
    target_link_libraries(pw_session_controller_test PRIVATE pw_runtime_api)
    set_target_properties(pw_session_controller_test PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
endif()
