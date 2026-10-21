// Packet transport (net_transport.h). The loopback backend: one UDP socket per instance on
// 127.0.0.1:28100+n (the first free n of 64); peer id = 0x4C4F4F50'0000 | port. Localhost UDP does
// not reorder; a large receive buffer keeps it lossless in practice, which is enough for the
// two-instance test bed (Steam's reliable channel replaces it for real play).
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include "net_transport.h"
#include "steam_net.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace net {
namespace {

constexpr uint64_t kLoopTag = 0x4C4F4F5000000000ull;      // "LOOP"
constexpr uint16_t kBasePort = 28100;

std::mutex g_mutex;
std::atomic<bool> g_started{ false };
std::atomic<uint64_t> g_local{ 0 };
SOCKET g_socket = INVALID_SOCKET;
Receiver g_receiver = nullptr;
bool g_steam = false;

void FromSteam(uint64_t peer, bool reliable, const uint8_t* data, size_t size)
{
    if (g_receiver) g_receiver(peer, reliable ? Channel::Reliable : Channel::Unreliable, data, size);
}

void ReceiveLoop()
{
    std::vector<uint8_t> buffer(65536);
    for (;;)
    {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int n = recvfrom(g_socket, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n < 1) { if (n == SOCKET_ERROR && WSAGetLastError() == WSAENOTSOCK) return; continue; }
        const uint64_t peer = kLoopTag | ntohs(from.sin_port);
        // First byte: the channel; the rest is the packet.
        if (g_receiver) g_receiver(peer, Channel(buffer[0] & 1), buffer.data() + 1, size_t(n - 1));
    }
}

} // namespace

bool Start(Receiver receiver)
{
    std::lock_guard l(g_mutex);
    if (g_started) return true;
    const char* choice = std::getenv("TTT2_NET");
    const bool wantLoopback = choice && std::strcmp(choice, "loopback") == 0;
    const bool needSteam = choice && std::strcmp(choice, "steam") == 0;
    if (!wantLoopback)
    {
        g_receiver = receiver;
        if (steamnet::Start(FromSteam))
        {
            g_steam = true;
            g_local = steamnet::LocalId();
            g_started = true;
            return true;
        }
        if (needSteam) return false;
        std::fprintf(stderr, "[net] Steam unavailable - loopback transport (this PC only)\n");
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return false;
    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET) return false;
    int big = 8 << 20;
    setsockopt(g_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof(big));
    setsockopt(g_socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&big), sizeof(big));
    BOOL exclusive = TRUE;
    setsockopt(g_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    // ICMP "port unreachable" must not turn into errors on the receive loop.
    BOOL off = FALSE;
    DWORD ignored = 0;
    WSAIoctl(g_socket, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &ignored, nullptr, nullptr);
    uint16_t port = 0;
    for (uint16_t n = 0; n < 64 && !port; ++n)
    {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(kBasePort + n));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(g_socket, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) port = uint16_t(kBasePort + n);
    }
    if (!port) { closesocket(g_socket); g_socket = INVALID_SOCKET; return false; }
    g_receiver = receiver;
    g_local = kLoopTag | port;
    g_started = true;
    std::thread(ReceiveLoop).detach();
    std::fprintf(stderr, "[net] loopback transport on 127.0.0.1:%u (peer %016llX)\n", port,
                 (unsigned long long)g_local.load());
    return true;
}

uint64_t LocalPeer() { return g_local.load(); }
const char* BackendName() { return g_steam ? "steam" : "loopback"; }

bool Send(uint64_t peer, Channel channel, const void* data, size_t size)
{
    if (g_steam) return steamnet::Send(peer, channel == Channel::Reliable, data, size);
    if (!g_started || (peer & 0xFFFFFFFF00000000ull) != kLoopTag || size + 1 > 65507) return false;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(uint16_t(peer & 0xFFFF));
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    thread_local std::vector<uint8_t> packet;
    packet.resize(size + 1);
    packet[0] = uint8_t(channel);
    if (size) std::memcpy(packet.data() + 1, data, size);
    return sendto(g_socket, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0,
                  reinterpret_cast<sockaddr*>(&to), sizeof(to)) == int(packet.size());
}

} // namespace net
