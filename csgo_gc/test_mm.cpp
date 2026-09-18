#include "stdafx.h"
#include "test_mm.h"
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
static constexpr SocketHandle InvalidSocketHandle = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle InvalidSocketHandle = -1;
#endif

namespace
{

// RESEARCH_FINDINGS.md #26 -- exact wire layout, do not guess additional fields.
constexpr uint32_t ConnectionlessHeader = 0xFFFFFFFFu;
constexpr uint8_t OpcodeReserveCheck = 0x21; // A2S_RESERVE_CHECK
constexpr uint8_t OpcodeReserveCheckResponse = 0x25; // S2A_RESERVE_CHECK_RESPONSE
constexpr size_t RequestSize = 33; // 4 + 1 + 4 + 4 + 4 + 8 + 8
constexpr size_t ResponseSize = 19; // 4 + 1 + 4 + 4 + 4 + 1 + 1

uint32_t ReadU32LE(const uint8_t *data)
{
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return value; // x86/x64 host is already little-endian
}

uint64_t ReadU64LE(const uint8_t *data)
{
    uint64_t value;
    memcpy(&value, data, sizeof(value));
    return value;
}

void AppendU32LE(uint8_t *buffer, size_t &offset, uint32_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
    offset += sizeof(value);
}

void CloseSocket(SocketHandle sock)
{
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

void ResponderThread(uint16_t port)
{
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        Platform::Print("[MM-TEST] TestMM: WSAStartup failed, responder not started\n");
        return;
    }
#endif

    SocketHandle sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == InvalidSocketHandle)
    {
        Platform::Print("[MM-TEST] TestMM: socket() failed, responder not started\n");
        return;
    }

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(port);

    if (bind(sock, reinterpret_cast<sockaddr *>(&bindAddr), sizeof(bindAddr)) != 0)
    {
        Platform::Print("[MM-TEST] TestMM: bind() to UDP port %u failed -- "
            "is a real dedicated server (or something else) already listening there?\n", port);
        CloseSocket(sock);
        return;
    }

    Platform::Print("[MM-TEST] TestMM: listening for A2S_RESERVE_CHECK on UDP port %u\n", port);

    uint8_t buffer[256];
    while (true)
    {
        sockaddr_in fromAddr{};
#ifdef _WIN32
        int fromLen = sizeof(fromAddr);
#else
        socklen_t fromLen = sizeof(fromAddr);
#endif
        int received = recvfrom(sock, reinterpret_cast<char *>(buffer), sizeof(buffer), 0,
            reinterpret_cast<sockaddr *>(&fromAddr), &fromLen);

        if (received < static_cast<int>(RequestSize))
        {
            continue; // too short to be our packet (or recvfrom error) -- ignore, keep listening
        }

        uint32_t marker = ReadU32LE(buffer);
        uint8_t opcode = buffer[4];
        if (marker != ConnectionlessHeader || opcode != OpcodeReserveCheck)
        {
            continue; // not an A2S_RESERVE_CHECK packet -- ignore
        }

        uint32_t protocolVersion = ReadU32LE(buffer + 5);
        uint32_t token = ReadU32LE(buffer + 9);
        uint32_t reservationStage = ReadU32LE(buffer + 13);
        uint64_t reservationCookie = ReadU64LE(buffer + 17);
        uint64_t clientSteamId = ReadU64LE(buffer + 25);

        Platform::Print(
            "[MM-TEST] Reservation check received: protocol=%u token=0x%x stage=%u "
            "cookie=0x%llx steamid=0x%llx\n",
            protocolVersion, token, reservationStage,
            static_cast<unsigned long long>(reservationCookie),
            static_cast<unsigned long long>(clientSteamId));

        // build S2A_RESERVE_CHECK_RESPONSE, byte-exact per RESEARCH_FINDINGS.md #26.
        // awaiting_clients=0 unconditionally -- this is the minimal "'G' branch" behavior
        // of the retail CBaseServer::ReplyReservationCheckRequest, not a real player queue.
        uint8_t response[ResponseSize];
        size_t offset = 0;
        AppendU32LE(response, offset, ConnectionlessHeader);
        response[offset++] = OpcodeReserveCheckResponse;
        AppendU32LE(response, offset, protocolVersion);   // echo
        AppendU32LE(response, offset, token);              // echo
        AppendU32LE(response, offset, reservationStage);   // echo
        response[offset++] = 0; // awaiting_clients = 0 -> immediate success (status=4 client-side)
        response[offset++] = 1; // total_clients_in_reservation

        Platform::Print("[MM-TEST] Sending reservation response (awaiting_clients=0)\n");

        sendto(sock, reinterpret_cast<const char *>(response), static_cast<int>(offset), 0,
            reinterpret_cast<sockaddr *>(&fromAddr), fromLen);
    }
}

} // namespace

void TestMM::EnsureStarted(uint16_t port)
{
    static std::atomic<bool> started{ false };

    bool expected = false;
    if (!started.compare_exchange_strong(expected, true))
    {
        return; // already running (for whatever port was passed the first time)
    }

    Platform::Print("[MM-TEST] TestMM initialized\n");
    std::thread(ResponderThread, port).detach();
}
