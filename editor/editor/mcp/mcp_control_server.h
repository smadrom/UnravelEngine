// McpControlServer — local TCP control server for the editor's MCP bridge.
//
// Transport ONLY: it frames request/response STRINGS (4-byte little-endian length
// prefix + UTF-8 payload) and never parses JSON — the dispatcher (McpCommands)
// owns all JSON. A worker thread accepts ONE client (a single agent bridge),
// reads a request, hands it to the MAIN thread via Drain(), and blocks until the
// main thread produces the response. This guarantees every editor/bgfx call runs
// on the main thread, honoring the single-threaded engine contract (the same
// reason main.cpp latches GUI actions and consumes them in the loop).
//
// The header is platform-clean (no <winsock2.h>) so main.cpp can include it
// without pulling Winsock; the socket is stored opaquely as a uintptr_t.
#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>
#include <future>
#include <cstdint>

class McpControlServer
{
public:
    // Given a raw request-JSON string, returns the response-JSON string.
    using Dispatcher = std::function<std::string(const std::string& requestJson)>;

    McpControlServer() = default;
    ~McpControlServer();

    // Bind 127.0.0.1:port and start the accept/IO worker thread. False on failure.
    bool Start(int port);
    // Stop the worker, close sockets, fail any in-flight request, join.
    void Stop();

    bool IsRunning() const { return m_running.load(); }
    int  Port()      const { return m_port; }

    // Main-thread pump: execute all queued requests via dispatcher (on THIS thread)
    // and release the worker thread waiting on each response. Call once per frame.
    void Drain(const Dispatcher& dispatcher);

private:
    struct Pending
    {
        std::string                  req;
        std::promise<std::string>    resp;
    };

    void IoThreadMain();

    std::thread         m_thread;
    std::atomic<bool>   m_running{false};
    int                 m_port = 0;
    std::uintptr_t      m_listenSock = ~std::uintptr_t(0);
    std::uintptr_t      m_clientSock = ~std::uintptr_t(0);
    std::mutex          m_mutex;
    std::deque<Pending> m_queue;
};
