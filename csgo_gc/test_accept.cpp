#include "stdafx.h"
#include "test_accept.h"

#include <chrono>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <funchook.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
static constexpr SocketHandle InvalidSocketHandle = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle InvalidSocketHandle = -1;
#endif

namespace AcceptTest
{

namespace
{

// wire layout: RESEARCH_FINDINGS.md #26 (0x21 = A2S_RESERVE_CHECK, 0x25 = S2A_RESERVE_CHECK_RESPONSE)
constexpr uint32_t ConnectionlessHeader = 0xFFFFFFFFu;
constexpr uint8_t OpcodeReserveCheck = 0x21;
constexpr uint8_t OpcodeReserveCheckResponse = 0x25;
constexpr size_t ReserveCheckSize = 33;         // 4 + 1 + 4 + 4 + 4 + 8 + 8
constexpr size_t ReserveCheckResponseSize = 19; // 4 + 1 + 4 + 4 + 4 + 1 + 1

// CBaseServer::GetHostVersion() == the client's protocol constant == ClientVersion of steam.inf
// (1.38.0.5 -> 13805). RESEARCH_FINDINGS.md #24. The engine silently drops 0x21 with any other value.
constexpr uint32_t HostVersion = 13805;

const Mode s_modes[] = {
    { "competitive", 8, 10, "de_dust2" },
    { "wingman", 10, 4, "de_lake" },
    { "dangerzone", 13, 16, "dz_blacksite" },
};

uint32_t ReadU32(const uint8_t *data)
{
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return value;
}

void WriteU32(uint8_t *buffer, size_t &offset, uint32_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
    offset += sizeof(value);
}

void WriteU64(uint8_t *buffer, size_t &offset, uint64_t value)
{
    memcpy(buffer + offset, &value, sizeof(value));
    offset += sizeof(value);
}

// individual account, public universe, desktop instance -- the engine only looks at the AccountID part
uint64_t SteamIdFromAccountId(uint32_t accountId)
{
    return (1ull << 56) | (1ull << 52) | (1ull << 32) | accountId;
}

} // namespace

const Mode *FindModeByName(std::string_view name)
{
    for (const Mode &mode : s_modes)
    {
        if (name == mode.name)
        {
            return &mode;
        }
    }

    return nullptr;
}

const Mode *FindModeByGame(uint32_t eGame)
{
    for (const Mode &mode : s_modes)
    {
        if (eGame == mode.eGame)
        {
            return &mode;
        }
    }

    return nullptr;
}

std::string BuildQueuedReservationPayload(uint64_t cookie, uint32_t realAccountId, uint32_t rosterSize)
{
    // format confirmed against CBaseServer::ReserveServerForQueuedGame / SetReservationCookie
    // (baseserver.cpp, RESEARCH_FINDINGS.md #29/#41): "%llx,%llx,%d:" = cookie, match id, bReserve
    // followed by one "[%x]" AccountID token per roster slot, in roster order.
    char header[64];
    snprintf(header, sizeof(header), "Q%llx,%llx,1:",
        static_cast<unsigned long long>(cookie), static_cast<unsigned long long>(cookie));

    std::string payload = header;

    char token[16];
    snprintf(token, sizeof(token), "[%x]", realAccountId);
    payload += token;

    for (uint32_t i = 1; i < rosterSize; i++)
    {
        snprintf(token, sizeof(token), "[%x]", FakeAccountId(i));
        payload += token;
    }

    return payload;
}

std::string BuildUnreservePayload(uint64_t cookie)
{
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "Q%llx,%llx,0:",
        static_cast<unsigned long long>(cookie), static_cast<unsigned long long>(cookie));
    return buffer;
}

// ---- server side ------------------------------------------------------------------------------

namespace
{

void CloseSocket(SocketHandle sock)
{
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

enum Command : int
{
    CommandNone,
    CommandArm,   // first reservation
    CommandRearm, // unreserve, wait for the engine to drop the cookie, reserve again with a fresh roster
    CommandPause, // a real client is on the server, leave everything alone
};

enum class Phase
{
    Paused,
    Cooldown, // unreserved, waiting for the engine to actually clear the cookie (and with it the roster)
    Probe,  // fakes send stage 1 until the popup is up (server answers stage 1 with awaiting == 0)
    Accept, // fakes send stage 2 one by one
    Done,
};

} // namespace

struct FakeRoster::Impl
{
    Params params;
    PostReserveFn postReserve;

    std::atomic<int> command{ CommandArm };
    std::atomic<bool> stopping{ false };
    std::thread thread;

    void Run();
};

FakeRoster::FakeRoster(const Params &params, PostReserveFn postReserve)
    : m_impl{ new Impl }
{
    m_impl->params = params;
    m_impl->postReserve = std::move(postReserve);
    m_impl->thread = std::thread{ &Impl::Run, m_impl };

    Platform::Print("[MM-ACCEPT] FakeRoster started: roster=%u (1 real + %u fake), server=%s:%u, accept delay=%ums\n",
        params.rosterSize, params.rosterSize - 1, params.serverAddress.c_str(), params.serverPort, params.acceptDelayMs);
}

FakeRoster::~FakeRoster()
{
    m_impl->stopping = true;
    m_impl->thread.join();
    delete m_impl;
}

void FakeRoster::OnMatchStarted()
{
    m_impl->command = CommandPause;
}

void FakeRoster::OnMatchEnded()
{
    m_impl->command = CommandRearm;
}

void FakeRoster::Impl::Run()
{
    using Clock = std::chrono::steady_clock;
    using std::chrono::milliseconds;
    using std::chrono::seconds;

    // reservation expires 21 s after the last ReserveServerForQueuedGame (sv_mmqueue_reservation_timeout,
    // baseserver.cpp) and nobody tells srcds when a player pressed Play, so keep refreshing it. Refreshing
    // with the same cookie doesn't touch the roster (SetReservationCookie only rebuilds it on a cookie change).
    constexpr auto KeepAliveInterval = seconds(8);
    constexpr auto FirstProbeDelay = milliseconds(1500);  // give the main thread time to run the reservation
    constexpr auto ProbeInterval = milliseconds(1000);    // same cadence as the client's own 0x21 retries
    constexpr auto AcceptStagger = milliseconds(150);     // one fake every 150 ms so the popup counter visibly moves
    constexpr auto DoneRearmDelay = seconds(90);          // nobody connected: start over with a clean roster
    // Unreserve() only zeroes the expiry time; the cookie (and the roster with its stale stages) is dropped later by
    // CGameServer::UpdateHibernationState once nobody is connected for sv_hibernate_postgame_delay (5 s by default).
    // Reserving the same cookie before that would keep the old roster, so wait it out.
    constexpr auto UnreserveCooldown = seconds(12);

    const uint32_t fakeCount = params.rosterSize > 0 ? params.rosterSize - 1 : 0;

    const std::string reservePayload = BuildQueuedReservationPayload(params.cookie, params.realAccountId, params.rosterSize);
    const std::string unreservePayload = BuildUnreservePayload(params.cookie);

#ifdef _WIN32
    WSADATA wsaData;
    bool socketsReady = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
    bool socketsReady = true;
#endif

    SocketHandle sock = InvalidSocketHandle;
    sockaddr_in serverAddr{};

    if (socketsReady)
    {
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(params.serverPort);
        if (inet_pton(AF_INET, params.serverAddress.c_str(), &serverAddr.sin_addr) != 1)
        {
            Platform::Print("[MM-ACCEPT] FakeRoster: bad server address '%s', fake players disabled\n", params.serverAddress.c_str());
        }
        else
        {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock != InvalidSocketHandle)
            {
                // explicit bind so recvfrom works before the first sendto; the server records the
                // source address of our datagrams and broadcasts 0x25 back to it
                sockaddr_in bindAddr{};
                bindAddr.sin_family = AF_INET;
                bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
                bindAddr.sin_port = 0;
                if (bind(sock, reinterpret_cast<sockaddr *>(&bindAddr), sizeof(bindAddr)) != 0)
                {
                    CloseSocket(sock);
                    sock = InvalidSocketHandle;
                }
            }

            if (sock == InvalidSocketHandle)
            {
                Platform::Print("[MM-ACCEPT] FakeRoster: could not create a UDP socket, fake players disabled\n");
            }
        }
    }

    // one fake's 0x21 datagram, byte-exact with the real client's request (see #26)
    auto sendReserveCheck = [&](uint32_t fakeIndex, uint32_t stage)
    {
        if (sock == InvalidSocketHandle)
        {
            return;
        }

        uint8_t packet[ReserveCheckSize];
        size_t offset = 0;
        WriteU32(packet, offset, ConnectionlessHeader);
        packet[offset++] = OpcodeReserveCheck;
        WriteU32(packet, offset, HostVersion);
        WriteU32(packet, offset, FakeAccountId(fakeIndex)); // token, echoed back by the server, any value
        WriteU32(packet, offset, stage);
        WriteU64(packet, offset, params.cookie);
        WriteU64(packet, offset, SteamIdFromAccountId(FakeAccountId(fakeIndex)));

        sendto(sock, reinterpret_cast<const char *>(packet), static_cast<int>(offset), 0,
            reinterpret_cast<const sockaddr *>(&serverAddr), sizeof(serverAddr));
    };

    Phase phase = Phase::Paused;
    Clock::time_point lastReserve{}, nextProbe{}, nextAccept{}, doneAt{}, cooldownUntil{};
    uint32_t acceptedFakes = 0;

    auto armReservation = [&](Clock::time_point now)
    {
        Platform::Print("[MM-ACCEPT] arming Q reservation: %s\n", reservePayload.c_str());
        postReserve(reservePayload);

        phase = Phase::Probe;
        lastReserve = now;
        nextProbe = now + FirstProbeDelay;
        acceptedFakes = 0;
    };

    while (!stopping)
    {
        Clock::time_point now = Clock::now();

        int cmd = command.exchange(CommandNone);
        if (phase == Phase::Done && now - doneAt >= DoneRearmDelay)
        {
            Platform::Print("[MM-ACCEPT] nobody connected for %lld s after the fake accept, re-arming a fresh roster\n",
                static_cast<long long>(DoneRearmDelay.count()));
            cmd = CommandRearm;
        }

        if (cmd == CommandPause)
        {
            Platform::Print("[MM-ACCEPT] a client connected, roster left alone until it disconnects\n");
            phase = Phase::Paused;
        }
        else if (cmd == CommandArm)
        {
            armReservation(now);
        }
        else if (cmd == CommandRearm)
        {
            Platform::Print("[MM-ACCEPT] unreserving, fresh roster in %lld s\n", static_cast<long long>(UnreserveCooldown.count()));
            postReserve(unreservePayload);

            phase = Phase::Cooldown;
            cooldownUntil = now + UnreserveCooldown;
        }
        else if (phase == Phase::Cooldown)
        {
            if (now >= cooldownUntil)
            {
                armReservation(now);
            }
        }
        else if (phase != Phase::Paused && now - lastReserve >= KeepAliveInterval)
        {
            postReserve(reservePayload);
            lastReserve = now;
        }

        if (phase == Phase::Probe && now >= nextProbe)
        {
            for (uint32_t i = 1; i <= fakeCount; i++)
            {
                sendReserveCheck(i, 1);
            }

            nextProbe = now + ProbeInterval;
        }

        if (phase == Phase::Accept && now >= nextAccept)
        {
            sendReserveCheck(++acceptedFakes, 2);
            nextAccept = now + AcceptStagger;

            if (acceptedFakes >= fakeCount)
            {
                Platform::Print("[MM-ACCEPT] all %u fake players sent stage 2\n", fakeCount);
                phase = Phase::Done;
                doneAt = now;
            }
        }

        if (sock == InvalidSocketHandle)
        {
            std::this_thread::sleep_for(milliseconds(20));
            continue;
        }

        // wait a little for the server's answers/broadcasts, then drain everything that is queued
        int timeoutMs = 20;
        while (true)
        {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);

            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = timeoutMs * 1000;
            timeoutMs = 0;

            if (select(static_cast<int>(sock) + 1, &readSet, nullptr, nullptr, &timeout) <= 0)
            {
                break;
            }

            uint8_t buffer[128];
            int received = recvfrom(sock, reinterpret_cast<char *>(buffer), sizeof(buffer), 0, nullptr, nullptr);
            if (received < 0)
            {
                break; // e.g. WSAECONNRESET from an ICMP unreachable, nothing to read
            }

            if (received != static_cast<int>(ReserveCheckResponseSize)
                || ReadU32(buffer) != ConnectionlessHeader
                || buffer[4] != OpcodeReserveCheckResponse)
            {
                continue;
            }

            uint32_t stage = ReadU32(buffer + 13);
            uint8_t awaiting = buffer[17];
            uint8_t total = buffer[18];

            // stage 1 with awaiting == 0: every roster member has probed, the client shows the Accept popup.
            // Only now do the fakes get around to accepting.
            if (phase == Phase::Probe && stage == 1 && awaiting == 0)
            {
                Platform::Print("[MM-ACCEPT] server reports stage 1 awaiting=0 total=%u -- fakes will accept in %ums\n",
                    total, params.acceptDelayMs);
                phase = Phase::Accept;
                nextAccept = Clock::now() + milliseconds(params.acceptDelayMs);
                acceptedFakes = 0;
            }
        }
    }

    if (sock != InvalidSocketHandle)
    {
        CloseSocket(sock);
    }

#ifdef _WIN32
    if (socketsReady)
    {
        WSACleanup();
    }
#endif
}

// ---- client side ------------------------------------------------------------------------------

namespace
{

std::mutex s_notifyMutex;
ClientNotifyFn s_notifyFn;
void *s_notifyContext;

std::atomic<bool> s_armed{ false };
std::atomic<uint32_t> s_armedIp{ 0 };
std::atomic<uint16_t> s_armedPort{ 0 };
std::atomic<int64_t> s_armedDeadline{ 0 }; // steady clock, ms

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// a stale arm (player cancelled the queue, nobody told us) must not fire on some later match
constexpr int64_t ArmTimeoutMs = 180 * 1000;

#ifdef _WIN32

// wsock32!recvfrom (what engine.dll imports, by ordinal 17) is a wrapper that calls ws2_32!WSARecvFrom
// synchronously (no overlapped, one buffer), so that is the function to watch
int(WSAAPI *Og_WSARecvFrom)(SOCKET s, LPWSABUF buffers, DWORD bufferCount, LPDWORD bytesReceived, LPDWORD flags,
    sockaddr *from, LPINT fromlen, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE completion);

void InspectReserveCheckResponse(const uint8_t *packet, const sockaddr *from, int fromlen)
{
    if (ReadU32(packet) != ConnectionlessHeader || packet[4] != OpcodeReserveCheckResponse)
    {
        return;
    }

    if (!from || fromlen < static_cast<int>(sizeof(sockaddr_in)) || from->sa_family != AF_INET)
    {
        return;
    }

    const sockaddr_in *fromIn = reinterpret_cast<const sockaddr_in *>(from);
    if (ntohl(fromIn->sin_addr.s_addr) != s_armedIp.load() || ntohs(fromIn->sin_port) != s_armedPort.load())
    {
        return;
    }

    if (NowMs() > s_armedDeadline.load())
    {
        s_armed = false;
        return;
    }

    uint32_t stage = ReadU32(packet + 13);
    uint8_t awaiting = packet[17];
    uint8_t total = packet[18];

    Platform::Print("[MM-ACCEPT] 0x25 from reservation server: stage=%u awaiting=%u total=%u\n", stage, awaiting, total);

    if (stage != 2 || awaiting != 0)
    {
        return;
    }

    // only the first "everybody accepted" report counts
    if (!s_armed.exchange(false))
    {
        return;
    }

    Platform::Print("[MM-ACCEPT] reservation fully accepted (stage 2, awaiting 0), notifying ClientGC\n");

    std::lock_guard lock{ s_notifyMutex };
    if (s_notifyFn)
    {
        s_notifyFn(s_notifyContext);
    }
}

int WSAAPI Hk_WSARecvFrom(SOCKET s, LPWSABUF buffers, DWORD bufferCount, LPDWORD bytesReceived, LPDWORD flags,
    sockaddr *from, LPINT fromlen, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE completion)
{
    int result = Og_WSARecvFrom(s, buffers, bufferCount, bytesReceived, flags, from, fromlen, overlapped, completion);

    // cheap filter first: this hook sits on every datagram the engine (and steam) receives
    if (result == 0 && !overlapped && bufferCount == 1 && buffers && bytesReceived
        && *bytesReceived == ReserveCheckResponseSize && s_armed.load(std::memory_order_relaxed))
    {
        InspectReserveCheckResponse(reinterpret_cast<const uint8_t *>(buffers[0].buf), from, fromlen ? *fromlen : 0);
    }

    return result;
}

#endif // _WIN32

} // namespace

void InstallClientRecvHook()
{
#ifdef _WIN32
    static bool s_installed;
    if (s_installed)
    {
        return;
    }

    s_installed = true;

    // engine.dll imports recvfrom from wsock32 by ordinal (17); wsock32's wrapper calls ws2_32!WSARecvFrom
    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (!ws2)
    {
        ws2 = LoadLibraryA("ws2_32.dll");
    }

    void *target = ws2 ? reinterpret_cast<void *>(GetProcAddress(ws2, "WSARecvFrom")) : nullptr;
    if (!target)
    {
        Platform::Print("[MM-ACCEPT] could not find ws2_32!WSARecvFrom, Accept completion trigger disabled\n");
        return;
    }

    funchook_t *funchook = funchook_create();
    void *bridge = target;
    if (!funchook
        || funchook_prepare(funchook, &bridge, reinterpret_cast<void *>(Hk_WSARecvFrom)) != 0
        || funchook_install(funchook, 0) != 0)
    {
        Platform::Print("[MM-ACCEPT] hooking ws2_32!WSARecvFrom failed, Accept completion trigger disabled\n");
        return;
    }

    Og_WSARecvFrom = reinterpret_cast<decltype(Og_WSARecvFrom)>(bridge);
    Platform::Print("[MM-ACCEPT] ws2_32!WSARecvFrom hooked (watching for reservation 0x25 responses)\n");
#else
    Platform::Print("[MM-ACCEPT] client recv hook is Windows-only, Accept completion trigger disabled\n");
#endif
}

void SetClientNotify(ClientNotifyFn fn, void *context)
{
    std::lock_guard lock{ s_notifyMutex };
    s_notifyFn = fn;
    s_notifyContext = context;
}

void ArmClient(uint32_t serverIp, uint16_t serverPort)
{
    s_armedIp = serverIp;
    s_armedPort = serverPort;
    s_armedDeadline = NowMs() + ArmTimeoutMs;
    s_armed = true;
}

void DisarmClient()
{
    s_armed = false;
}

} // namespace AcceptTest
