#include <pw_runtime/pw_runtime_api.h>

#include <Windows.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace
{
void PW_RUNTIME_CALL capture_response(const char* response_json, uint32_t response_size, void* user_data)
{
    auto* response = static_cast<std::string*>(user_data);
    response->assign(response_json, response_size);
}

auto fail(const char* message) -> int
{
    std::cerr << message << '\n';
    return 1;
}

auto copy_runtime_error(const pw_runtime_api_v1& api, pw_runtime_handle handle) -> std::string
{
    uint32_t required_size = 0;
    if(api.copy_last_error(handle, nullptr, 0, &required_size) != PW_RUNTIME_RESULT_BUFFER_TOO_SMALL || required_size == 0)
    {
        return "unknown runtime error";
    }
    std::string error(required_size, '\0');
    if(api.copy_last_error(handle, error.data(), required_size, &required_size) != PW_RUNTIME_RESULT_OK)
    {
        return "unknown runtime error";
    }
    error.resize(std::char_traits<char>::length(error.c_str()));
    return error;
}
}

auto wmain(int argument_count, wchar_t** arguments) -> int
{
    if(argument_count < 3 || argument_count > 4)
    {
        return fail("usage: pw_runtime_bridge_smoke <bridge.dll> <mcp-server.exe> [working-directory]");
    }
    HMODULE library = LoadLibraryW(arguments[1]);
    if(library == nullptr)
    {
        return fail("cannot load runtime bridge");
    }
    const auto get_api = reinterpret_cast<pw_runtime_get_api_fn>(GetProcAddress(library, "pw_runtime_get_api"));
    if(get_api == nullptr)
    {
        FreeLibrary(library);
        return fail("runtime bridge has no API entry point");
    }
    pw_runtime_api_v1 api{};
    if(get_api(PW_RUNTIME_ABI_VERSION, sizeof(api), &api) != PW_RUNTIME_RESULT_OK)
    {
        FreeLibrary(library);
        return fail("runtime bridge rejected ABI version 1");
    }
    pw_runtime_handle handle = nullptr;
    if(api.create(&handle) != PW_RUNTIME_RESULT_OK || handle == nullptr)
    {
        FreeLibrary(library);
        return fail("runtime bridge could not create an instance");
    }
    const std::filesystem::path server_path(arguments[2]);
    const std::wstring working_directory = argument_count == 4 ? arguments[3] : server_path.parent_path().wstring();
    pw_runtime_start_info_v1 start_info{};
    start_info.struct_size = sizeof(start_info);
    start_info.executable_path = arguments[2];
    start_info.working_directory = working_directory.c_str();
    start_info.startup_timeout_ms = 45000;
    if(api.start(handle, &start_info) != PW_RUNTIME_RESULT_OK)
    {
        const std::string error = copy_runtime_error(api, handle);
        api.destroy(handle);
        FreeLibrary(library);
        std::cerr << "runtime bridge start failed: " << error << '\n';
        return 1;
    }
    pw_runtime_status_v1 status{};
    status.struct_size = sizeof(status);
    if(api.get_status(handle, &status) != PW_RUNTIME_RESULT_OK || status.process_state != PW_RUNTIME_PROCESS_RUNNING || status.process_id == 0)
    {
        api.destroy(handle);
        FreeLibrary(library);
        return fail("runtime bridge did not report a running child");
    }
    const std::string request = R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"pw.status","arguments":{}}})";
    std::string response;
    const pw_runtime_result request_result = api.request_json(handle, request.data(), static_cast<uint32_t>(request.size()), 45000, &capture_response, &response);
    if(request_result != PW_RUNTIME_RESULT_OK || response.find("\"connected\":false") == std::string::npos)
    {
        const std::string error = request_result != PW_RUNTIME_RESULT_OK ? copy_runtime_error(api, handle) : "unexpected pw.status response";
        api.destroy(handle);
        FreeLibrary(library);
        std::cerr << "runtime bridge request failed: " << error << '\n';
        return 1;
    }
    if(api.stop(handle, 3000) != PW_RUNTIME_RESULT_OK)
    {
        api.destroy(handle);
        FreeLibrary(library);
        return fail("runtime bridge could not stop the child");
    }
    api.destroy(handle);
    FreeLibrary(library);
    std::cout << "PW runtime bridge smoke PASS\n";
    return 0;
}
