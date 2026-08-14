#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define PW_RUNTIME_CALL __cdecl
#if defined(PW_RUNTIME_BRIDGE_EXPORTS)
#define PW_RUNTIME_EXPORT __declspec(dllexport)
#else
#define PW_RUNTIME_EXPORT
#endif
#else
#define PW_RUNTIME_CALL
#define PW_RUNTIME_EXPORT
#endif

#ifdef __cplusplus
extern "C"
{
#endif

enum
{
    PW_RUNTIME_ABI_VERSION_V1 = 1,
    PW_RUNTIME_ABI_VERSION_V2 = 2,
    PW_RUNTIME_ABI_VERSION = PW_RUNTIME_ABI_VERSION_V2
};

typedef void* pw_runtime_handle;

typedef enum pw_runtime_result
{
    PW_RUNTIME_RESULT_OK = 0,
    PW_RUNTIME_RESULT_INVALID_ARGUMENT = 1,
    PW_RUNTIME_RESULT_UNSUPPORTED_VERSION = 2,
    PW_RUNTIME_RESULT_NOT_READY = 3,
    PW_RUNTIME_RESULT_PROCESS_ERROR = 4,
    PW_RUNTIME_RESULT_IO_ERROR = 5,
    PW_RUNTIME_RESULT_TIMEOUT = 6,
    PW_RUNTIME_RESULT_PROTOCOL_ERROR = 7,
    PW_RUNTIME_RESULT_BUFFER_TOO_SMALL = 8,
    PW_RUNTIME_RESULT_OUT_OF_MEMORY = 9
} pw_runtime_result;

typedef enum pw_runtime_process_state
{
    PW_RUNTIME_PROCESS_STOPPED = 0,
    PW_RUNTIME_PROCESS_RUNNING = 1,
    PW_RUNTIME_PROCESS_EXITED = 2
} pw_runtime_process_state;

typedef struct pw_runtime_start_info_v1
{
    uint32_t struct_size;
    const wchar_t* executable_path;
    const wchar_t* working_directory;
    uint32_t startup_timeout_ms;
} pw_runtime_start_info_v1;

/* v2 start info. Credentials arrive as in-memory UTF-8 byte spans so they never
   pass through MCP JSON, the command line, or logs. The bridge copies them into
   the child environment block only and securely erases every temporary buffer it
   owns after CreateProcess. A null span or zero size means "not provided": the
   bridge then keeps the inherited-allowlist behavior for that value. */
typedef struct pw_runtime_start_info_v2
{
    uint32_t struct_size;
    const wchar_t* executable_path;
    const wchar_t* working_directory;
    uint32_t startup_timeout_ms;
    uint32_t role_admin;             /* 0 = stock auto login, 1 = hold at character select */
    const uint8_t* account_utf8;     /* optional account name byte span (UTF-8, not NUL-terminated) */
    uint32_t account_utf8_size;
    const uint8_t* password_utf8;    /* optional password byte span (UTF-8, not NUL-terminated) */
    uint32_t password_utf8_size;
    const wchar_t* server;           /* optional "ip:port" for PW_SERVER */
    int32_t exact_role_id;           /* 0 = none; with role_admin == 0 selects PW_HEADLESS_ROLE_ID */
} pw_runtime_start_info_v2;

typedef struct pw_runtime_status_v1
{
    uint32_t struct_size;
    uint32_t process_state;
    uint32_t process_id;
    int32_t exit_code;
} pw_runtime_status_v1;

typedef void(PW_RUNTIME_CALL* pw_runtime_response_callback)(const char* response_json,
                                                            uint32_t response_size,
                                                            void* user_data);

typedef struct pw_runtime_api_v1
{
    uint32_t struct_size;
    uint32_t abi_version;
    pw_runtime_result(PW_RUNTIME_CALL* create)(pw_runtime_handle* out_handle);
    void(PW_RUNTIME_CALL* destroy)(pw_runtime_handle handle);
    pw_runtime_result(PW_RUNTIME_CALL* start)(pw_runtime_handle handle,
                                              const pw_runtime_start_info_v1* start_info);
    pw_runtime_result(PW_RUNTIME_CALL* stop)(pw_runtime_handle handle, uint32_t timeout_ms);
    pw_runtime_result(PW_RUNTIME_CALL* get_status)(pw_runtime_handle handle,
                                                   pw_runtime_status_v1* out_status);
    pw_runtime_result(PW_RUNTIME_CALL* request_json)(pw_runtime_handle handle,
                                                     const char* request_json,
                                                     uint32_t request_size,
                                                     uint32_t timeout_ms,
                                                     pw_runtime_response_callback callback,
                                                     void* user_data);
    pw_runtime_result(PW_RUNTIME_CALL* copy_last_error)(pw_runtime_handle handle,
                                                       char* destination,
                                                       uint32_t destination_size,
                                                       uint32_t* out_required_size);
} pw_runtime_api_v1;

/* v2 extends v1 with the credentials-carrying start. The layout prefix matches
   pw_runtime_api_v1 exactly, so a v2 table can be read through a v1 view. */
typedef struct pw_runtime_api_v2
{
    uint32_t struct_size;
    uint32_t abi_version;
    pw_runtime_result(PW_RUNTIME_CALL* create)(pw_runtime_handle* out_handle);
    void(PW_RUNTIME_CALL* destroy)(pw_runtime_handle handle);
    pw_runtime_result(PW_RUNTIME_CALL* start)(pw_runtime_handle handle,
                                              const pw_runtime_start_info_v1* start_info);
    pw_runtime_result(PW_RUNTIME_CALL* stop)(pw_runtime_handle handle, uint32_t timeout_ms);
    pw_runtime_result(PW_RUNTIME_CALL* get_status)(pw_runtime_handle handle,
                                                   pw_runtime_status_v1* out_status);
    pw_runtime_result(PW_RUNTIME_CALL* request_json)(pw_runtime_handle handle,
                                                     const char* request_json,
                                                     uint32_t request_size,
                                                     uint32_t timeout_ms,
                                                     pw_runtime_response_callback callback,
                                                     void* user_data);
    pw_runtime_result(PW_RUNTIME_CALL* copy_last_error)(pw_runtime_handle handle,
                                                       char* destination,
                                                       uint32_t destination_size,
                                                       uint32_t* out_required_size);
    pw_runtime_result(PW_RUNTIME_CALL* start_v2)(pw_runtime_handle handle,
                                                 const pw_runtime_start_info_v2* start_info);
} pw_runtime_api_v2;

typedef pw_runtime_result(PW_RUNTIME_CALL* pw_runtime_get_api_fn)(uint32_t requested_version,
                                                                  uint32_t caller_struct_size,
                                                                  pw_runtime_api_v1* out_api);

/* Version negotiation: request PW_RUNTIME_ABI_VERSION_V1 with a pw_runtime_api_v1
   destination, or PW_RUNTIME_ABI_VERSION_V2 with a pw_runtime_api_v2 destination
   (passed through the same pw_runtime_api_v1* pointer type to keep the exported
   signature stable). caller_struct_size must cover the requested table. */
PW_RUNTIME_EXPORT pw_runtime_result PW_RUNTIME_CALL pw_runtime_get_api(uint32_t requested_version,
                                                                       uint32_t caller_struct_size,
                                                                       pw_runtime_api_v1* out_api);

#ifdef __cplusplus
}
#endif
