// Offline regression test of RESEARCH_FINDINGS.md #61 (Training / bots: "Invalid user info").
//
// The failure: a csgo.exe that had started a non-Accept matchmaking search reserved ITS OWN engine server for the game type
// (ClientGC::StartServerFlow -> IVEngineServer::ReserveServerForQueuedGame), the cookie never expired there, and the next
// local Training game was rejected by CBaseServer::ConnectClient ("mismatching cookie from loopback, client 0, server
// 293a206f6c6c6548"). Two parts:
//   1. the engine rule (baseserver.cpp ConnectClient) as a model, replayed with the old and the new client flow;
//   2. the call graph, checked on the real sources (copied to the build folder by build.bat): who may call
//      ReserveServerForQueuedGame in which process kind, and that the flows that must keep it still do.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int g_checks;
static int g_failed;

#define CHECK(cond) \
    do \
    { \
        g_checks++; \
        if (!(cond)) \
        { \
            g_failed++; \
            printf("  FAILED line %d: %s\n", __LINE__, #cond); \
        } \
    } while (0)

static constexpr uint64_t Cookie = 0x293A206F6C6C6548ull;

// ---- 1. engine model ------------------------------------------------------------------------------------------------

// The process' engine server object (`sv`). Only ReserveServerForQueuedGame(bReserve=1) writes the cookie; nothing expires
// it on a listen server (the expiry runs in the dedicated SV_Think only, sv_main.cpp:3331), which is what made the cookie
// sticky.
struct EngineServer
{
    uint64_t reservationCookie{};
    bool dedicated{};

    void Reserve(uint64_t cookie) { reservationCookie = cookie; }

    // baseserver.cpp:686-726: a reserved server only accepts clients whose cl_session (userinfo "$<cookie>") matches
    bool ConnectClient(uint64_t clientSession) const
    {
        return reservationCookie == 0 || clientSession == reservationCookie;
    }
};

// what the client process does when the backend assigned a server (the part of StartServerFlow that touches the engine)
static void ClientStartServerFlow(EngineServer &localSv, bool acceptRequired, bool clientReservesLocalEngine)
{
    if (acceptRequired)
    {
        return; // Accept modes never did
    }
    if (clientReservesLocalEngine)
    {
        localSv.Reserve(Cookie);
    }
}

static void TestEngineModel()
{
    printf("engine model\n");

    // old behaviour: Casual search, then Training -> rejected (the observed failure)
    {
        EngineServer sv;
        ClientStartServerFlow(sv, false, true);
        CHECK(sv.reservationCookie == Cookie);
        CHECK(!sv.ConnectClient(0));
    }

    // new behaviour: the same sequence leaves the local server alone -> Training connects
    {
        EngineServer sv;
        ClientStartServerFlow(sv, false, false);
        CHECK(sv.reservationCookie == 0);
        CHECK(sv.ConnectClient(0));
    }

    // Accept flow (Competitive/Wingman/Danger Zone) never touched the local engine
    {
        EngineServer sv;
        ClientStartServerFlow(sv, true, false);
        CHECK(sv.ConnectClient(0));
    }

    // the dedicated srcds flow is untouched: its own server is reserved by its own GC and the client's QueueConnect
    // session carries the same cookie
    {
        EngineServer srcds;
        srcds.dedicated = true;
        srcds.Reserve(Cookie);
        CHECK(srcds.ConnectClient(Cookie));
        CHECK(!srcds.ConnectClient(0)); // a stranger without the cookie is still refused (Valve_Reject_Reserved_For_Lobby)
    }
}

// ---- 2. call graph on the real sources ------------------------------------------------------------------------------

static std::string ReadFile(const char *path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// the text of `void Class::name(...) { ... }` up to the closing brace at column 0
static std::string FunctionBody(const std::string &source, const std::string &signature)
{
    size_t start = source.find(signature);
    if (start == std::string::npos)
    {
        return {};
    }
    size_t end = source.find("\n}\n", start);
    return end == std::string::npos ? source.substr(start) : source.substr(start, end - start + 3);
}

static bool Has(const std::string &text, const char *needle)
{
    return text.find(needle) != std::string::npos;
}

// Drops // comments so that a mention of an identifier in the explaining comment does not count as a call.
static std::string StripLineComments(const std::string &text)
{
    std::string out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        size_t pos = line.find("//");
        out += (pos == std::string::npos ? line : line.substr(0, pos)) + "\n";
    }
    return out;
}

static void TestCallGraph()
{
    printf("call graph\n");

    const std::string client = ReadFile("gc_client.cpp.src");
    const std::string server = ReadFile("gc_server.cpp.src");
    CHECK(!client.empty());
    CHECK(!server.empty());

    // the client process never posts a reservation for its own engine
    CHECK(!Has(StripLineComments(client), "HostEvent::ReserveServerForQueuedGame"));
    CHECK(!Has(StripLineComments(client), "BuildReservationPayload"));

    // StartServerFlow keeps everything else: the assignment message for the client, the Accept flow and its arming
    const std::string flow = StripLineComments(FunctionBody(client, "void ClientGC::StartServerFlow("));
    CHECK(!flow.empty());
    CHECK(Has(flow, "MatchmakingGC2ClientReserve"));
    CHECK(Has(flow, "set_reservationid(GameServerCookieId)"));
    CHECK(Has(flow, "mode->acceptRequired"));
    CHECK(Has(flow, "m_pendingAccept.active = true"));
    CHECK(Has(flow, "AcceptTest::ArmClient"));
    CHECK(!Has(flow, "PostToHost"));

    // the server GC: dedicated -> keep-alive lease, listen -> nothing
    const std::string reserve = StripLineComments(FunctionBody(server, "void ServerGC::ReserveServerForOurCookie()"));
    CHECK(!reserve.empty());
    CHECK(Has(reserve, "if (m_dedicated)"));
    CHECK(Has(reserve, "HostEvent::ReservationKeepAlive"));
    CHECK(!Has(reserve, "HostEvent::ReserveServerForQueuedGame"));
    // the accept-test roster still gets first pick, before the dedicated/listen split
    size_t roster = reserve.find("StartAcceptTestRoster()");
    size_t dedicated = reserve.find("if (m_dedicated)");
    CHECK(roster != std::string::npos && dedicated != std::string::npos && roster < dedicated);
}

int main()
{
    TestEngineModel();
    TestCallGraph();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
