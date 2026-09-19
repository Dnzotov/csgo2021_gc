#include "stdafx.h"
#include "test_accept.h"
#include "test_diag.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
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

// awaiting value the engine answers when the account is not part of the reservation (or the cookie does not match)
constexpr uint8_t AwaitingUnknownAccount = 0x7F;

// CBaseServer::GetHostVersion() == the client's protocol constant == ClientVersion of steam.inf
// (1.38.0.5 -> 13805). RESEARCH_FINDINGS.md #24. The engine silently drops 0x21 with any other value.
constexpr uint32_t HostVersion = 13805;

uint32_t ReadU32(const uint8_t *data)
{
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return value;
}

uint64_t ReadU64(const uint8_t *data)
{
    uint64_t value;
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

std::string BuildQueuedReservationPayload(uint64_t cookie, const std::vector<uint32_t> &participants)
{
    // format confirmed against CBaseServer::ReserveServerForQueuedGame / SetReservationCookie
    // (baseserver.cpp, RESEARCH_FINDINGS.md #29/#41): "%llx,%llx,%d:" = cookie, match id, bReserve
    // followed by one "[%x]" AccountID token per roster slot, in roster order.
    char header[64];
    snprintf(header, sizeof(header), "Q%llx,%llx,1:",
        static_cast<unsigned long long>(cookie), static_cast<unsigned long long>(cookie));

    std::string payload = header;

    char token[16];
    for (uint32_t accountId : participants)
    {
        snprintf(token, sizeof(token), "[%x]", accountId);
        payload += token;
    }

    return payload;
}

std::string BuildQueuedReservationPayload(uint64_t cookie, uint32_t realAccountId, uint32_t rosterSize)
{
    std::vector<uint32_t> participants{ realAccountId };
    for (uint32_t i = 1; i < rosterSize; i++)
    {
        participants.push_back(FakeAccountId(i));
    }

    return BuildQueuedReservationPayload(cookie, participants);
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
    Cooldown,  // unreserved, waiting for the engine to actually clear the cookie (and with it the roster)
    Probe,     // every fake sends ONE stage 1 request (paced), retried only after a timeout
    WaitPopup, // all fakes are at stage 1, waiting for the server to report stage 1 / awaiting 0 (popup is up)
    Accept,    // every fake sends ONE stage 2 request (paced), retried only after a timeout
    Done,
};

const char *PhaseName(Phase phase)
{
    switch (phase)
    {
    case Phase::Paused: return "paused";
    case Phase::Cooldown: return "cooldown";
    case Phase::Probe: return "probe";
    case Phase::WaitPopup: return "wait-popup";
    case Phase::Accept: return "accept";
    case Phase::Done: return "done";
    }

    return "?";
}

void FakeLog(const char *format, ...)
{
    char text[384];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    Platform::Print("[FAKE-MM] %s\n", text);
}

} // namespace

struct FakeRoster::Impl
{
    Params params;
    PostReserveFn postReserve;
    std::vector<uint32_t> fakeIds; // the fake participants this driver answers for, in roster order

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

    Params &p = m_impl->params;
    if (p.participants.empty())
    {
        // the legacy roster: the real player learned from its first 0x21 + deterministic fake participants
        p.participants.push_back(p.realAccountId);
        for (uint32_t i = 1; i < p.rosterSize; i++)
        {
            p.participants.push_back(FakeAccountId(i));
        }
    }

    p.rosterSize = static_cast<uint32_t>(p.participants.size());
    if (p.source.empty())
    {
        p.source = "legacy";
    }

    for (uint32_t accountId : p.participants)
    {
        if (IsFakeAccountId(accountId))
        {
            m_impl->fakeIds.push_back(accountId);
        }
    }

    // a reservation with another roster is active: unreserve it first, the engine keeps a roster per cookie
    m_impl->command = p.unreserveFirst ? CommandRearm : CommandArm;
    m_impl->thread = std::thread{ &Impl::Run, m_impl };

    FakeLog("started: mode=%s roster=%u (%u real + %zu fake), source=%s, server=%s:%u, accept delay=%ums",
        p.modeName.c_str(), p.rosterSize, p.rosterSize - static_cast<uint32_t>(m_impl->fakeIds.size()), m_impl->fakeIds.size(),
        p.source.c_str(), p.serverAddress.c_str(), p.serverPort, p.acceptDelayMs);
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

    // Rate: the engine drops ALL connectionless packets of an IP once it sends more than sv_max_queries_sec (10)
    // per second on average over sv_max_queries_window (30 s), i.e. > 300 packets / 30 s (retail engine.dll
    // defaults, RESEARCH_FINDINGS.md #44.4). The real client and the fakes share one IP, so the fakes send each
    // request ONCE (2 x roster packets per match), paced, and only retry what the server did not answer.
    constexpr auto PacketGap = milliseconds(150);       // <= 6.7 pps while a burst is running
    constexpr auto RetryTimeout = milliseconds(1500);   // per fake, only if no matching 0x25 came back
    constexpr int MaxAttempts = 5;                      // per fake and stage, then it is reported as TIMEOUT
    constexpr auto FirstProbeDelay = milliseconds(1500);// give the main thread time to run the reservation
    constexpr auto KeepAliveInterval = seconds(8);      // sv_mmqueue_reservation_timeout is 21 s
    constexpr auto DoneRearmDelay = seconds(90);        // nobody connected: start over with a clean roster
    constexpr auto GaveUpRearmDelay = seconds(30);      // a fake never got confirmed: start over
    // Unreserve() only zeroes the expiry time; the cookie (and the roster with its stale stages) is dropped later by
    // CGameServer::UpdateHibernationState once nobody is connected for sv_hibernate_postgame_delay (5 s by default).
    // Reserving the same cookie before that would keep the old roster, so wait it out.
    constexpr auto UnreserveCooldown = seconds(12);

    const uint32_t fakeCount = static_cast<uint32_t>(fakeIds.size());

    // the fake with 1-based index i is fakeIds[i - 1] (with the legacy roster that is FakeAccountId(i))
    auto fakeId = [&](uint32_t index) { return fakeIds[index - 1]; };
    auto notifyReady = [&](bool ready)
    {
        if (params.onFakesReady)
        {
            params.onFakesReady(ready);
        }
    };

    const std::string reservePayload = BuildQueuedReservationPayload(params.cookie, params.participants);
    const std::string unreservePayload = BuildUnreservePayload(params.cookie);

    uint32_t payloadEntries = 0;
    for (char c : reservePayload)
    {
        if (c == '[')
        {
            payloadEntries++;
        }
    }

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
            FakeLog("bad server address '%s', fake players disabled", params.serverAddress.c_str());
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
                FakeLog("could not create a UDP socket, fake players disabled");
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
        WriteU32(packet, offset, fakeId(fakeIndex)); // token, echoed back by the server: identifies the fake
        WriteU32(packet, offset, stage);
        WriteU64(packet, offset, params.cookie);
        WriteU64(packet, offset, SteamIdFromAccountId(fakeId(fakeIndex)));

        sendto(sock, reinterpret_cast<const char *>(packet), static_cast<int>(offset), 0,
            reinterpret_cast<const sockaddr *>(&serverAddr), sizeof(serverAddr));
    };

    struct Fake
    {
        uint32_t confirmed{};   // highest stage the server answered for
        int attempts{};         // requests sent for the stage that is currently being driven
        Clock::time_point lastSent{};
        bool gaveUp{};
    };

    std::vector<Fake> fakes(fakeCount + 1); // 1-based, like the account ids

    Phase phase = Phase::Paused;
    Clock::time_point lastReserve{}, probeStart{}, acceptAt{}, doneAt{}, cooldownUntil{}, nextSend{}, gaveUpAt{};
    bool popupSeen = false;
    bool totalChecked = false;
    bool finalLogged = false;
    uint32_t cursor = 1;

    auto fakeName = [&](uint32_t index, char *out, size_t size)
    {
        snprintf(out, size, "fake-%02u(acct=%08x)", index, fakeId(index));
    };

    auto resetFakeAttempts = [&]()
    {
        for (Fake &fake : fakes)
        {
            fake.attempts = 0;
            fake.gaveUp = false;
        }
    };

    auto armReservation = [&](Clock::time_point now)
    {
        Platform::Print("[MM-ACCEPT] arming Q reservation: %s\n", reservePayload.c_str());
        FakeLog("roster expected=%u actual=%u%s", params.rosterSize, payloadEntries,
            payloadEntries == params.rosterSize ? "" : "   <== MISMATCH (payload does not match the mode's required players)");

        notifyReady(false);
        postReserve(reservePayload);

        phase = Phase::Probe;
        lastReserve = now;
        probeStart = now + FirstProbeDelay;
        for (Fake &fake : fakes)
        {
            fake = Fake{};
        }

        popupSeen = false;
        totalChecked = false;
        finalLogged = false;
        cursor = 1;
    };

    while (!stopping)
    {
        Clock::time_point now = Clock::now();

        int cmd = command.exchange(CommandNone);
        if (phase == Phase::Done && now - doneAt >= DoneRearmDelay)
        {
            FakeLog("nobody connected for %lld s after the fake accept, re-arming a fresh roster",
                static_cast<long long>(DoneRearmDelay.count()));
            cmd = CommandRearm;
        }

        if ((phase == Phase::Probe || phase == Phase::Accept) && gaveUpAt != Clock::time_point{}
            && now - gaveUpAt >= GaveUpRearmDelay)
        {
            FakeLog("a fake player was never confirmed by the server (see TIMEOUT above), re-arming a fresh roster");
            gaveUpAt = {};
            cmd = CommandRearm;
        }

        if (cmd == CommandPause)
        {
            FakeLog("a client connected, roster left alone until it disconnects");
            phase = Phase::Paused;
        }
        else if (cmd == CommandArm)
        {
            armReservation(now);
        }
        else if (cmd == CommandRearm)
        {
            FakeLog("unreserving, fresh roster in %lld s", static_cast<long long>(UnreserveCooldown.count()));
            notifyReady(false);
            postReserve(unreservePayload);

            phase = Phase::Cooldown;
            cooldownUntil = now + UnreserveCooldown;
            gaveUpAt = {};
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

        // ---- phase transitions -----------------------------------------------------------------------------
        auto allConfirmed = [&](uint32_t stage)
        {
            for (uint32_t i = 1; i <= fakeCount; i++)
            {
                if (fakes[i].confirmed < stage)
                {
                    return false;
                }
            }

            return true;
        };

        if (popupSeen && (phase == Phase::Probe || phase == Phase::WaitPopup))
        {
            // stage 1 / awaiting 0 means every roster entry (fakes included) is at stage >= 1 on the server
            for (uint32_t i = 1; i <= fakeCount; i++)
            {
                if (fakes[i].confirmed < 1)
                {
                    fakes[i].confirmed = 1;
                }
            }

            phase = Phase::Accept;
            acceptAt = now + milliseconds(params.acceptDelayMs);
            resetFakeAttempts();
            notifyReady(true);
            FakeLog("server reports stage 1 awaiting=0 (accept popup is up), fakes accept in %ums", params.acceptDelayMs);
        }
        else if (phase == Phase::Probe && now >= probeStart && allConfirmed(1))
        {
            phase = Phase::WaitPopup;
            notifyReady(true);
            FakeLog("all %u fake players confirmed at stage 1, waiting for the real player to reach stage 1", fakeCount);
        }
        else if (phase == Phase::Accept && now >= acceptAt && allConfirmed(2))
        {
            phase = Phase::Done;
            doneAt = now;
            FakeLog("all %u fake players confirmed at stage 2, waiting for the real player's accept", fakeCount);
        }

        // ---- paced sending (one packet per PacketGap) --------------------------------------------------------
        uint32_t desiredStage = 0;
        if (phase == Phase::Probe && now >= probeStart)
        {
            desiredStage = 1;
        }
        else if (phase == Phase::Accept && now >= acceptAt)
        {
            desiredStage = 2;
        }

        if (desiredStage && now >= nextSend)
        {
            for (uint32_t k = 0; k < fakeCount; k++)
            {
                uint32_t i = ((cursor - 1 + k) % fakeCount) + 1;
                Fake &fake = fakes[i];

                if (fake.confirmed >= desiredStage || fake.gaveUp)
                {
                    continue;
                }

                if (fake.attempts > 0 && now - fake.lastSent < RetryTimeout)
                {
                    continue;
                }

                char name[40];
                fakeName(i, name, sizeof(name));

                if (fake.attempts >= MaxAttempts)
                {
                    fake.gaveUp = true;
                    gaveUpAt = now;
                    FakeLog("player=%s stage=%u TIMEOUT after %d attempts, the server never confirmed it", name, desiredStage, MaxAttempts);
                    continue;
                }

                sendReserveCheck(i, desiredStage);
                fake.attempts++;
                fake.lastSent = now;
                FakeLog("player=%s stage=%u sent%s", name, desiredStage, fake.attempts > 1 ? " (retry)" : "");

                cursor = (i % fakeCount) + 1;
                nextSend = now + PacketGap;
                break;
            }
        }

        if (sock == InvalidSocketHandle)
        {
            std::this_thread::sleep_for(milliseconds(20));
            continue;
        }

        // ---- server answers / broadcasts -----------------------------------------------------------------------
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

            uint32_t token = ReadU32(buffer + 9);
            uint32_t stage = ReadU32(buffer + 13);
            uint8_t awaiting = buffer[17];
            uint8_t total = buffer[18];

            uint32_t index = 0;
            for (uint32_t i = 1; i <= fakeCount; i++)
            {
                if (fakeId(i) == token)
                {
                    index = i;
                    break;
                }
            }

            if (index == 0)
            {
                continue;
            }

            char name[40];
            fakeName(index, name, sizeof(name));
            Fake &fake = fakes[index];

            if (awaiting == AwaitingUnknownAccount)
            {
                // reservation not (yet) active, or this account is not in it -- not a confirmation
                FakeLog("player=%s stage=%u answered awaiting=127 (server does not know this account / reservation)", name, stage);
                continue;
            }

            if (!totalChecked)
            {
                totalChecked = true;
                if (total == params.rosterSize)
                {
                    FakeLog("server roster total=%u expected=%u OK", total, params.rosterSize);
                }
                else
                {
                    FakeLog("server roster total=%u expected=%u   <== MISMATCH: the srcds roster does not match the mode "
                        "(is this srcds started with the right mode / was it restarted after a config change?)",
                        total, params.rosterSize);
                }
            }

            if ((stage == 1 || stage == 2) && fake.confirmed < stage)
            {
                fake.confirmed = stage;
                FakeLog("player=%s stage=%u confirmed (awaiting=%u total=%u)", name, stage, awaiting, total);
            }

            if (stage == 1 && awaiting == 0 && (phase == Phase::Probe || phase == Phase::WaitPopup))
            {
                popupSeen = true;
            }

            if (stage == 2 && awaiting == 0 && !finalLogged)
            {
                finalLogged = true;
                FakeLog("awaiting=0 total=%u (everybody accepted, phase=%s)", total, PhaseName(phase));
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

// ---- shared hook state ----------------------------------------------------------------------------

namespace
{

std::mutex s_notifyMutex;
ClientNotifyFn s_notifyFn;
void *s_notifyContext;

std::atomic<bool> s_armed{ false };
std::atomic<uint32_t> s_armedIp{ 0 };
std::atomic<uint16_t> s_armedPort{ 0 };
std::atomic<uint32_t> s_armedExpectedPlayers{ 0 };
std::atomic<bool> s_armedTotalChecked{ false };
std::atomic<int64_t> s_armedDeadline{ 0 }; // steady clock, ms

std::mutex s_sniffMutex;
ServerSniffFn s_sniffFn;
void *s_sniffContext;
std::atomic<uint64_t> s_sniffCookie{ 0 };
std::atomic<bool> s_sniffActive{ false };
std::atomic<uint32_t> s_sniffLastAccount{ 0 };

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

    // the roster size the server reports has to match what the mode requires -- never hide a mismatch
    if (awaiting != AwaitingUnknownAccount && !s_armedTotalChecked.exchange(true))
    {
        uint32_t expected = s_armedExpectedPlayers.load();
        if (expected && total != expected)
        {
            Platform::Print("[MM-ACCEPT] server roster total=%u but the mode requires %u players   <== MISMATCH "
                "(that srcds was started for another mode / before a config change)\n", total, expected);
        }
        else if (expected)
        {
            Platform::Print("[MM-ACCEPT] server roster total=%u matches the mode's required players\n", total);
        }
    }

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

// srcds: an incoming 0x21. The first one from a non-fake account with our cookie tells us who the real player is
// (its SteamID is what the client engine got from ISteamUser::GetSteamID()).
void InspectReserveCheckRequest(const uint8_t *packet)
{
    if (ReadU32(packet) != ConnectionlessHeader || packet[4] != OpcodeReserveCheck)
    {
        return;
    }

    uint32_t stage = ReadU32(packet + 13);
    uint64_t cookie = ReadU64(packet + 17);
    uint64_t steamId = ReadU64(packet + 25);
    uint32_t accountId = static_cast<uint32_t>(steamId & 0xffffffffu);

    if (!stage || !accountId || IsFakeAccountId(accountId) || cookie != s_sniffCookie.load())
    {
        return;
    }

    if (s_sniffLastAccount.exchange(accountId) == accountId)
    {
        return;
    }

    std::lock_guard lock{ s_sniffMutex };
    if (s_sniffFn)
    {
        s_sniffFn(s_sniffContext, accountId);
    }
}

int WSAAPI Hk_WSARecvFrom(SOCKET s, LPWSABUF buffers, DWORD bufferCount, LPDWORD bytesReceived, LPDWORD flags,
    sockaddr *from, LPINT fromlen, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE completion)
{
    int result = Og_WSARecvFrom(s, buffers, bufferCount, bytesReceived, flags, from, fromlen, overlapped, completion);

    // cheap filter first: this hook sits on every datagram the engine (and steam) receives
    if (result == 0 && !overlapped && bufferCount == 1 && buffers && bytesReceived)
    {
        if (*bytesReceived == ReserveCheckResponseSize)
        {
            if (DiagEnabled())
            {
                DiagOnDatagram19(buffers[0].buf, from, fromlen ? *fromlen : 0);
            }

            if (s_armed.load(std::memory_order_relaxed))
            {
                InspectReserveCheckResponse(reinterpret_cast<const uint8_t *>(buffers[0].buf), from, fromlen ? *fromlen : 0);
            }
        }
        else if (*bytesReceived == ReserveCheckSize && s_sniffActive.load(std::memory_order_relaxed))
        {
            InspectReserveCheckRequest(reinterpret_cast<const uint8_t *>(buffers[0].buf));
        }
    }

    return result;
}

#endif // _WIN32

} // namespace

void InstallRecvHook()
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
        Platform::Print("[MM-ACCEPT] could not find ws2_32!WSARecvFrom, Accept test hooks disabled\n");
        return;
    }

    funchook_t *funchook = funchook_create();
    void *bridge = target;
    if (!funchook
        || funchook_prepare(funchook, &bridge, reinterpret_cast<void *>(Hk_WSARecvFrom)) != 0
        || funchook_install(funchook, 0) != 0)
    {
        Platform::Print("[MM-ACCEPT] hooking ws2_32!WSARecvFrom failed, Accept test hooks disabled\n");
        return;
    }

    Og_WSARecvFrom = reinterpret_cast<decltype(Og_WSARecvFrom)>(bridge);
    Platform::Print("[MM-ACCEPT] ws2_32!WSARecvFrom hooked (0x25 watcher on the client, 0x21 sniffer on srcds)\n");
#else
    Platform::Print("[MM-ACCEPT] recv hook is Windows-only, Accept test hooks disabled\n");
#endif
}

void SetServerSniff(ServerSniffFn fn, void *context, uint64_t cookie)
{
    std::lock_guard lock{ s_sniffMutex };
    s_sniffFn = fn;
    s_sniffContext = context;
    s_sniffCookie = cookie;
    s_sniffLastAccount = 0;
    s_sniffActive = fn != nullptr;
}

void SetClientNotify(ClientNotifyFn fn, void *context)
{
    std::lock_guard lock{ s_notifyMutex };
    s_notifyFn = fn;
    s_notifyContext = context;
}

void ArmClient(uint32_t serverIp, uint16_t serverPort, uint32_t expectedPlayers)
{
    s_armedIp = serverIp;
    s_armedPort = serverPort;
    s_armedExpectedPlayers = expectedPlayers;
    s_armedTotalChecked = false;
    s_armedDeadline = NowMs() + ArmTimeoutMs;
    s_armed = true;
}

void DisarmClient()
{
    s_armed = false;
}

} // namespace AcceptTest
