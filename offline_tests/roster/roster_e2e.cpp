// Offline end-to-end of the backend-driven roster (RESEARCH_FINDINGS.md #55) with the REAL classes of the DLL:
//   real Java backend  <-  BackendClient (client search, roster poll, roster ready)
//   RosterFeed::Poller + RosterFeed::Controller (the srcds decision logic)  ->  AcceptTest::FakeRoster (the fake driver)
//   -> a stand-in for the engine: a UDP responder that implements the reservation check (0x21 -> 0x25) of a Q roster
// and a stand-in for the real player: a UDP socket that sends the client's stage 1 / stage 2 checks.
//
//   roster_e2e.exe <account> <roster size> <wait ms>            the happy path: assignment, stage 1, stage 2
//   roster_e2e.exe <account> <roster size> <wait ms> legacy     the srcds-local roster
//   roster_e2e.exe <account> <roster size> <wait ms> timeout    RESEARCH_FINDINGS.md #63: the player does not accept, the
//        backend cancels the match at its deadline (run it against a backend with backend.accept-timeout=PT6S and
//        backend.server-release-cooldown=PT4S): the client hears about it (the search is SEARCHING / MATCHED again), srcds
//        drops the reservation, the same search gets a NEW match, which is accepted and reported (ReportAccepted).
#include "stdafx.h"
#include "config.h"
#include "keyvalue.h"
#include "backend_client.h"
#include "server_roster.h"
#include "test_accept.h"

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <winsock2.h>
#include <ws2tcpip.h>

using Clock = std::chrono::steady_clock;
static Clock::time_point g_start = Clock::now();
static std::mutex g_print;
static double Now() { return std::chrono::duration<double>(Clock::now() - g_start).count(); }

void Platform::Print(const char *format, ...)
{
    std::lock_guard lock{ g_print };
    char buf[4096];
    va_list ap; va_start(ap, format); vsnprintf(buf, sizeof(buf), format, ap); va_end(ap);
    printf("%7.3f %s", Now(), buf); fflush(stdout);
}
std::string Platform::CommandLine() { return {}; }

namespace AcceptTest
{
bool DiagEnabled() { return false; }
void DiagOnDatagram19(const void *, const void *, int) {}
}

const GCConfig &GetConfig() { static GCConfig c; return c; }
GCConfig::GCConfig()
{
    m_url = "http://127.0.0.1:18090";
    m_key = "test-api";
}

// ------------------------------------------------------------------------------------------------ the engine stand-in
constexpr uint64_t Cookie = 0x293A206F6C6C6548ull;   // GameServerCookieId
constexpr uint16_t EnginePort = 27555;

class Engine
{
public:
    Engine()
    {
        WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
        m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(EnginePort); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (bind(m_sock, (sockaddr *)&a, sizeof(a)) != 0) { printf("engine bind failed\n"); exit(2); }
        m_thread = std::thread{ &Engine::Run, this };
    }
    ~Engine() { m_stop = true; closesocket(m_sock); m_thread.join(); }

    // IVEngineServer::ReserveServerForQueuedGame("Q<cookie>,<match>,<reserve>:[id][id]...")
    void Reserve(const std::string &payload)
    {
        std::lock_guard lock{ m_mutex };
        m_payloads.push_back(payload);
        const size_t colon = payload.find(':');
        const bool reserve = payload.size() > 3 && colon != std::string::npos && payload[colon - 1] == '1';
        if (!reserve) { m_roster.clear(); m_reserved = false; return; }
        if (m_reserved) { return; }   // same cookie: the engine keeps the roster it has (keep-alive)
        m_reserved = true;
        m_roster.clear();
        for (size_t p = colon; (p = payload.find('[', p)) != std::string::npos;)
        {
            const size_t e = payload.find(']', p);
            m_roster[(uint32_t)strtoul(payload.substr(p + 1, e - p - 1).c_str(), nullptr, 16)] = 0;
            p = e;
        }
    }

    size_t RosterSize() { std::lock_guard l{ m_mutex }; return m_roster.size(); }
    bool Reserved() { std::lock_guard l{ m_mutex }; return m_reserved; }
    std::string LastPayload() { std::lock_guard l{ m_mutex }; return m_payloads.empty() ? "" : m_payloads.back(); }
    int Reservations() { std::lock_guard l{ m_mutex }; int n = 0; for (auto &p : m_payloads) { size_t c = p.find(':'); n += (c != std::string::npos && p[c - 1] == '1') ? 1 : 0; } return n; }

private:
    struct Peer { sockaddr_in addr; uint32_t token; uint32_t stage; };

    static void Put32(uint8_t *b, size_t &o, uint32_t v) { memcpy(b + o, &v, 4); o += 4; }

    void Answer(const sockaddr_in &to, uint32_t token, uint32_t stage)
    {
        uint32_t awaiting = 127, total = 0;
        if (m_reserved)
        {
            awaiting = 0; total = (uint32_t)m_roster.size();
            for (auto &kv : m_roster) awaiting += kv.second < stage ? 1 : 0;
        }
        uint8_t pkt[19]; size_t o = 0;
        Put32(pkt, o, 0xFFFFFFFFu); pkt[o++] = 0x25; Put32(pkt, o, 13805); Put32(pkt, o, token); Put32(pkt, o, stage);
        pkt[o++] = (uint8_t)awaiting; pkt[o++] = (uint8_t)total;
        sendto(m_sock, (const char *)pkt, (int)o, 0, (const sockaddr *)&to, sizeof(to));
    }

    void Run()
    {
        while (!m_stop)
        {
            uint8_t buf[128]; sockaddr_in from{}; int fl = sizeof(from);
            int n = recvfrom(m_sock, (char *)buf, sizeof(buf), 0, (sockaddr *)&from, &fl);
            if (n != 33 || buf[4] != 0x21) continue;
            std::lock_guard lock{ m_mutex };
            uint32_t token, stage; uint64_t cookie, steam;
            memcpy(&token, buf + 9, 4); memcpy(&stage, buf + 13, 4); memcpy(&cookie, buf + 17, 8); memcpy(&steam, buf + 25, 8);
            const uint32_t account = (uint32_t)(steam & 0xFFFFFFFFu);
            auto it = m_roster.find(account);
            if (!m_reserved || cookie != Cookie || it == m_roster.end()) { Answer(from, token, stage); continue; }
            const bool changed = it->second < stage;
            it->second = (std::max)(it->second, stage);
            Answer(from, token, stage);
            m_peers[account] = { from, token, stage };
            if (changed)
            {
                // the server broadcasts the new state to everybody that has asked before
                for (auto &kv : m_peers)
                {
                    if (kv.first != account) Answer(kv.second.addr, kv.second.token, kv.second.stage);
                }
            }
        }
    }

    SOCKET m_sock{};
    std::thread m_thread;
    std::atomic<bool> m_stop{ false };
    std::mutex m_mutex;
    bool m_reserved{};
    std::map<uint32_t, uint32_t> m_roster;   // account -> highest stage
    std::map<uint32_t, Peer> m_peers;
    std::vector<std::string> m_payloads;
};

// ------------------------------------------------------------------------------------------------ the real player stand-in
struct PlayerCheck { bool got{}; uint32_t stage{}, awaiting{}, total{}; };

static PlayerCheck Check(uint32_t account, uint32_t stage)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); bind(s, (sockaddr *)&a, sizeof(a));
    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(EnginePort); inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    uint8_t pkt[33]; size_t o = 0;
    auto p32 = [&](uint32_t v) { memcpy(pkt + o, &v, 4); o += 4; };
    auto p64 = [&](uint64_t v) { memcpy(pkt + o, &v, 8); o += 8; };
    p32(0xFFFFFFFFu); pkt[o++] = 0x21; p32(13805); p32(account); p32(stage); p64(Cookie);
    p64((1ull << 56) | (1ull << 52) | (1ull << 32) | account);
    sendto(s, (const char *)pkt, (int)o, 0, (sockaddr *)&to, sizeof(to));
    DWORD timeout = 500; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    uint8_t buf[64]; PlayerCheck r;
    int n = recv(s, (char *)buf, sizeof(buf), 0);
    if (n == 19 && buf[4] == 0x25) { r.got = true; memcpy(&r.stage, buf + 13, 4); r.awaiting = buf[17]; r.total = buf[18]; }
    closesocket(s);
    return r;
}

// ------------------------------------------------------------------------------------------------ the srcds glue (what ServerGC does)
struct Srcds
{
    std::mutex mutex;                // the real code serializes on the GC thread
    Engine &engine;
    std::unique_ptr<AcceptTest::FakeRoster> roster;
    std::unique_ptr<RosterFeed::Controller> controller;
    std::unique_ptr<RosterFeed::Poller> poller;
    bool playerOnServer{};

    explicit Srcds(Engine &e, const char *mode, uint32_t required) : engine{ e }
    {
        RosterFeed::Controller::Host host;
        host.arm = [this](const std::vector<uint32_t> &participants, const std::string &source, bool unreserveFirst)
        {
            AcceptTest::FakeRoster::Params p;
            p.cookie = Cookie;
            for (uint32_t id : participants) { if (!AcceptTest::IsFakeAccountId(id)) { p.realAccountId = id; break; } }
            p.rosterSize = (uint32_t)participants.size();
            p.modeName = "competitive";
            p.serverAddress = "127.0.0.1";
            p.serverPort = EnginePort;
            p.acceptDelayMs = 800;
            p.participants = participants;
            p.source = source;
            p.unreserveFirst = unreserveFirst;
            p.onFakesReady = [this](bool ready) { std::lock_guard l{ mutex }; controller->OnFakesReady(ready); };
            roster.reset();
            roster = std::make_unique<AcceptTest::FakeRoster>(p, [this](const std::string &payload) { engine.Reserve(payload); });
        };
        host.confirm = [this](const std::string &matchId) { poller->Confirm(matchId); };
        host.release = [this](const std::string &) { if (roster) { roster->Release(); } };   // ServerGC::ReleaseRoster
        host.playerOnServer = [this] { return playerOnServer; };
        controller = std::make_unique<RosterFeed::Controller>(mode, required, std::move(host));
        poller = std::make_unique<RosterFeed::Poller>("127.0.0.1", EnginePort, [this](const RosterFeed::Snapshot &s)
        {
            std::lock_guard l{ mutex }; controller->OnSnapshot(s);
        });
    }
    ~Srcds() { poller.reset(); roster.reset(); }
};

// ------------------------------------------------------------------------------------------------ the client search
static std::mutex g_resultMutex;
static std::vector<BackendClient::SearchResult> g_results;
static void OnResult(void *, const BackendClient::SearchResult &r)
{
    printf("%7.3f   [client] search status=%s players=%u/%u assigned=%d %s:%u match=%s\n", Now(), r.status.c_str(), r.matchPlayers,
        r.matchRequired, r.IsAssigned() ? 1 : 0, r.assignment.serverAddress.c_str(), r.assignment.serverPort, r.assignment.matchId.c_str());
    fflush(stdout);
    std::lock_guard l{ g_resultMutex };
    g_results.push_back(r);
}
static double AssignedAt()
{
    std::lock_guard l{ g_resultMutex };
    return 0;
}
// the next assignment (WAITING_ACCEPT / READY_TO_CONNECT) whose match is not `notMatch`
static bool WaitAssignedOtherThan(const std::string &notMatch, int ms)
{
    auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end)
    {
        { std::lock_guard l{ g_resultMutex }; for (auto &r : g_results) if (r.IsAssigned() && r.assignment.matchId != notMatch) return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// the search is SEARCHING / MATCHED again after `assignedMatch` was assigned: the backend took the match back
static bool WaitWithdrawn(const std::string &assignedMatch, int ms)
{
    auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end)
    {
        {
            std::lock_guard l{ g_resultMutex };
            bool seenAssigned = false;
            for (auto &r : g_results)
            {
                if (r.IsAssigned() && r.assignment.matchId == assignedMatch) seenAssigned = true;
                else if (seenAssigned && (r.status == "SEARCHING" || r.status == "MATCHED")) return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

static std::string LastAssignedMatch()
{
    std::lock_guard l{ g_resultMutex };
    for (auto it = g_results.rbegin(); it != g_results.rend(); ++it) if (it->IsAssigned()) return it->assignment.matchId;
    return {};
}

static bool WaitAssigned(int ms)
{
    auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end)
    {
        { std::lock_guard l{ g_resultMutex }; for (auto &r : g_results) if (r.IsAssigned()) return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

int main(int argc, char **argv)
{
    const uint32_t real = argc > 1 ? (uint32_t)atoll(argv[1]) : 1050166997u;
    const int required = argc > 2 ? atoi(argv[2]) : 10;
    const int waitMs = argc > 3 ? atoi(argv[3]) : 15000;

    WSADATA d; WSAStartup(MAKEWORD(2, 2), &d);
    Engine engine;

    if (argc > 4 && std::string(argv[4]) == "legacy")
    {
        // the OLD path, unchanged in behavior: no participants list, the roster is [real] + fake 1..N-1
        printf("%7.3f [harness] LEGACY fake roster of %d (real %08x learned from its first 0x21)\n", Now(), required, real);
        std::atomic<bool> ready{ false };
        AcceptTest::FakeRoster::Params p;
        p.cookie = Cookie; p.realAccountId = real; p.rosterSize = (uint32_t)required; p.modeName = "wingman";
        p.serverAddress = "127.0.0.1"; p.serverPort = EnginePort; p.acceptDelayMs = 500;
        p.onFakesReady = [&](bool r) { ready = r; };
        AcceptTest::FakeRoster roster(p, [&](const std::string &payload) { engine.Reserve(payload); });
        for (int i = 0; i < 100 && !ready; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        printf("%7.3f [harness] fakes ready=%d payload=%s\n", Now(), ready.load() ? 1 : 0, engine.LastPayload().c_str());
        PlayerCheck first = Check(real, 1);
        printf("%7.3f [client] stage 1 -> awaiting=%u total=%u\n", Now(), first.awaiting, first.total);
        PlayerCheck accept;
        for (int i = 0; i < 40; i++) { accept = Check(real, 2); if (accept.got && accept.awaiting == 0) break; std::this_thread::sleep_for(std::chrono::milliseconds(250)); }
        printf("%7.3f [client] stage 2 -> awaiting=%u total=%u\n", Now(), accept.awaiting, accept.total);
        return ready && first.awaiting == 0 && first.total == (uint32_t)required && accept.awaiting == 0 ? 0 : 3;
    }

    Srcds srcds(engine, "competitive", (uint32_t)required);
    BackendClient::SetResultHandler(OnResult, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // srcds polls for a while: the backend learns it reads rosters

    const bool timeoutScenario = argc > 4 && std::string(argv[4]) == "timeout";

    BackendClient::SearchInfo info;
    info.accountId = real; info.gameType = 520; info.mode = "competitive"; info.gameMode = "competitive"; info.maps = { "de_dust2" };
    printf("%7.3f [client] starts a Competitive search on de_dust2\n", Now());
    BackendClient::SearchStarted(info, 76561197960265728ull + real);

    const bool assigned = WaitAssigned(waitMs);
    const double assignedAt = Now();
    printf("%7.3f [harness] %s\n", assignedAt, assigned ? "the client GOT the assignment" : "NO assignment in time");
    if (!assigned) return 1;

    if (timeoutScenario)
    {
        bool ok = true;
        auto verdict = [&](bool cond, const char *what) { printf("%7.3f [check] %-4s %s\n", Now(), cond ? "OK" : "FAIL", what); ok = ok && cond; };

        const std::string firstMatch = LastAssignedMatch();
        PlayerCheck popup = Check(real, 1);
        verdict(popup.got && popup.awaiting == 0, "the popup is up (stage 1, awaiting=0) on the first attempt");
        verdict(engine.Reserved(), "the srcds reservation is armed");

        printf("%7.3f [client] does NOT accept: waiting for the backend deadline\n", Now());
        verdict(WaitWithdrawn(firstMatch, 15000), "the client hears that the backend took the match back (its search is SEARCHING / MATCHED again)");
        const double withdrawnAt = Now();

        // srcds sees no match on its server for the cooldown and drops the reservation
        bool dropped = false;
        for (int i = 0; i < 100 && !dropped; i++) { dropped = !engine.Reserved(); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        verdict(dropped, "srcds released the reservation (the engine has no cookie / roster any more)");
        PlayerCheck late = Check(real, 2);
        verdict(late.got && late.awaiting == 127, "a late Accept of the old match finds no reservation (awaiting=127)");
        printf("%7.3f [harness] reservation dropped %.1f s after the client heard of the withdrawal\n", Now(), Now() - withdrawnAt);

        // the same search gets a new match once the server is free again, srcds arms it afresh, this time the player accepts
        verdict(WaitAssignedOtherThan(firstMatch, waitMs), "the same search is assigned a NEW match after the cooldown");
        const std::string secondMatch = LastAssignedMatch();
        verdict(secondMatch != firstMatch && !secondMatch.empty(), "with another match id");
        PlayerCheck again = Check(real, 1);
        verdict(again.got && again.awaiting == 0 && again.total == (uint32_t)required, "the new match: popup up on the first attempt (stage 1, awaiting=0)");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        PlayerCheck accept2;
        for (int i = 0; i < 40; i++) { accept2 = Check(real, 2); if (accept2.got && accept2.awaiting == 0) break; std::this_thread::sleep_for(std::chrono::milliseconds(250)); }
        verdict(accept2.got && accept2.awaiting == 0, "everybody accepted (stage 2, awaiting=0)");

        printf("%7.3f [client] reports the Accept to the backend (BackendClient::ReportAccepted)\n", Now());
        BackendClient::ReportAccepted();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        BackendClient::SearchCancelled();                                    // the client's stop when it connects
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        printf("[engine] reservations=%d roster=%zu\n", engine.Reservations(), engine.RosterSize());
        printf("RESULT: %s\n", ok ? "OK" : "FAILED");
        return ok ? 0 : 3;
    }

    // the client now sends its first reservation check (stage 1): with the roster armed this must already be awaiting=0
    PlayerCheck first = Check(real, 1);
    printf("%7.3f [client] 0x21 stage 1 -> 0x25 got=%d stage=%u awaiting=%u total=%u   <== %s\n", Now(), first.got, first.stage,
        first.awaiting, first.total, first.got && first.awaiting == 0 ? "FIRST ATTEMPT OK (popup would show)" : "NOT ready (awaiting != 0)");

    // Accept -> stage 2, the fakes accept after their delay, the last check must be awaiting=0 too
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    PlayerCheck accept;
    for (int i = 0; i < 40; i++)
    {
        accept = Check(real, 2);
        if (accept.got && accept.awaiting == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    printf("%7.3f [client] 0x21 stage 2 -> awaiting=%u total=%u   <== %s\n", Now(), accept.awaiting, accept.total,
        accept.got && accept.awaiting == 0 ? "EVERYBODY ACCEPTED (second 9107 would follow)" : "not accepted");

    printf("[engine] reservations=%d roster=%zu payload=%s\n", engine.Reservations(), engine.RosterSize(), engine.LastPayload().c_str());
    BackendClient::SearchCancelled();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    return first.got && first.awaiting == 0 && accept.got && accept.awaiting == 0 ? 0 : 3;
}
