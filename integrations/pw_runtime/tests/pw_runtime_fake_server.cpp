// Manual test double for the native headless ElementClient MCP sidecar.
//
// Two modes:
//   - static (default): canned initialize plus a fixed {"connected":false}
//     style tool result for any request; used by pw_runtime_bridge_smoke.
//   - scenario: selected by the PW_FAKE_SCENARIO environment variable or by
//     argv[1] (a path to a tests/fixtures/scenarios/*.json file; relative
//     paths resolve against the current working directory). The server then
//     answers initialize and tools/list and replays scripted per-tool
//     tools/call responses in order, optionally dropping the connection
//     after a fixed number of answered calls to simulate a crash.
//
// Scenario mode is intentionally format-specific: it extracts raw JSON
// fragments from the fixture with a minimal balanced-brace scanner instead
// of pulling in a JSON library. Standard library only, C++17.

#include <Windows.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
auto try_extract_frame(std::string& buffer, std::string& message) -> bool
{
    const size_t header_end = buffer.find("\r\n\r\n");
    if(header_end == std::string::npos)
    {
        return false;
    }
    const std::string header = buffer.substr(0, header_end);
    const size_t length_offset = header.find("Content-Length:");
    if(length_offset == std::string::npos)
    {
        buffer.erase(0, header_end + 4);
        return false;
    }
    const char* length_begin = header.c_str() + length_offset + 15;
    const size_t content_length = static_cast<size_t>(std::strtoul(length_begin, nullptr, 10));
    const size_t body_begin = header_end + 4;
    if(buffer.size() < body_begin + content_length)
    {
        return false;
    }
    message.assign(buffer.data() + body_begin, content_length);
    buffer.erase(0, body_begin + content_length);
    return true;
}

auto read_request(std::string& buffer, std::string& request) -> bool
{
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    while(!try_extract_frame(buffer, request))
    {
        char chunk[512];
        DWORD bytes_read = 0;
        if(!ReadFile(input, chunk, sizeof(chunk), &bytes_read, nullptr) || bytes_read == 0)
        {
            return false;
        }
        buffer.append(chunk, bytes_read);
    }
    return true;
}

auto write_response(std::string_view response) -> bool
{
    const std::string frame = "Content-Length: " + std::to_string(response.size()) + "\r\n\r\n" + std::string(response);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    size_t written_total = 0;
    while(written_total < frame.size())
    {
        DWORD bytes_written = 0;
        if(!WriteFile(output, frame.data() + written_total, static_cast<DWORD>(frame.size() - written_total), &bytes_written, nullptr) || bytes_written == 0)
        {
            return false;
        }
        written_total += bytes_written;
    }
    return true;
}

auto extract_id(std::string_view request) -> int
{
    const size_t id_offset = request.find("\"id\"");
    if(id_offset == std::string_view::npos)
    {
        return 0;
    }
    const size_t colon_offset = request.find(':', id_offset);
    if(colon_offset == std::string_view::npos)
    {
        return 0;
    }
    return std::atoi(request.data() + colon_offset + 1);
}

auto run_static_mode() -> int
{
    std::string buffer;
    std::string request;
    while(read_request(buffer, request))
    {
        const int id = extract_id(request);
        if(request.find("notifications/initialized") != std::string::npos)
        {
            request.clear();
            continue;
        }
        std::string response;
        if(request.find("\"method\":\"initialize\"") != std::string::npos)
        {
            response = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"serverInfo\":{\"name\":\"fake\",\"version\":\"1\"}}}";
        }
        else
        {
            response = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"runtime-ok\"}],\"structuredContent\":{\"connected\":false},\"isError\":false}}";
        }
        if(!write_response(response))
        {
            return 1;
        }
        request.clear();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Scenario mode
// ---------------------------------------------------------------------------

constexpr long k_no_disconnect = -1;
constexpr long k_no_hang = -1;

struct scenario_response
{
    std::string structured; // raw structuredContent JSON object text
    bool is_error = false;  // mirrors structuredContent.ok == false
};

struct scenario_tool
{
    std::string name;
    std::vector<scenario_response> responses;
    size_t served_count = 0;
};

struct scenario_script
{
    std::vector<scenario_tool> tools;
    long disconnect_after_calls = k_no_disconnect; // exit after this many answered tools/call
    long hang_after_calls = k_no_hang;             // never answer this 1-based tools/call (timeout test)
    std::string tools_list_fixture;                // path relative to the scenario file directory
    std::string scenario_dir;
    std::string tools_array_raw;                   // lazily extracted raw "tools" array
    bool is_tools_array_loaded = false;
};

auto skip_whitespace(std::string_view text, size_t pos) -> size_t
{
    while(pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
    {
        ++pos;
    }
    return pos;
}

// Returns one past the closing bracket of the object/array starting at pos
// (which must hold '{' or '['), or npos on malformed input. Only the outer
// bracket type is counted; inner brackets of the other type are balanced by
// construction in valid JSON, and brackets inside strings are skipped.
auto scan_balanced(std::string_view text, size_t pos) -> size_t
{
    if(pos >= text.size() || (text[pos] != '{' && text[pos] != '['))
    {
        return std::string_view::npos;
    }
    const char open = text[pos];
    const char close = open == '{' ? '}' : ']';
    int depth = 0;
    bool is_in_string = false;
    bool is_escaped = false;
    for(size_t i = pos; i < text.size(); ++i)
    {
        const char c = text[i];
        if(is_in_string)
        {
            if(is_escaped)
            {
                is_escaped = false;
            }
            else if(c == '\\')
            {
                is_escaped = true;
            }
            else if(c == '"')
            {
                is_in_string = false;
            }
            continue;
        }
        if(c == '"')
        {
            is_in_string = true;
        }
        else if(c == open)
        {
            ++depth;
        }
        else if(c == close)
        {
            --depth;
            if(depth == 0)
            {
                return i + 1;
            }
        }
    }
    return std::string_view::npos;
}

// Parses the quoted string starting at pos (which must hold '"'). Fixture
// keys and the tools_list_fixture path carry no escapes; backslash pairs are
// skipped so a quoted quote cannot terminate the scan early.
auto scan_quoted_string(std::string_view text, size_t pos, size_t& end) -> std::string
{
    end = std::string_view::npos;
    if(pos >= text.size() || text[pos] != '"')
    {
        return std::string();
    }
    size_t i = pos + 1;
    while(i < text.size())
    {
        if(text[i] == '\\')
        {
            i += 2;
            continue;
        }
        if(text[i] == '"')
        {
            end = i + 1;
            return std::string(text.substr(pos + 1, i - pos - 1));
        }
        ++i;
    }
    return std::string();
}

// Locates "key" from `from` onward and returns the position of its value
// (whitespace skipped), or npos when absent.
auto find_key_value(std::string_view text, std::string_view key, size_t from) -> size_t
{
    const std::string quoted_key = "\"" + std::string(key) + "\"";
    const size_t key_offset = text.find(quoted_key, from);
    if(key_offset == std::string_view::npos)
    {
        return std::string_view::npos;
    }
    const size_t colon_offset = text.find(':', key_offset + quoted_key.size());
    if(colon_offset == std::string_view::npos)
    {
        return std::string_view::npos;
    }
    return skip_whitespace(text, colon_offset + 1);
}

auto read_text_file(const std::filesystem::path& path, std::string& content) -> bool
{
    std::ifstream stream(path, std::ios::binary);
    if(!stream)
    {
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    content = buffer.str();
    return true;
}

// The fixtures never nest an "ok" key below the structuredContent top level,
// so a plain key lookup identifies tool errors.
auto is_tool_error_response(std::string_view structured) -> bool
{
    const size_t value_offset = find_key_value(structured, "ok", 0);
    return value_offset != std::string_view::npos && value_offset < structured.size() && structured[value_offset] == 'f';
}

auto parse_scenario_tools(std::string_view text, scenario_script& script, std::string& error) -> bool
{
    size_t pos = find_key_value(text, "tools", 0);
    if(pos == std::string_view::npos || pos >= text.size() || text[pos] != '{')
    {
        error = "scenario has no top-level \"tools\" object";
        return false;
    }
    pos = skip_whitespace(text, pos + 1);
    while(pos < text.size() && text[pos] != '}')
    {
        if(text[pos] == ',')
        {
            pos = skip_whitespace(text, pos + 1);
            continue;
        }
        size_t name_end = 0;
        scenario_tool tool;
        tool.name = scan_quoted_string(text, pos, name_end);
        if(name_end == std::string_view::npos)
        {
            error = "malformed tool name in \"tools\"";
            return false;
        }
        pos = skip_whitespace(text, name_end);
        if(pos >= text.size() || text[pos] != ':')
        {
            error = "missing ':' after tool name " + tool.name;
            return false;
        }
        const size_t object_begin = skip_whitespace(text, pos + 1);
        const size_t object_end = scan_balanced(text, object_begin);
        if(object_end == std::string_view::npos)
        {
            error = "malformed tool object for " + tool.name;
            return false;
        }
        size_t responses_pos = find_key_value(text, "responses", object_begin);
        if(responses_pos == std::string_view::npos || responses_pos >= object_end || text[responses_pos] != '[')
        {
            error = "tool " + tool.name + " has no \"responses\" array";
            return false;
        }
        responses_pos = skip_whitespace(text, responses_pos + 1);
        while(responses_pos < object_end && text[responses_pos] != ']')
        {
            if(text[responses_pos] == ',')
            {
                responses_pos = skip_whitespace(text, responses_pos + 1);
                continue;
            }
            const size_t entry_end = scan_balanced(text, responses_pos);
            if(entry_end == std::string_view::npos || entry_end > object_end)
            {
                error = "malformed response entry for " + tool.name;
                return false;
            }
            scenario_response response;
            response.structured = std::string(text.substr(responses_pos, entry_end - responses_pos));
            response.is_error = is_tool_error_response(response.structured);
            tool.responses.push_back(std::move(response));
            responses_pos = skip_whitespace(text, entry_end);
        }
        if(tool.responses.empty())
        {
            error = "tool " + tool.name + " has an empty \"responses\" array";
            return false;
        }
        script.tools.push_back(std::move(tool));
        pos = skip_whitespace(text, object_end);
    }
    return true;
}

auto load_scenario(const std::string& scenario_path, scenario_script& script, std::string& error) -> bool
{
    const std::filesystem::path absolute_path = std::filesystem::absolute(scenario_path).lexically_normal();
    script.scenario_dir = absolute_path.parent_path().string();
    std::string text;
    if(!read_text_file(absolute_path, text))
    {
        error = "cannot read scenario file: " + absolute_path.string();
        return false;
    }
    const size_t disconnect_offset = find_key_value(text, "disconnect_after_calls", 0);
    if(disconnect_offset != std::string_view::npos && disconnect_offset < text.size() && text[disconnect_offset] != 'n')
    {
        script.disconnect_after_calls = std::strtol(text.data() + disconnect_offset, nullptr, 10);
    }
    const size_t hang_offset = find_key_value(text, "hang_after_calls", 0);
    if(hang_offset != std::string_view::npos && hang_offset < text.size() && text[hang_offset] != 'n')
    {
        script.hang_after_calls = std::strtol(text.data() + hang_offset, nullptr, 10);
    }
    const size_t fixture_offset = find_key_value(text, "tools_list_fixture", 0);
    if(fixture_offset != std::string_view::npos)
    {
        size_t fixture_end = 0;
        script.tools_list_fixture = scan_quoted_string(text, fixture_offset, fixture_end);
    }
    if(!parse_scenario_tools(text, script, error))
    {
        return false;
    }
    if(script.tools.empty())
    {
        error = "scenario declares no tools";
        return false;
    }
    return true;
}

// Escapes a raw JSON fragment so it can be embedded as the content text.
auto json_escape(std::string_view text) -> std::string
{
    static const char hex_digits[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(text.size() + 16);
    for(const char c : text)
    {
        switch(c)
        {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if(static_cast<unsigned char>(c) < 0x20)
            {
                escaped += "\\u00";
                escaped += hex_digits[(static_cast<unsigned char>(c) >> 4) & 0x0f];
                escaped += hex_digits[static_cast<unsigned char>(c) & 0x0f];
            }
            else
            {
                escaped += c;
            }
            break;
        }
    }
    return escaped;
}

auto extract_method(std::string_view request) -> std::string
{
    const size_t value_offset = find_key_value(request, "method", 0);
    if(value_offset == std::string_view::npos)
    {
        return std::string();
    }
    size_t end = 0;
    return scan_quoted_string(request, value_offset, end);
}

auto extract_call_name(std::string_view request) -> std::string
{
    const size_t params_offset = request.find("\"params\"");
    if(params_offset == std::string_view::npos)
    {
        return std::string();
    }
    const size_t value_offset = find_key_value(request, "name", params_offset);
    if(value_offset == std::string_view::npos)
    {
        return std::string();
    }
    size_t end = 0;
    return scan_quoted_string(request, value_offset, end);
}

auto build_initialize_response(int id) -> std::string
{
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
        ",\"result\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{\"tools\":{}},"
        "\"serverInfo\":{\"name\":\"pw_runtime_fake_server\",\"version\":\"scenario\"}}}";
}

auto build_rpc_error(int id, int code, const char* message) -> std::string
{
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
        ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"" + message + "\"}}";
}

auto build_tool_result(int id, const scenario_response& response) -> std::string
{
    return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
        ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" + json_escape(response.structured) +
        "\"}],\"structuredContent\":" + response.structured +
        ",\"isError\":" + (response.is_error ? "true" : "false") + "}}";
}

auto load_tools_array(scenario_script& script) -> bool
{
    if(script.is_tools_array_loaded)
    {
        return !script.tools_array_raw.empty();
    }
    script.is_tools_array_loaded = true;
    if(script.tools_list_fixture.empty())
    {
        return false;
    }
    const std::filesystem::path fixture_path =
        (std::filesystem::path(script.scenario_dir) / script.tools_list_fixture).lexically_normal();
    std::string text;
    if(!read_text_file(fixture_path, text))
    {
        return false;
    }
    const size_t array_offset = find_key_value(text, "tools", 0);
    if(array_offset == std::string_view::npos || array_offset >= text.size() || text[array_offset] != '[')
    {
        return false;
    }
    const size_t array_end = scan_balanced(text, array_offset);
    if(array_end == std::string_view::npos)
    {
        return false;
    }
    script.tools_array_raw = text.substr(array_offset, array_end - array_offset);
    return true;
}

auto find_scenario_tool(scenario_script& script, const std::string& name) -> scenario_tool*
{
    for(scenario_tool& tool : script.tools)
    {
        if(tool.name == name)
        {
            return &tool;
        }
    }
    return nullptr;
}

// Simulates a crashed sidecar: the pipe closes without any further frame.
[[noreturn]] void terminate_connection()
{
    CloseHandle(GetStdHandle(STD_OUTPUT_HANDLE));
    ExitProcess(0);
}

// Records only the PRESENCE and length of credential-bearing variables so
// bridge-level tests can prove the v2 credential plumbing without ever writing
// a secret value to disk.
auto write_environment_probe() -> void
{
    const char* account = std::getenv("PW_ACCOUNT");
    const char* password = std::getenv("PW_PASSWORD");
    const char* role_admin = std::getenv("PW_HEADLESS_ROLE_ADMIN");
    const char* server = std::getenv("PW_SERVER");
    std::ostringstream probe;
    probe << "{\"account_set\":" << (account != nullptr && account[0] != '\0' ? "true" : "false")
          << ",\"account_len\":" << (account != nullptr ? std::strlen(account) : 0)
          << ",\"password_set\":" << (password != nullptr && password[0] != '\0' ? "true" : "false")
          << ",\"password_len\":" << (password != nullptr ? std::strlen(password) : 0)
          << ",\"role_admin\":\"" << (role_admin != nullptr ? role_admin : "")
          << "\",\"server_set\":" << (server != nullptr && server[0] != '\0' ? "true" : "false")
          << "}";
    std::ofstream out("pw_fake_env_probe.json", std::ios::binary | std::ios::trunc);
    out << probe.str();
}

auto run_scenario_mode(const std::string& scenario_path) -> int
{
    scenario_script script;
    std::string error;
    if(!load_scenario(scenario_path, script, error))
    {
        const std::string message = "pw_runtime_fake_server: " + error + "\n";
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
        return 2;
    }
    long answered_calls = 0;
    std::string buffer;
    std::string request;
    write_environment_probe();
    while(read_request(buffer, request))
    {
        const int id = extract_id(request);
        const std::string method = extract_method(request);
        std::string response;
        if(method == "notifications/initialized" || method.empty() && request.find("notifications/initialized") != std::string::npos)
        {
            request.clear();
            continue;
        }
        if(method == "initialize")
        {
            response = build_initialize_response(id);
        }
        else if(method == "tools/list")
        {
            if(load_tools_array(script))
            {
                response = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
                    ",\"result\":{\"tools\":" + script.tools_array_raw + "}}";
            }
            else
            {
                response = build_rpc_error(id, -32603, "tools list fixture unavailable");
            }
        }
        else if(method == "tools/call")
        {
            scenario_tool* tool = find_scenario_tool(script, extract_call_name(request));
            if(tool == nullptr)
            {
                response = build_rpc_error(id, -32601, "unknown tool");
            }
            else
            {
                // Queues are ordered; the last scripted response repeats so
                // callers may poll a terminal state an arbitrary number of times.
                const size_t index = tool->served_count < tool->responses.size()
                    ? tool->served_count
                    : tool->responses.size() - 1;
                ++tool->served_count;
                response = build_tool_result(id, tool->responses[index]);
                ++answered_calls;
            }
        }
        else
        {
            response = build_rpc_error(id, -32601, "method not found");
        }
        if(script.hang_after_calls != k_no_hang && answered_calls >= script.hang_after_calls)
        {
            // Simulates a wedged sidecar: the request is accepted but never
            // answered, so the caller must surface its own timeout.
            Sleep(INFINITE);
        }
        if(!write_response(response))
        {
            return 1;
        }
        if(script.disconnect_after_calls != k_no_disconnect && answered_calls >= script.disconnect_after_calls)
        {
            terminate_connection();
        }
        request.clear();
    }
    return 0;
}
}

auto main(int argc, char** argv) -> int
{
    const char* scenario_path = argc > 1 ? argv[1] : nullptr;
    if(scenario_path == nullptr || scenario_path[0] == '\0')
    {
        scenario_path = std::getenv("PW_FAKE_SCENARIO");
    }
    if(scenario_path == nullptr || scenario_path[0] == '\0')
    {
        // Bridge-launched children receive a scrubbed allowlist environment
        // without PW_FAKE_SCENARIO, so the bridge-level tests select the
        // scenario through the child working directory instead.
        static const char cwd_scenario[] = "pw_fake_scenario.json";
        std::error_code probe_error;
        if(std::filesystem::exists(cwd_scenario, probe_error))
        {
            scenario_path = cwd_scenario;
        }
    }
    if(scenario_path != nullptr && scenario_path[0] != '\0')
    {
        return run_scenario_mode(scenario_path);
    }
    return run_static_mode();
}
