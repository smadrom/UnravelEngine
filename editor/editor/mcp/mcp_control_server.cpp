#include "mcp_control_server.h"

#include <logging/logging.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <vector>
#include <chrono>
#include <cstring>
#include <exception>

#pragma comment(lib, "ws2_32.lib")

namespace
{
    constexpr DWORD kClientIoTimeoutMs = 5000;

    const char* SocketErrorName(int error)
    {
        switch(error)
        {
            case WSAETIMEDOUT:
                return "timeout";
            case WSAECONNRESET:
                return "connection reset";
            case WSAECONNABORTED:
                return "connection aborted";
            case WSAENOTSOCK:
                return "not a socket";
            case WSAESHUTDOWN:
                return "socket shutdown";
            default:
                return "socket error";
        }
    }

    bool RecvAll(SOCKET s, char* buf, int n, const char* label)
    {
        int got = 0;
        while (got < n)
        {
            int r = recv(s, buf + got, n - got, 0);
            if (r == 0)
            {
                APPLOG_DEBUG("mcp transport client closed while reading {} ({}/{})", label, got, n);
                return false;
            }
            if (r == SOCKET_ERROR)
            {
                const int error = WSAGetLastError();
                APPLOG_INFO("mcp transport closing client: {} while reading {} ({}/{}, wsa={})",
                            SocketErrorName(error),
                            label,
                            got,
                            n,
                            error);
                return false;
            }
            got += r;
        }
        return true;
    }

    bool SendAll(SOCKET s, const char* buf, int n, const char* label)
    {
        int sent = 0;
        while (sent < n)
        {
            int r = send(s, buf + sent, n - sent, 0);
            if (r == 0)
            {
                APPLOG_DEBUG("mcp transport client closed while sending {} ({}/{})", label, sent, n);
                return false;
            }
            if (r == SOCKET_ERROR)
            {
                const int error = WSAGetLastError();
                APPLOG_INFO("mcp transport closing client: {} while sending {} ({}/{}, wsa={})",
                            SocketErrorName(error),
                            label,
                            sent,
                            n,
                            error);
                return false;
            }
            sent += r;
        }
        return true;
    }

    bool RecvFrame(SOCKET s, std::string& out)
    {
        unsigned char len4[4];
        if (!RecvAll(s, (char*)len4, 4, "frame length")) return false;
        std::uint32_t n = (std::uint32_t)len4[0]
                        | ((std::uint32_t)len4[1] << 8)
                        | ((std::uint32_t)len4[2] << 16)
                        | ((std::uint32_t)len4[3] << 24);
        if (n > 64u * 1024u * 1024u)
        {
            APPLOG_INFO("mcp transport closing client: frame too large ({} bytes)", n);
            return false;
        }
        out.resize(n);
        if (n && !RecvAll(s, &out[0], (int)n, "frame body")) return false;
        return true;
    }

    bool SendFrame(SOCKET s, const std::string& in)
    {
        std::uint32_t n = (std::uint32_t)in.size();
        unsigned char len4[4] = {
            (unsigned char)(n & 0xff), (unsigned char)((n >> 8) & 0xff),
            (unsigned char)((n >> 16) & 0xff), (unsigned char)((n >> 24) & 0xff)
        };
        if (!SendAll(s, (char*)len4, 4, "frame length")) return false;
        if (n && !SendAll(s, in.data(), (int)n, "frame body")) return false;
        return true;
    }
}

McpControlServer::~McpControlServer()
{
    Stop();
}

bool McpControlServer::Start(int port)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        APPLOG_ERROR("mcp control server failed to initialize Winsock");
        return false;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        APPLOG_ERROR("mcp control server failed to create socket: wsa={}", WSAGetLastError());
        WSACleanup();
        return false;
    }

    BOOL exclusive = TRUE;
    if (setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive)) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        APPLOG_ERROR("mcp control server failed to request exclusive port ownership: wsa={}", error);
        closesocket(s);
        WSACleanup();
        return false;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);   // local only

    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        APPLOG_ERROR("mcp control server failed to bind 127.0.0.1:{}: wsa={}", port, error);
        closesocket(s);
        WSACleanup();
        return false;
    }
    if (listen(s, SOMAXCONN) == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        APPLOG_ERROR("mcp control server failed to listen on 127.0.0.1:{}: wsa={}", port, error);
        closesocket(s);
        WSACleanup();
        return false;
    }

    m_listenSock = (std::uintptr_t)s;
    m_port = port;
    m_running.store(true);
    m_thread = std::thread(&McpControlServer::IoThreadMain, this);
    APPLOG_INFO("mcp control server listening on 127.0.0.1:{}", port);
    return true;
}

void McpControlServer::Stop()
{
    if (!m_running.exchange(false) && !m_thread.joinable())
        return;

    if (m_listenSock != ~std::uintptr_t(0)) closesocket((SOCKET)m_listenSock);
    if (m_clientSock != ~std::uintptr_t(0)) closesocket((SOCKET)m_clientSock);

    // Release any in-flight request so the worker's wait loop bails (it also polls
    // m_running, so this is belt-and-suspenders).
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& p : m_queue)
        {
            try { p.resp.set_value("{\"ok\":false,\"error\":\"server stopping\"}"); }
            catch (...) {}
        }
        m_queue.clear();
    }

    if (m_thread.joinable()) m_thread.join();

    m_listenSock = ~std::uintptr_t(0);
    m_clientSock = ~std::uintptr_t(0);
    WSACleanup();
}

void McpControlServer::IoThreadMain()
{
    APPLOG_INFO("mcp transport worker started: port={} listen_socket={}", m_port, m_listenSock);
    while (m_running.load())
    {
        sockaddr_in peer;
        int peerLen = sizeof(peer);
        SOCKET c = accept((SOCKET)m_listenSock, (sockaddr*)&peer, &peerLen);
        if (c == INVALID_SOCKET)
        {
            if (!m_running.load()) break;
            const int error = WSAGetLastError();
            APPLOG_INFO("mcp transport accept failed: {}", SocketErrorName(error));
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        m_clientSock = (std::uintptr_t)c;

        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&kClientIoTimeoutMs, sizeof(kClientIoTimeoutMs));
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char*)&kClientIoTimeoutMs, sizeof(kClientIoTimeoutMs));
        BOOL noDelay = TRUE;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));

        char peerAddr[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, peerAddr, sizeof(peerAddr));
        APPLOG_INFO("mcp transport client accepted: {}:{}", peerAddr, ntohs(peer.sin_port));

        // Serve this client request-by-request (strictly one in flight).
        for (;;)
        {
            std::string req;
            if (!RecvFrame(c, req)) break;
            if (!m_running.load())  break;

            std::promise<std::string> pr;
            std::future<std::string>  fut = pr.get_future();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_queue.push_back(Pending{ std::move(req), std::move(pr) });
                APPLOG_INFO("mcp transport request queued: queue={}", m_queue.size());
            }

            // Wait for the main thread (Drain) to produce the response, but bail
            // promptly if we're stopping so Stop()'s join never hangs.
            std::string resp;
            bool got = false;
            for (;;)
            {
                if (fut.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready)
                {
                    resp = fut.get();
                    got = true;
                    break;
                }
                if (!m_running.load()) break;
            }
            if (!got) break;
            if (!SendFrame(c, resp)) break;
            APPLOG_INFO("mcp transport response sent: bytes={}", resp.size());
        }

        closesocket(c);
        m_clientSock = ~std::uintptr_t(0);
        APPLOG_INFO("mcp transport client closed");
    }
    APPLOG_INFO("mcp transport worker stopped");
}

void McpControlServer::Drain(const Dispatcher& dispatcher)
{
    std::vector<Pending> local;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_queue.empty())
        {
            local.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
        }
    }
    if (!local.empty())
    {
        APPLOG_INFO("mcp drain dispatching {} request(s)", local.size());
    }
    for (auto& p : local)
    {
        std::string resp;
        try { resp = dispatcher(p.req); }
        catch (const std::exception& e)
        {
            APPLOG_ERROR("mcp dispatch exception: {}", e.what());
            resp = "{\"ok\":false,\"error\":\"dispatch exception\"}";
        }
        catch (...)
        {
            APPLOG_ERROR("mcp dispatch unknown exception");
            resp = "{\"ok\":false,\"error\":\"dispatch exception\"}";
        }
        try { p.resp.set_value(std::move(resp)); }
        catch (...) {}
    }
}
