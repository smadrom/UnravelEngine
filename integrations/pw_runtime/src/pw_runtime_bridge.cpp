#include <pw_runtime/pw_runtime_api.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr uint32_t DEFAULT_STARTUP_TIMEOUT_MS = 5000;
constexpr uint32_t DEFAULT_STOP_TIMEOUT_MS = 1000;
constexpr size_t MAX_FRAME_SIZE = 16u * 1024u * 1024u;
constexpr DWORD POLL_INTERVAL_MS = 1;
constexpr std::array<const wchar_t*, 35> CHILD_ENVIRONMENT_ALLOWLIST = {
    L"ALLUSERSPROFILE",
    L"APPDATA",
    L"COMSPEC",
    L"HOME",
    L"HOMEDRIVE",
    L"HOMEPATH",
    L"LANG",
    L"LC_ALL",
    L"LOCALAPPDATA",
    L"NUMBER_OF_PROCESSORS",
    L"OS",
    L"PATH",
    L"PATHEXT",
    L"PROCESSOR_ARCHITECTURE",
    L"PROCESSOR_IDENTIFIER",
    L"PROCESSOR_LEVEL",
    L"PROCESSOR_REVISION",
    L"PROGRAMDATA",
    L"PUBLIC",
    L"SYSTEMDRIVE",
    L"SYSTEMROOT",
    L"TEMP",
    L"TMP",
    L"TZ",
    L"USERDOMAIN",
    L"USERNAME",
    L"USERPROFILE",
    L"WINDIR",
    L"PW_ACCOUNT",
    L"PW_PASSWORD",
    L"PW_SERVER",
    L"PW_HEADLESS_ROLE_ID",
    L"PW_HEADLESS_ROLE_ADMIN",
    L"PW_HEADLESS",
    L"PW_MCP_STDIO",
};

void close_handle(HANDLE& handle)
{
    if(handle == nullptr || handle == INVALID_HANDLE_VALUE)
    {
        return;
    }
    CloseHandle(handle);
    handle = nullptr;
}

auto convert_wide_to_utf8(const std::wstring& text) -> std::string
{
    if(text.empty())
    {
        return {};
    }
    const int required_size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if(required_size <= 0)
    {
        return {};
    }
    std::string result(static_cast<size_t>(required_size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), required_size, nullptr, nullptr);
    return result;
}

auto describe_windows_error(DWORD error_code) -> std::string
{
    wchar_t* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD size = FormatMessageW(flags, nullptr, error_code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring wide_message = size > 0 && message != nullptr ? std::wstring(message, size) : L"unknown Windows error";
    if(message != nullptr)
    {
        LocalFree(message);
    }
    while(!wide_message.empty() && (wide_message.back() == L'\r' || wide_message.back() == L'\n'))
    {
        wide_message.pop_back();
    }
    return convert_wide_to_utf8(wide_message);
}

auto starts_with_environment_name(const std::wstring& entry, const wchar_t* name) -> bool
{
    const size_t name_length = std::wcslen(name);
    return entry.size() > name_length && entry[name_length] == L'=' && _wcsnicmp(entry.c_str(), name, name_length) == 0;
}

auto is_allowed_child_environment(const std::wstring& entry) -> bool
{
    return std::any_of(CHILD_ENVIRONMENT_ALLOWLIST.begin(), CHILD_ENVIRONMENT_ALLOWLIST.end(), [&](const wchar_t* name)
    {
        return starts_with_environment_name(entry, name);
    });
}

//	Explicit per-start overrides (v2). Each entry replaces any inherited value
//	with the same name; entries whose value may carry credentials are flagged so
//	the caller can scrub the temporary buffers after the child is created.
struct environment_override
{
    std::wstring name;
    std::wstring value;
    bool sensitive = false;
};

void scrub_environment_overrides(std::vector<environment_override>& overrides)
{
    for(environment_override& entry : overrides)
    {
        if(entry.sensitive && !entry.value.empty())
        {
            SecureZeroMemory(entry.value.data(), entry.value.size() * sizeof(wchar_t));
        }
    }
}

auto build_child_environment(const std::vector<environment_override>& overrides) -> std::vector<wchar_t>
{
    std::vector<std::wstring> entries;
    wchar_t* environment = GetEnvironmentStringsW();
    if(environment != nullptr)
    {
        for(const wchar_t* cursor = environment; *cursor != L'\0'; cursor += std::wcslen(cursor) + 1)
        {
            std::wstring entry(cursor);
            if(!is_allowed_child_environment(entry) ||
               starts_with_environment_name(entry, L"PW_HEADLESS") ||
               starts_with_environment_name(entry, L"PW_MCP_STDIO"))
            {
                continue;
            }
            bool overridden = false;
            for(const environment_override& override_entry : overrides)
            {
                if(starts_with_environment_name(entry, override_entry.name.c_str()))
                {
                    overridden = true;
                    break;
                }
            }
            if(overridden)
            {
                continue;
            }
            entries.emplace_back(std::move(entry));
        }
        FreeEnvironmentStringsW(environment);
    }
    for(const environment_override& override_entry : overrides)
    {
        entries.emplace_back(override_entry.name + L"=" + override_entry.value);
    }
    entries.emplace_back(L"PW_HEADLESS=1");
    entries.emplace_back(L"PW_MCP_STDIO=1");
    std::sort(entries.begin(), entries.end(), [](const std::wstring& left, const std::wstring& right)
    {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });
    size_t character_count = 1;
    for(const std::wstring& entry : entries)
    {
        character_count += entry.size() + 1;
    }
    std::vector<wchar_t> block(character_count, L'\0');
    wchar_t* destination = block.data();
    for(const std::wstring& entry : entries)
    {
        std::memcpy(destination, entry.c_str(), entry.size() * sizeof(wchar_t));
        destination += entry.size() + 1;
    }
    //	Scrub the temporary concatenated copies of sensitive overrides; the
    //	returned block is scrubbed by the caller once the child process exists.
    for(const environment_override& override_entry : overrides)
    {
        if(!override_entry.sensitive)
        {
            continue;
        }
        for(std::wstring& entry : entries)
        {
            if(starts_with_environment_name(entry, override_entry.name.c_str()))
            {
                SecureZeroMemory(entry.data(), entry.size() * sizeof(wchar_t));
            }
        }
    }
    return block;
}

auto find_case_insensitive(std::string_view text, std::string_view needle) -> size_t
{
    if(needle.empty() || text.size() < needle.size())
    {
        return std::string_view::npos;
    }
    for(size_t i = 0; i <= text.size() - needle.size(); ++i)
    {
        bool matches = true;
        for(size_t j = 0; j < needle.size(); ++j)
        {
            const unsigned char left = static_cast<unsigned char>(text[i + j]);
            const unsigned char right = static_cast<unsigned char>(needle[j]);
            if(std::tolower(left) != std::tolower(right))
            {
                matches = false;
                break;
            }
        }
        if(matches)
        {
            return i;
        }
    }
    return std::string_view::npos;
}

auto try_extract_frame(std::string& buffer, std::string& response) -> bool
{
    size_t header_end = buffer.find("\r\n\r\n");
    size_t separator_size = 4;
    if(header_end == std::string::npos)
    {
        header_end = buffer.find("\n\n");
        separator_size = 2;
    }
    if(header_end == std::string::npos)
    {
        return false;
    }
    const std::string_view headers(buffer.data(), header_end);
    const std::string_view content_length_name = "Content-Length:";
    const size_t content_length_offset = find_case_insensitive(headers, content_length_name);
    if(content_length_offset == std::string_view::npos)
    {
        buffer.erase(0, header_end + separator_size);
        return false;
    }
    size_t value_begin = content_length_offset + content_length_name.size();
    while(value_begin < headers.size() && (headers[value_begin] == ' ' || headers[value_begin] == '\t'))
    {
        ++value_begin;
    }
    size_t value_end = value_begin;
    while(value_end < headers.size() && headers[value_end] >= '0' && headers[value_end] <= '9')
    {
        ++value_end;
    }
    if(value_begin == value_end)
    {
        buffer.erase(0, header_end + separator_size);
        return false;
    }
    size_t content_length = 0;
    for(size_t i = value_begin; i < value_end; ++i)
    {
        content_length = content_length * 10 + static_cast<size_t>(headers[i] - '0');
        if(content_length > MAX_FRAME_SIZE)
        {
            buffer.clear();
            return false;
        }
    }
    const size_t body_begin = header_end + separator_size;
    if(buffer.size() < body_begin + content_length)
    {
        return false;
    }
    response.assign(buffer.data() + body_begin, content_length);
    buffer.erase(0, body_begin + content_length);
    return true;
}

class runtime_instance
{
public:
    ~runtime_instance()
    {
        std::scoped_lock lock(mutex_);
        stop_locked(DEFAULT_STOP_TIMEOUT_MS);
    }

    auto start(const pw_runtime_start_info_v1& start_info) -> pw_runtime_result
    {
        if(start_info.struct_size < sizeof(pw_runtime_start_info_v1) || start_info.executable_path == nullptr || start_info.executable_path[0] == L'\0')
        {
            return set_error(PW_RUNTIME_RESULT_INVALID_ARGUMENT, "invalid runtime start information");
        }
        std::vector<environment_override> no_overrides;
        std::scoped_lock lock(mutex_);
        return start_core_locked(start_info.executable_path, start_info.working_directory,
                                 start_info.startup_timeout_ms, no_overrides);
    }

    auto start_v2(const pw_runtime_start_info_v2& start_info) -> pw_runtime_result
    {
        if(start_info.struct_size < sizeof(pw_runtime_start_info_v2) || start_info.executable_path == nullptr || start_info.executable_path[0] == L'\0')
        {
            return set_error(PW_RUNTIME_RESULT_INVALID_ARGUMENT, "invalid runtime start information");
        }
        if((start_info.account_utf8 == nullptr && start_info.account_utf8_size != 0) ||
           (start_info.password_utf8 == nullptr && start_info.password_utf8_size != 0))
        {
            return set_error(PW_RUNTIME_RESULT_INVALID_ARGUMENT, "credential spans need both pointer and size");
        }

        //	Credentials move from the caller's byte spans into the child
        //	environment only; every temporary wide copy is scrubbed below,
        //	including on the error paths, and nothing here is logged.
        std::vector<environment_override> overrides;
        std::wstring account;
        if(!utf8_span_to_wide(start_info.account_utf8, start_info.account_utf8_size, account))
        {
            return set_error(PW_RUNTIME_RESULT_INVALID_ARGUMENT, "account must be valid UTF-8");
        }
        std::wstring password;
        if(!utf8_span_to_wide(start_info.password_utf8, start_info.password_utf8_size, password))
        {
            SecureZeroMemory(account.data(), account.size() * sizeof(wchar_t));
            return set_error(PW_RUNTIME_RESULT_INVALID_ARGUMENT, "password must be valid UTF-8");
        }
        if(!account.empty())
        {
            overrides.push_back({L"PW_ACCOUNT", account, true});
        }
        if(!password.empty())
        {
            overrides.push_back({L"PW_PASSWORD", password, true});
        }
        SecureZeroMemory(account.data(), account.size() * sizeof(wchar_t));
        SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
        if(start_info.server != nullptr && start_info.server[0] != L'\0')
        {
            overrides.push_back({L"PW_SERVER", start_info.server, false});
        }
        if(start_info.role_admin != 0)
        {
            overrides.push_back({L"PW_HEADLESS_ROLE_ADMIN", L"1", false});
        }
        else if(start_info.exact_role_id > 0)
        {
            overrides.push_back({L"PW_HEADLESS_ROLE_ID", std::to_wstring(start_info.exact_role_id), false});
        }

        std::scoped_lock lock(mutex_);
        const pw_runtime_result result = start_core_locked(start_info.executable_path,
                                                           start_info.working_directory,
                                                           start_info.startup_timeout_ms,
                                                           overrides);
        scrub_environment_overrides(overrides);
        return result;
    }

private:
    static auto utf8_span_to_wide(const uint8_t* data, uint32_t size, std::wstring& out) -> bool
    {
        out.clear();
        if(data == nullptr || size == 0)
        {
            return true;
        }
        const int required_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                      reinterpret_cast<const char*>(data),
                                                      static_cast<int>(size), nullptr, 0);
        if(required_size <= 0)
        {
            return false;
        }
        out.resize(static_cast<size_t>(required_size));
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(data),
                            static_cast<int>(size), out.data(), required_size);
        return true;
    }

    auto start_core_locked(const wchar_t* executable_path, const wchar_t* working_directory,
                           uint32_t startup_timeout_ms,
                           const std::vector<environment_override>& overrides) -> pw_runtime_result
    {
        refresh_process_state_locked();
        if(process_ != nullptr)
        {
            return set_error(PW_RUNTIME_RESULT_NOT_READY, "headless client is already running");
        }
        SECURITY_ATTRIBUTES security_attributes{};
        security_attributes.nLength = sizeof(security_attributes);
        security_attributes.bInheritHandle = TRUE;
        HANDLE child_stdin_read = nullptr;
        HANDLE child_stdout_write = nullptr;
        HANDLE child_stderr = nullptr;
        if(!CreatePipe(&child_stdin_read, &stdin_write_, &security_attributes, 0) || !SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0))
        {
            close_handle(child_stdin_read);
            reset_process_handles_locked();
            return set_windows_error(PW_RUNTIME_RESULT_PROCESS_ERROR, "cannot create child stdin");
        }
        if(!CreatePipe(&stdout_read_, &child_stdout_write, &security_attributes, 0) || !SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0))
        {
            close_handle(child_stdin_read);
            close_handle(child_stdout_write);
            reset_process_handles_locked();
            return set_windows_error(PW_RUNTIME_RESULT_PROCESS_ERROR, "cannot create child stdout");
        }
        child_stderr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security_attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);
        startup_info.dwFlags = STARTF_USESTDHANDLES;
        startup_info.hStdInput = child_stdin_read;
        startup_info.hStdOutput = child_stdout_write;
        startup_info.hStdError = child_stderr != INVALID_HANDLE_VALUE ? child_stderr : child_stdout_write;
        PROCESS_INFORMATION process_info{};
        std::wstring command_line = L"\"" + std::wstring(executable_path) + L"\"";
        std::vector<wchar_t> environment = build_child_environment(overrides);
        const wchar_t* working_directory_value = working_directory != nullptr && working_directory[0] != L'\0' ? working_directory : nullptr;
        const DWORD creation_flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
        const BOOL created = CreateProcessW(executable_path, command_line.data(), nullptr, nullptr, TRUE, creation_flags, environment.data(), working_directory_value, &startup_info, &process_info);
        const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
        // The child owns a copy of the environment from here on; scrub ours.
        SecureZeroMemory(environment.data(), environment.size() * sizeof(wchar_t));
        close_handle(child_stdin_read);
        close_handle(child_stdout_write);
        close_handle(child_stderr);
        if(!created)
        {
            reset_process_handles_locked();
            return set_windows_error(PW_RUNTIME_RESULT_PROCESS_ERROR, "cannot start headless client", create_error);
        }
        process_ = process_info.hProcess;
        process_id_ = process_info.dwProcessId;
        has_started_ = true;
        close_handle(process_info.hThread);
        read_buffer_.clear();
        last_exit_code_ = STILL_ACTIVE;
        const uint32_t timeout_ms = startup_timeout_ms == 0 ? DEFAULT_STARTUP_TIMEOUT_MS : startup_timeout_ms;
        const std::string initialize_request = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"UnravelEngine","version":"pw-runtime-bridge"}}})";
        std::string initialize_response;
        pw_runtime_result result = exchange_locked(initialize_request, timeout_ms, initialize_response);
        if(result != PW_RUNTIME_RESULT_OK)
        {
            stop_locked(DEFAULT_STOP_TIMEOUT_MS);
            return result;
        }
        if(initialize_response.find("\"result\"") == std::string::npos)
        {
            stop_locked(DEFAULT_STOP_TIMEOUT_MS);
            return set_error(PW_RUNTIME_RESULT_PROTOCOL_ERROR, "headless client rejected MCP initialization");
        }
        const std::string initialized_notification = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
        result = write_frame_locked(initialized_notification);
        if(result != PW_RUNTIME_RESULT_OK)
        {
            stop_locked(DEFAULT_STOP_TIMEOUT_MS);
            return result;
        }
        last_error_.clear();
        return PW_RUNTIME_RESULT_OK;
    }

public:
    auto stop(uint32_t timeout_ms) -> pw_runtime_result
    {
        std::scoped_lock lock(mutex_);
        return stop_locked(timeout_ms == 0 ? DEFAULT_STOP_TIMEOUT_MS : timeout_ms);
    }

    auto get_status(pw_runtime_status_v1& out_status) -> pw_runtime_result
    {
        std::scoped_lock lock(mutex_);
        if(out_status.struct_size < sizeof(pw_runtime_status_v1))
        {
            return set_error(PW_RUNTIME_RESULT_INVALID_ARGUMENT, "runtime status structure is too small");
        }
        refresh_process_state_locked();
        out_status.process_id = process_id_;
        out_status.exit_code = static_cast<int32_t>(last_exit_code_);
        if(process_ != nullptr)
        {
            out_status.process_state = PW_RUNTIME_PROCESS_RUNNING;
        }
        else if(has_started_)
        {
            out_status.process_state = PW_RUNTIME_PROCESS_EXITED;
        }
        else
        {
            out_status.process_state = PW_RUNTIME_PROCESS_STOPPED;
            out_status.exit_code = 0;
        }
        return PW_RUNTIME_RESULT_OK;
    }

    auto request(std::string_view request_json, uint32_t timeout_ms, pw_runtime_response_callback callback, void* user_data) -> pw_runtime_result
    {
        std::scoped_lock lock(mutex_);
        if(request_json.empty() || callback == nullptr)
        {
            return set_error(PW_RUNTIME_RESULT_INVALID_ARGUMENT, "request and response callback are required");
        }
        std::string response;
        const pw_runtime_result result = exchange_locked(request_json, timeout_ms, response);
        if(result != PW_RUNTIME_RESULT_OK)
        {
            return result;
        }
        callback(response.data(), static_cast<uint32_t>(response.size()), user_data);
        return PW_RUNTIME_RESULT_OK;
    }

    auto copy_last_error(char* destination, uint32_t destination_size, uint32_t& required_size) -> pw_runtime_result
    {
        std::scoped_lock lock(mutex_);
        required_size = static_cast<uint32_t>(last_error_.size() + 1);
        if(destination == nullptr || destination_size < required_size)
        {
            return PW_RUNTIME_RESULT_BUFFER_TOO_SMALL;
        }
        std::memcpy(destination, last_error_.c_str(), required_size);
        return PW_RUNTIME_RESULT_OK;
    }

private:
    auto set_error(pw_runtime_result result, std::string message) -> pw_runtime_result
    {
        last_error_ = std::move(message);
        return result;
    }

    auto set_windows_error(pw_runtime_result result, const char* context, DWORD error_code = GetLastError()) -> pw_runtime_result
    {
        last_error_ = std::string(context) + ": " + describe_windows_error(error_code);
        return result;
    }

    auto write_all_locked(const char* data, size_t size) -> pw_runtime_result
    {
        size_t written_total = 0;
        while(written_total < size)
        {
            DWORD written = 0;
            const DWORD chunk_size = static_cast<DWORD>(std::min<size_t>(size - written_total, MAXDWORD));
            if(!WriteFile(stdin_write_, data + written_total, chunk_size, &written, nullptr) || written == 0)
            {
                return set_windows_error(PW_RUNTIME_RESULT_IO_ERROR, "cannot write to headless client");
            }
            written_total += written;
        }
        return PW_RUNTIME_RESULT_OK;
    }

    auto write_frame_locked(std::string_view request_json) -> pw_runtime_result
    {
        if(process_ == nullptr || stdin_write_ == nullptr)
        {
            return set_error(PW_RUNTIME_RESULT_NOT_READY, "headless client is not running");
        }
        const std::string header = "Content-Length: " + std::to_string(request_json.size()) + "\r\n\r\n";
        pw_runtime_result result = write_all_locked(header.data(), header.size());
        if(result != PW_RUNTIME_RESULT_OK)
        {
            return result;
        }
        return write_all_locked(request_json.data(), request_json.size());
    }

    auto read_frame_locked(uint32_t timeout_ms, std::string& response) -> pw_runtime_result
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while(std::chrono::steady_clock::now() < deadline)
        {
            if(try_extract_frame(read_buffer_, response))
            {
                return PW_RUNTIME_RESULT_OK;
            }
            DWORD available = 0;
            if(!PeekNamedPipe(stdout_read_, nullptr, 0, nullptr, &available, nullptr))
            {
                refresh_process_state_locked();
                return set_windows_error(PW_RUNTIME_RESULT_IO_ERROR, "cannot read from headless client");
            }
            if(available > 0)
            {
                const DWORD read_size = std::min<DWORD>(available, 4096);
                char buffer[4096];
                DWORD bytes_read = 0;
                if(!ReadFile(stdout_read_, buffer, read_size, &bytes_read, nullptr) || bytes_read == 0)
                {
                    refresh_process_state_locked();
                    return set_windows_error(PW_RUNTIME_RESULT_IO_ERROR, "cannot read MCP response");
                }
                read_buffer_.append(buffer, bytes_read);
                if(read_buffer_.size() > MAX_FRAME_SIZE)
                {
                    read_buffer_.clear();
                    return set_error(PW_RUNTIME_RESULT_PROTOCOL_ERROR, "MCP response exceeds the frame limit");
                }
                continue;
            }
            DWORD exit_code = STILL_ACTIVE;
            if(process_ == nullptr || !GetExitCodeProcess(process_, &exit_code) || exit_code != STILL_ACTIVE)
            {
                refresh_process_state_locked();
                return set_error(PW_RUNTIME_RESULT_PROCESS_ERROR, "headless client exited before replying");
            }
            Sleep(POLL_INTERVAL_MS);
        }
        return set_error(PW_RUNTIME_RESULT_TIMEOUT, "timed out waiting for headless client MCP response");
    }

    auto exchange_locked(std::string_view request_json, uint32_t timeout_ms, std::string& response) -> pw_runtime_result
    {
        const pw_runtime_result write_result = write_frame_locked(request_json);
        if(write_result != PW_RUNTIME_RESULT_OK)
        {
            return write_result;
        }
        return read_frame_locked(timeout_ms == 0 ? DEFAULT_STARTUP_TIMEOUT_MS : timeout_ms, response);
    }

    auto stop_locked(uint32_t timeout_ms) -> pw_runtime_result
    {
        refresh_process_state_locked();
        if(process_ == nullptr)
        {
            return PW_RUNTIME_RESULT_OK;
        }
        close_handle(stdin_write_);
        DWORD wait_result = WaitForSingleObject(process_, timeout_ms);
        if(wait_result == WAIT_TIMEOUT)
        {
            if(!TerminateProcess(process_, 0))
            {
                return set_windows_error(PW_RUNTIME_RESULT_PROCESS_ERROR, "cannot stop headless client");
            }
            wait_result = WaitForSingleObject(process_, DEFAULT_STOP_TIMEOUT_MS);
        }
        if(wait_result == WAIT_FAILED)
        {
            return set_windows_error(PW_RUNTIME_RESULT_PROCESS_ERROR, "cannot wait for headless client shutdown");
        }
        refresh_process_state_locked();
        return PW_RUNTIME_RESULT_OK;
    }

    void refresh_process_state_locked()
    {
        if(process_ == nullptr)
        {
            return;
        }
        DWORD exit_code = STILL_ACTIVE;
        if(GetExitCodeProcess(process_, &exit_code) && exit_code == STILL_ACTIVE)
        {
            return;
        }
        if(exit_code != STILL_ACTIVE)
        {
            last_exit_code_ = exit_code;
        }
        close_handle(process_);
        close_handle(stdin_write_);
        close_handle(stdout_read_);
        process_id_ = 0;
        read_buffer_.clear();
    }

    void reset_process_handles_locked()
    {
        close_handle(process_);
        close_handle(stdin_write_);
        close_handle(stdout_read_);
        process_id_ = 0;
        read_buffer_.clear();
    }

    std::mutex mutex_;
    HANDLE process_ = nullptr;
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    DWORD process_id_ = 0;
    DWORD last_exit_code_ = 0;
    bool has_started_ = false;
    std::string read_buffer_;
    std::string last_error_;
};

auto to_instance(pw_runtime_handle handle) -> runtime_instance*
{
    return static_cast<runtime_instance*>(handle);
}

auto PW_RUNTIME_CALL create_runtime(pw_runtime_handle* out_handle) -> pw_runtime_result
{
    if(out_handle == nullptr)
    {
        return PW_RUNTIME_RESULT_INVALID_ARGUMENT;
    }
    runtime_instance* instance = new(std::nothrow) runtime_instance();
    if(instance == nullptr)
    {
        return PW_RUNTIME_RESULT_OUT_OF_MEMORY;
    }
    *out_handle = instance;
    return PW_RUNTIME_RESULT_OK;
}

void PW_RUNTIME_CALL destroy_runtime(pw_runtime_handle handle)
{
    delete to_instance(handle);
}

auto PW_RUNTIME_CALL start_runtime(pw_runtime_handle handle, const pw_runtime_start_info_v1* start_info) -> pw_runtime_result
{
    if(handle == nullptr || start_info == nullptr)
    {
        return PW_RUNTIME_RESULT_INVALID_ARGUMENT;
    }
    return to_instance(handle)->start(*start_info);
}

auto PW_RUNTIME_CALL stop_runtime(pw_runtime_handle handle, uint32_t timeout_ms) -> pw_runtime_result
{
    if(handle == nullptr)
    {
        return PW_RUNTIME_RESULT_INVALID_ARGUMENT;
    }
    return to_instance(handle)->stop(timeout_ms);
}

auto PW_RUNTIME_CALL get_runtime_status(pw_runtime_handle handle, pw_runtime_status_v1* out_status) -> pw_runtime_result
{
    if(handle == nullptr || out_status == nullptr)
    {
        return PW_RUNTIME_RESULT_INVALID_ARGUMENT;
    }
    return to_instance(handle)->get_status(*out_status);
}

auto PW_RUNTIME_CALL request_runtime_json(pw_runtime_handle handle,
                                          const char* request_json,
                                          uint32_t request_size,
                                          uint32_t timeout_ms,
                                          pw_runtime_response_callback callback,
                                          void* user_data) -> pw_runtime_result
{
    if(handle == nullptr || request_json == nullptr)
    {
        return PW_RUNTIME_RESULT_INVALID_ARGUMENT;
    }
    const size_t actual_size = request_size == 0 ? std::strlen(request_json) : request_size;
    if(actual_size > MAX_FRAME_SIZE)
    {
        return PW_RUNTIME_RESULT_INVALID_ARGUMENT;
    }
    return to_instance(handle)->request(std::string_view(request_json, actual_size), timeout_ms, callback, user_data);
}

auto PW_RUNTIME_CALL copy_runtime_last_error(pw_runtime_handle handle,
                                             char* destination,
                                             uint32_t destination_size,
                                             uint32_t* out_required_size) -> pw_runtime_result
{
    if(handle == nullptr || out_required_size == nullptr)
    {
        return PW_RUNTIME_RESULT_INVALID_ARGUMENT;
    }
    return to_instance(handle)->copy_last_error(destination, destination_size, *out_required_size);
}

auto PW_RUNTIME_CALL start_runtime_v2(pw_runtime_handle handle, const pw_runtime_start_info_v2* start_info) -> pw_runtime_result
{
    if(handle == nullptr || start_info == nullptr)
    {
        return PW_RUNTIME_RESULT_INVALID_ARGUMENT;
    }
    return to_instance(handle)->start_v2(*start_info);
}
}

extern "C" PW_RUNTIME_EXPORT auto PW_RUNTIME_CALL pw_runtime_get_api(uint32_t requested_version,
                                                                      uint32_t caller_struct_size,
                                                                      pw_runtime_api_v1* out_api) -> pw_runtime_result
{
    if(out_api == nullptr || caller_struct_size < sizeof(pw_runtime_api_v1))
    {
        return PW_RUNTIME_RESULT_INVALID_ARGUMENT;
    }
    if(requested_version == PW_RUNTIME_ABI_VERSION_V1)
    {
        const pw_runtime_api_v1 api = {
            sizeof(pw_runtime_api_v1),
            PW_RUNTIME_ABI_VERSION_V1,
            &create_runtime,
            &destroy_runtime,
            &start_runtime,
            &stop_runtime,
            &get_runtime_status,
            &request_runtime_json,
            &copy_runtime_last_error,
        };
        *out_api = api;
        return PW_RUNTIME_RESULT_OK;
    }
    if(requested_version == PW_RUNTIME_ABI_VERSION_V2)
    {
        if(caller_struct_size < sizeof(pw_runtime_api_v2))
        {
            return PW_RUNTIME_RESULT_INVALID_ARGUMENT;
        }
        const pw_runtime_api_v2 api_v2 = {
            sizeof(pw_runtime_api_v2),
            PW_RUNTIME_ABI_VERSION_V2,
            &create_runtime,
            &destroy_runtime,
            &start_runtime,
            &stop_runtime,
            &get_runtime_status,
            &request_runtime_json,
            &copy_runtime_last_error,
            &start_runtime_v2,
        };
        *reinterpret_cast<pw_runtime_api_v2*>(out_api) = api_v2;
        return PW_RUNTIME_RESULT_OK;
    }
    return PW_RUNTIME_RESULT_UNSUPPORTED_VERSION;
}
