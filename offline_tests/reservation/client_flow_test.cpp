// Offline regression test of RESEARCH_FINDINGS.md #61 (Training / bots: "Invalid user info").
//
// The failure: a csgo.exe that had started a non-Accept matchmaking search reserved ITS OWN engine server for the game type
// (ClientGC::StartServerFlow -> IVEngineServer::ReserveServerForQueuedGame), the cookie never expired there, and the next
// local Training game was rejected by CBaseServer::ConnectClient ("mismatching cookie from loopback, client 0, server
// 293a206f6c6c6548"). Two parts:
//   1. the engine rule (baseserver.cpp ConnectClient) as a model, replayed with the old and the new client flow;
//   2. the call graph, checked on the real sources (copied to the build folder by build.bat): who may call
//      ReserveServerForQueuedGame in which process kind, and that the flows that must keep it still do.
#include <algorithm>
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
    std::string text = ss.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());   // a checkout may have CRLF line ends
    return text;
}

// the text of `void Class::name(...) { ... }` up to the closing brace at column 0
static std::string FunctionBody(const std::string &source, const std::string &signature, const char *close = "\n}\n")
{
    size_t start = source.find(signature);
    if (start == std::string::npos)
    {
        return {};
    }
    size_t end = source.find(close, start);
    return end == std::string::npos ? source.substr(start) : source.substr(start, end - start + std::string(close).size());
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

// ---- 3. Accept gating (RESEARCH_FINDINGS.md #65): nobody connects on his own accept ----------------------------------------
static void TestAcceptGating()
{
    printf("accept gating\n");

    const std::string client = ReadFile("gc_client.cpp.src");
    const std::string backend = ReadFile("backend_client.cpp.src");
    CHECK(!client.empty() && !backend.empty());

    // the notification of the game server (0x25 stage 2 awaiting 0) only reports; it does not connect by itself
    const std::string notified = StripLineComments(FunctionBody(client, "void ClientGC::OnReservationFullyAccepted()"));
    CHECK(!notified.empty());
    CHECK(Has(notified, "BackendClient::ReportAccepted()"));
    CHECK(Has(notified, "TryConnectAfterAccept()"));
    CHECK(!Has(notified, "SendMessageToGame"));
    CHECK(!Has(notified, "MatchmakingGC2ClientReserve"));

    // the connect (second 9107) needs BOTH: the game server and the backend
    const std::string connect = StripLineComments(FunctionBody(client, "void ClientGC::TryConnectAfterAccept()"));
    CHECK(!connect.empty());
    CHECK(Has(connect, "!m_pendingAccept.serverAccepted || !m_pendingAccept.backendAccepted"));
    CHECK(Has(connect, "MatchmakingGC2ClientReserve"));
    // ... and it is the only place that sends it
    size_t sends = 0;
    for (size_t at = client.find("SetFinalAcceptGameType"); at != std::string::npos; at = client.find("SetFinalAcceptGameType", at + 1))
    {
        sends++;
    }
    CHECK(sends == 1);

    // the backend's answer that every real player accepted is what completes the second condition
    const std::string result = StripLineComments(FunctionBody(client, "void ClientGC::OnBackendSearchResult("));
    CHECK(Has(result, "READY_TO_CONNECT"));
    CHECK(Has(result, "backendAccepted = true"));

    // the search keeps being polled after the report (the report must not stop it), the connect stops it
    const std::string report = StripLineComments(FunctionBody(backend, "    void ReportAccepted()", "\n    }\n"));
    CHECK(!report.empty());
    CHECK(!Has(report, "Phase::Done"));

    // a lobby leader reports the whole party, a member's GC follows the leader's search
    const std::string start = StripLineComments(FunctionBody(client, "void ClientGC::OnMatchmakingStart("));
    CHECK(Has(start, "partyAccountIds"));
    CHECK(Has(StripLineComments(client), "BackendClient::WatchAccount("));
    CHECK(Has(StripLineComments(FunctionBody(client, "void ClientGC::OnPartySearchDiscovered(")), "m_followsParty = true"));
}

// ---- 4. every player has to hear about the match and see who else is in it (RESEARCH_FINDINGS.md #66) ------------------------
static void TestMatchFoundForEveryPlayer()
{
    printf("match found for every player\n");

    const std::string client = ReadFile("gc_client.cpp.src");
    const std::string backend = ReadFile("backend_client.cpp.src");
    const std::string server = ReadFile("gc_server.cpp.src");
    CHECK(!client.empty() && !backend.empty() && !server.empty());

    // the 9107 of a client is built from the assignment ITS OWN GC got (nobody sends one for another player): the roster of
    // the match is logged there, with a warning when the account is not in it
    const std::string flow = StripLineComments(FunctionBody(client, "void ClientGC::StartServerFlow("));
    CHECK(Has(flow, "assignment.realAccounts"));
    CHECK(Has(flow, "NOT IN THE ROSTER"));
    CHECK(Has(flow, "MatchmakingGC2ClientReserve"));
    // the total the client expects from the game server is the roster of the backend's match (real + the configured fake players,
    // RESEARCH_FINDINGS.md #67), not the capacity of the mode
    CHECK(Has(flow, "assignment.requiredPlayers"));
    CHECK(Has(flow, "AcceptTest::ArmClient(testServerIp, testServerPort, rosterSize)"));

    // the progress of the accepts reaches the client and is logged on every change
    const std::string result = StripLineComments(FunctionBody(client, "void ClientGC::OnBackendSearchResult("));
    CHECK(Has(result, "matchAcceptedPlayers"));
    CHECK(Has(result, "nobody connects before all of them did"));
    CHECK(Has(backend, "accepted_players"));
    CHECK(Has(backend, "real_accounts"));

    // srcds: a real player's first 0x21 before the backend answered its first roster poll must not arm the one-real-player
    // legacy roster (the first search would lose every player but one)
    const std::string seen = StripLineComments(FunctionBody(server, "void ServerGC::OnTestRealPlayerSeen("));
    CHECK(!seen.empty());
    CHECK(Has(seen, "HasSnapshot()"));
    CHECK(Has(seen, "m_testPendingSniff"));
    size_t wait = seen.find("HasSnapshot()");
    size_t legacy = seen.find("CreateTestRoster(");
    CHECK(wait != std::string::npos && legacy != std::string::npos && wait < legacy);
    const std::string roster = StripLineComments(FunctionBody(server, "void ServerGC::OnBackendRoster("));
    CHECK(Has(roster, "m_testPendingSniff"));

    // srcds -backend_ip/-backend_port: the roster poller identifies the game server to the backend by the backend address,
    // while everything that talks to the game server itself (the fake participants) keeps -ip/-port
    // (offline_tests/roster/launch_args_test.cpp runs the real Poller with what these getters resolve to)
    const std::string start = StripLineComments(FunctionBody(server, "bool ServerGC::StartAcceptTestRoster("));
    CHECK(!start.empty());
    CHECK(Has(start, "RosterFeed::Poller>(config.BackendServerAddress(), config.BackendServerPort()"));
    CHECK(!Has(start, "RosterFeed::Poller>(config.DedicatedServerAddress()"));
    const std::string legacyRoster = StripLineComments(FunctionBody(server, "void ServerGC::CreateTestRoster("));
    CHECK(Has(legacyRoster, "params.serverAddress = config.DedicatedServerAddress();"));
    CHECK(Has(legacyRoster, "params.serverPort = config.DedicatedServerPort();"));
    const std::string armed = StripLineComments(FunctionBody(server, "void ServerGC::ArmRoster("));
    CHECK(Has(armed, "params.serverAddress = config.DedicatedServerAddress();"));
    CHECK(Has(armed, "params.serverPort = config.DedicatedServerPort();"));
    CHECK(!Has(armed, "BackendServer") && !Has(legacyRoster, "BackendServer"));
}

int main()
{
    TestEngineModel();
    TestCallGraph();
    TestAcceptGating();
    TestMatchFoundForEveryPlayer();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
