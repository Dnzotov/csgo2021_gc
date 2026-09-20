// Offline end-to-end of the backend-driven roster (RESEARCH_FINDINGS.md #55) with the REAL classes of the DLL:
//   real Java backend  <-  BackendClient (client search, roster poll, roster ready)
//   RosterFeed::Poller + RosterFeed::Controller (the srcds decision logic)  ->  AcceptTest::FakeRoster (the fake driver)
//   -> a stand-in for the engine: a UDP responder that implements the reservation check (0x21 -> 0x25) of a Q roster
// and a stand-in for the real player: a UDP socket that sends the client's stage 1 / stage 2 checks.
//
//   roster_e2e.exe <account> <roster size> <wait ms>            the happy path: assignment, stage 1, stage 2
//   roster_e2e.exe <account> <roster size> <wait ms> legacy     the srcds-local roster
//   roster_e2e.exe <account> <roster size> <wait ms> party      RESEARCH_FINDINGS.md #65: this process is the GC of a party MEMBER
//        (BackendClient::WatchAccount): the lobby leader's search (sent with curl, account <account>, party [<account>+1]) is found
//        for it, both real players are in the srcds roster, one player accepting early must NOT connect anybody, the connect
//        (READY_TO_CONNECT) comes only after both reported. Needs a backend with a fake search of <roster size> - 2 players.
//   roster_e2e.exe <account> <roster size> <wait ms> gather     RESEARCH_FINDINGS.md #66: two real players who search one after the
//        other (A with curl, at once; this process is the GC of B, 1.5 s later, its own search) must end up in ONE match with the
//        fake players filling only the 8 that are missing (gather window: run the backend with backend.fake-players.gather-window=PT3S
//        and a fake search of <roster size> - 1 players). Both are in the srcds roster, each gets its own Match Found, the one who
//        accepts first is not sent on, the connect (READY_TO_CONNECT) comes when both reported.
//   roster_e2e.exe <account> <roster size> <wait ms> three      RESEARCH_FINDINGS.md #67: THREE real players (A and C search with curl,
//        this process is the GC of B) and the Fake Players profile of the backend (set in its admin API; the profile count is the
//        number of virtual players, NOT derived from the capacity: 7 -> a roster of 10, 2 -> a roster of 5, 0 -> a roster of 3;
//        <roster size> is that total). One match, every real player is in the srcds roster, the popup needs all three at stage 1,
//        one and two Accepts connect nobody, the third one does (READY_TO_CONNECT). Backend: --backend.fake-players.gather-window=PT3S
//        and --backend.accept-timeout=PT30S.
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
    printf("%7.3f   [client] search %s status=%s players=%u/%u accepted=%u/%u assigned=%d %s:%u match=%s%s\n", Now(), r.requestId.c_str(), r.status.c_str(),
        r.matchPlayers, r.matchRequired, r.matchAcceptedPlayers, r.matchRealPlayers, r.IsAssigned() ? 1 : 0, r.assignment.serverAddress.c_str(), r.assignment.serverPort,
        r.assignment.matchId.c_str(), r.discovered ? "   <== DISCOVERED (party member: mode " : "");
    if (r.discovered) { printf("%s game_type=%u)\n", r.mode.c_str(), r.gameType); }
    fflush(stdout);
    std::lock_guard l{ g_resultMutex };
    g_results.push_back(r);
}
// runs curl.exe against the test backend, returns its output (the harness plays the parts of the party that are not the tracked GC)
static std::string Curl(const std::string &args)
{
    const std::string out = "e2e_curl_out.txt";
    const std::string cmd = "curl.exe -s -m 5 --noproxy * -H \"X-Api-Key: test-api\" -H \"Content-Type: application/json\" " + args + " > " + out;
    (void)system(cmd.c_str());
    FILE *f = fopen(out.c_str(), "rb");
    std::string text;
    if (f) { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n); fclose(f); }
    return text;
}

static bool WaitStatus(const std::string &status, int ms)
{
    auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end)
    {
        { std::lock_guard l{ g_resultMutex }; for (auto &r : g_results) if (r.status == status && !r.discovered) return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

static bool SeenStatus(const std::string &status)
{
    std::lock_guard l{ g_resultMutex };
    for (auto &r : g_results) if (r.status == status && !r.discovered) return true;
    return false;
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
    const bool partyScenario = argc > 4 && std::string(argv[4]) == "party";
    const bool gatherScenario = argc > 4 && std::string(argv[4]) == "gather";
    const bool threeScenario = argc > 4 && std::string(argv[4]) == "three";

    if (threeScenario)
    {
        bool ok = true;
        auto verdict = [&](bool cond, const char *what) { printf("%7.3f [check] %-4s %s\n", Now(), cond ? "OK" : "FAIL", what); ok = ok && cond; };
        const uint32_t a = real, b = real + 1, c = real + 2;   // A and C search from other machines (curl), this process is the GC of B
        const uint32_t fakeCount = (uint32_t)required - 3;

        auto searchWithCurl = [&](uint32_t account, const char *requestId)
        {
            char body[512];
            snprintf(body, sizeof(body),
                "-d \"{\\\"account_id\\\":%u,\\\"game_type\\\":520,\\\"mode\\\":\\\"competitive\\\",\\\"maps\\\":[\\\"de_dust2\\\"],"
                "\\\"request_id\\\":\\\"%s\\\"}\" http://127.0.0.1:18090/api/v1/matchmaking/search", account, requestId);
            Curl(body);
        };
        auto reportWithCurl = [&](uint32_t account, const char *requestId)
        {
            char report[256];
            snprintf(report, sizeof(report), "-d \"{\\\"account_id\\\":%u,\\\"request_id\\\":\\\"%s\\\"}\" http://127.0.0.1:18090/api/v1/matchmaking/accepted", account, requestId);
            return Curl(report);
        };

        printf("%7.3f [A's client] Competitive search (account %u)\n", Now(), a);
        searchWithCurl(a, "thr-a");
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        printf("%7.3f [B's client] Competitive search (account %u)\n", Now(), b);
        BackendClient::SearchInfo info;
        info.accountId = b; info.gameType = 520; info.mode = "competitive"; info.gameMode = "competitive"; info.maps = { "de_dust2" };
        BackendClient::SearchStarted(info, 76561197960265728ull + b);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        printf("%7.3f [C's client] Competitive search (account %u)\n", Now(), c);
        searchWithCurl(c, "thr-c");

        verdict(WaitStatus("MATCHED", 4000), "B is placed in a gathering match: no server, no Match Found yet");
        {
            bool three = false;
            for (int i = 0; i < 200 && !three; i++)   // C joins a second after B: B's next poll reports it
            {
                { std::lock_guard l{ g_resultMutex }; for (auto &r : g_results) three = three || (r.status == "MATCHED" && r.matchPlayers == 3); }
                if (!three) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            verdict(three, "the gathering match has 3 players: A, B and C (real players only, the virtual ones are not decided yet)");
        }

        const bool assignedB = WaitAssigned(waitMs);
        verdict(assignedB, "B got its OWN Match Found (assignment) after the gather window");
        if (!assignedB) { printf("RESULT: FAILED\n"); return 3; }
        {
            std::lock_guard l{ g_resultMutex };
            bool all = false, counts = false;
            for (auto &r : g_results)
            {
                if (!r.IsAssigned()) continue;
                bool hasA = false, hasB = false, hasC = false;
                for (uint32_t id : r.assignment.realAccounts) { hasA = hasA || id == a; hasB = hasB || id == b; hasC = hasC || id == c; }
                all = all || (hasA && hasB && hasC);
                counts = counts || (r.assignment.realPlayers == 3 && r.assignment.fakePlayers == fakeCount
                    && r.assignment.requiredPlayers == (uint32_t)required);
            }
            verdict(all, "the assignment lists all THREE real accounts (the roster the game server arms)");
            verdict(counts, "3 real + exactly the configured fake players = the roster (no topping up to the capacity of 10)");
        }

        // the popup needs all three real players at stage 1
        PlayerCheck a1 = Check(a, 1);
        verdict(a1.got && a1.awaiting == 2 && a1.total == (uint32_t)required, "A stage 1: awaiting=2 (B and C are not there): NO popup for one player");
        PlayerCheck b1 = Check(b, 1);
        verdict(b1.got && b1.awaiting == 1, "B stage 1: awaiting=1 (C is not there): still no popup");
        PlayerCheck c1 = Check(c, 1);
        verdict(c1.got && c1.awaiting == 0, "C stage 1: awaiting=0 (all three): the popup is up");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        verdict(Check(a, 1).awaiting == 0 && Check(b, 1).awaiting == 0, "and for A and B too: everybody has its Confirm Match");

        // Accepts one by one
        PlayerCheck a2 = Check(a, 2);
        verdict(a2.got && a2.awaiting >= 1, "ONE Accept (A): the server does not let anybody on");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        PlayerCheck b2 = Check(b, 2);
        verdict(b2.got && b2.awaiting == 1, "TWO Accepts (A, B): still awaiting=1 (C): nobody connects");
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        verdict(Check(a, 2).awaiting == 1, "... also for A after the virtual players accepted (they have no popup of their own)");

        PlayerCheck c2 = Check(c, 2);
        verdict(c2.got && c2.awaiting == 0, "THREE Accepts: stage 2 awaiting=0");

        // the GCs report; the backend lets B connect only when all three have reported
        printf("%7.3f [B's GC] reports the Accept\n", Now());
        BackendClient::ReportAccepted();
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        verdict(!SeenStatus("READY_TO_CONNECT"), "B is NOT sent on: A and C have not reported");
        printf("%7.3f [A's GC] reports the Accept\n", Now());
        std::string first = reportWithCurl(a, "thr-a");
        verdict(first.find("\"match_accepted\":false") != std::string::npos, "the backend: 2 of 3 reported, not accepted yet");
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        verdict(!SeenStatus("READY_TO_CONNECT"), "B is still NOT sent on");
        {
            std::lock_guard l{ g_resultMutex };
            bool progress = false;
            for (auto &r : g_results) progress = progress || (r.IsAssigned() && r.matchRealPlayers == 3 && r.matchAcceptedPlayers == 2);
            verdict(progress, "B's poll says: 2 of 3 real players accepted");
        }
        printf("%7.3f [C's GC] reports the Accept\n", Now());
        std::string last = reportWithCurl(c, "thr-c");
        verdict(last.find("\"match_accepted\":true") != std::string::npos, "the backend: every real player accepted");
        verdict(WaitStatus("READY_TO_CONNECT", 6000), "B's GC hears READY_TO_CONNECT -> connect (QueueConnect)");

        BackendClient::StopPolling();
        BackendClient::SearchCancelled();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        printf("RESULT: %s\n", ok ? "OK" : "FAILED");
        return ok ? 0 : 3;
    }

    if (gatherScenario)
    {
        bool ok = true;
        auto verdict = [&](bool cond, const char *what) { printf("%7.3f [check] %-4s %s\n", Now(), cond ? "OK" : "FAIL", what); ok = ok && cond; };
        const uint32_t a = real, b = real + 1;      // A searches from another machine (curl), this process is the GC of B

        char body[512];
        snprintf(body, sizeof(body),
            "-d \"{\\\"account_id\\\":%u,\\\"game_type\\\":520,\\\"mode\\\":\\\"competitive\\\",\\\"maps\\\":[\\\"de_dust2\\\"],"
            "\\\"request_id\\\":\\\"gat-a\\\"}\" http://127.0.0.1:18090/api/v1/matchmaking/search", a);
        printf("%7.3f [A's client] starts a Competitive search (account %u)\n", Now(), a);
        Curl(body);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        BackendClient::SearchInfo info;
        info.accountId = b; info.gameType = 520; info.mode = "competitive"; info.gameMode = "competitive"; info.maps = { "de_dust2" };
        printf("%7.3f [B's client] starts a Competitive search 1.5 s later (account %u)\n", Now(), b);
        BackendClient::SearchStarted(info, 76561197960265728ull + b);

        verdict(WaitStatus("MATCHED", 4000), "B is placed in a gathering match (no server, no Match Found for one player)");
        {
            std::lock_guard l{ g_resultMutex };
            bool two = false;
            for (auto &r : g_results) two = two || (r.status == "MATCHED" && r.matchPlayers == 2);
            verdict(two, "B's match has 2 players: it is A's match, the fake players have not filled it up yet");
        }

        const bool assignedB = WaitAssigned(waitMs);
        verdict(assignedB, "B got its OWN Match Found (assignment) once the gather window was over");
        if (!assignedB) { printf("RESULT: FAILED\n"); return 3; }
        {
            std::lock_guard l{ g_resultMutex };
            bool both = false, counts = false;
            for (auto &r : g_results)
            {
                if (!r.IsAssigned()) continue;
                bool hasA = false, hasB = false;
                for (uint32_t id : r.assignment.realAccounts) { hasA = hasA || id == a; hasB = hasB || id == b; }
                both = both || (hasA && hasB);
                counts = counts || (r.assignment.realPlayers == 2 && r.assignment.fakePlayers == (uint32_t)required - 2);
            }
            verdict(both, "the assignment lists BOTH real accounts (the roster the game server arms)");
            verdict(counts, "2 real + the fake players that were missing (pool of 9 gave 8)");
        }

        // srcds armed both: the popup needs BOTH players at stage 1
        PlayerCheck a1 = Check(a, 1);
        verdict(a1.got && a1.awaiting == 1 && a1.total == (uint32_t)required, "A stage 1: awaiting=1 (B has not got there): NO popup for one player");
        PlayerCheck b1 = Check(b, 1);
        verdict(b1.got && b1.awaiting == 0, "B stage 1: awaiting=0 (both are in): the popup is up");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        PlayerCheck a1b = Check(a, 1);
        verdict(a1b.got && a1b.awaiting == 0, "and for A too");

        // A accepts first: he is not sent on
        PlayerCheck a2 = Check(a, 2);
        verdict(a2.got && a2.awaiting >= 1, "A accepts first: stage 2 awaiting>0 -> the server does NOT let him on alone");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        a2 = Check(a, 2);
        verdict(a2.got && a2.awaiting == 1, "... still not after the fake players accepted (B has not)");

        PlayerCheck b2 = Check(b, 2);
        verdict(b2.got && b2.awaiting == 0, "B accepts: stage 2 awaiting=0");
        printf("%7.3f [B's GC] reports the Accept (BackendClient::ReportAccepted)\n", Now());
        BackendClient::ReportAccepted();
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        verdict(!SeenStatus("READY_TO_CONNECT"), "B is NOT sent on: A has not reported, the backend says not everybody accepted");
        {
            std::lock_guard l{ g_resultMutex };
            bool progress = false;
            for (auto &r : g_results) progress = progress || (r.IsAssigned() && r.matchRealPlayers == 2 && r.matchAcceptedPlayers == 1);
            verdict(progress, "B's poll says: 1 of 2 real players accepted");
        }

        char report[256];
        snprintf(report, sizeof(report), "-d \"{\\\"account_id\\\":%u,\\\"request_id\\\":\\\"gat-a\\\"}\" http://127.0.0.1:18090/api/v1/matchmaking/accepted", a);
        printf("%7.3f [A's GC] reports the Accept\n", Now());
        std::string answer = Curl(report);
        verdict(answer.find("\"match_accepted\":true") != std::string::npos, "the backend: every real player accepted (match_accepted)");
        verdict(WaitStatus("READY_TO_CONNECT", 6000), "B's GC now hears READY_TO_CONNECT -> connect (QueueConnect)");

        BackendClient::StopPolling();
        BackendClient::SearchCancelled();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        printf("RESULT: %s\n", ok ? "OK" : "FAILED");
        return ok ? 0 : 3;
    }

    if (partyScenario)
    {
        bool ok = true;
        auto verdict = [&](bool cond, const char *what) { printf("%7.3f [check] %-4s %s\n", Now(), cond ? "OK" : "FAIL", what); ok = ok && cond; };
        const uint32_t leader = real, member = real + 1;    // this process is the GC of the MEMBER

        BackendClient::WatchAccount(member);
        char body[512];
        snprintf(body, sizeof(body),
            "-d \"{\\\"account_id\\\":%u,\\\"game_type\\\":520,\\\"mode\\\":\\\"competitive\\\",\\\"maps\\\":[\\\"de_dust2\\\"],"
            "\\\"party_account_ids\\\":[%u,%u],\\\"request_id\\\":\\\"lead-e2e\\\"}\" http://127.0.0.1:18090/api/v1/matchmaking/search",
            leader, leader, member);
        printf("%7.3f [leader's client] starts a Competitive search for the lobby [%u, %u]\n", Now(), leader, member);
        Curl(body);

        // the member's GC finds it on its own and is assigned the server
        bool assignedB = WaitAssigned(waitMs);
        verdict(assignedB, "the party MEMBER's GC found the leader's search and was assigned the server (it got its own Match Found)");
        { std::lock_guard l{ g_resultMutex }; bool disc = false; for (auto &r : g_results) disc = disc || (r.discovered && r.partyMember && r.mode == "competitive"); verdict(disc, "the search was reported as discovered for a party member (mode competitive)"); }
        if (!assignedB) { printf("RESULT: FAILED\n"); return 3; }

        // both real players are in the srcds roster: the leader alone is NOT enough for the popup
        PlayerCheck a1 = Check(leader, 1);
        verdict(a1.got && a1.awaiting == 1 && a1.total == (uint32_t)required, "leader stage 1: awaiting=1 (the member is still to arrive): the popup is NOT up for one player");
        PlayerCheck b1 = Check(member, 1);
        verdict(b1.got && b1.awaiting == 0, "member stage 1: awaiting=0 (both are in): the popup is up for the member");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        PlayerCheck a1b = Check(leader, 1);
        verdict(a1b.got && a1b.awaiting == 0, "and for the leader too (everybody Match Found)");

        // the leader accepts FIRST
        PlayerCheck a2 = Check(leader, 2);
        verdict(a2.got && a2.awaiting >= 1, "leader accepts first: stage 2 awaiting>0 (the member and the fakes have not) -> the server does NOT let him on alone");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        a2 = Check(leader, 2);
        verdict(a2.got && a2.awaiting == 1, "... and still not after the fake players accepted (the member has not)");

        // the member accepts: the game server now says stage 2 awaiting 0. Its GC reports it - the leader has not reported yet
        PlayerCheck b2 = Check(member, 2);
        verdict(b2.got && b2.awaiting == 0, "member accepts: stage 2 awaiting=0");
        printf("%7.3f [member's GC] reports the Accept (BackendClient::ReportAccepted)\n", Now());
        BackendClient::ReportAccepted();
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        verdict(!SeenStatus("READY_TO_CONNECT"), "the member is NOT sent on: the leader has not reported, the backend says not everybody accepted");

        // the leader's GC reports (its 0x25 awaiting 0 arrives after the member's accept)
        PlayerCheck a2c = Check(leader, 2);
        verdict(a2c.got && a2c.awaiting == 0, "leader: stage 2 awaiting=0 now");
        std::string leaderRequest = "lead-e2e";
        char report[256];
        snprintf(report, sizeof(report), "-d \"{\\\"account_id\\\":%u,\\\"request_id\\\":\\\"%s\\\"}\" http://127.0.0.1:18090/api/v1/matchmaking/accepted",
            leader, leaderRequest.c_str());
        printf("%7.3f [leader's GC] reports the Accept\n", Now());
        std::string answer = Curl(report);
        verdict(answer.find("\"match_accepted\":true") != std::string::npos, "the backend: every real player accepted (match_accepted)");
        verdict(WaitStatus("READY_TO_CONNECT", 6000), "the member's GC now hears READY_TO_CONNECT -> connect (QueueConnect)");

        BackendClient::StopPolling();
        BackendClient::SearchCancelled();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        printf("RESULT: %s\n", ok ? "OK" : "FAILED");
        return ok ? 0 : 3;
    }

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
        verdict(WaitStatus("READY_TO_CONNECT", 6000), "the backend answers: everybody accepted (the search is READY_TO_CONNECT) -> connect");
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
