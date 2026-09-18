#pragma once

#include <functional>
#include <string>
#include <string_view>

// TEST ONLY -- scaffolding for exercising the retail Accept flow (game_type 8/10/13) end to end
// before a real backend exists. See RESEARCH_FINDINGS.md #41/#42.
//
// Two independent halves, one per process:
//
//  * SERVER side (srcds): builds the 'Q' reservation payload for ReserveServerForQueuedGame with
//    the real player's AccountID plus deterministic fake AccountIDs, and runs a small driver
//    (FakeRoster) that makes the fake participants "probe" (A2S_RESERVE_CHECK stage 1) and later
//    "accept" (stage 2) by sending real 0x21 datagrams to the local dedicated server. The engine's
//    own CBaseServer::ReplyReservationCheckRequest then computes awaiting/total exactly like it does
//    for real players -- nothing in the engine is patched.
//
//  * CLIENT side (csgo.exe): observes the retail S2A_RESERVE_CHECK_RESPONSE (0x25) datagrams. When the
//    server reports stage 2 with awaiting == 0 (everybody accepted) from the armed reservation
//    address, it notifies ClientGC, which then sends the second MatchmakingGC2ClientReserve (9107).
//    This is the TEST ONLY replacement for the missing retail server->GC->client completion path
//    (ReportGCQueuedMatchStart is a stub in public builds, see RESEARCH_FINDINGS.md #40).
namespace AcceptTest
{

struct Mode
{
    const char *name;     // config value of matchmaking.test_accept_mode
    uint32_t eGame;       // game_type & 0xF as seen in MatchmakingStart (9101)
    uint32_t rosterSize;  // real player + fake participants
    const char *map;      // map advertised to the client in 9107
};

const Mode *FindModeByName(std::string_view name);
const Mode *FindModeByGame(uint32_t eGame);

// Fake AccountIDs live far above any real Steam account id (real ones are < 2^31 today), so
// they can never collide with a real player: 0xFA4E0000 ("FAKE") + 1-based index.
constexpr uint32_t FakeAccountBase = 0xFA4E0000u;
inline uint32_t FakeAccountId(uint32_t index) { return FakeAccountBase + index; }

// "Q<cookie>,<matchid>,1:[<real>][<fake1>]...[<fakeN-1>]" -- the third field is bReserve (not a count)
std::string BuildQueuedReservationPayload(uint64_t cookie, uint32_t realAccountId, uint32_t rosterSize);
// same payload header with bReserve=0 (IVEngineServer::ReserveServerForQueuedGame -> Unreserve())
std::string BuildUnreservePayload(uint64_t cookie);

// ---- server side ------------------------------------------------------------------------------

class FakeRoster
{
public:
    struct Params
    {
        uint64_t cookie;
        uint32_t realAccountId;
        uint32_t rosterSize;      // total, including the real player
        std::string serverAddress; // where the dedicated server's game socket is reachable
        uint16_t serverPort;
        uint32_t acceptDelayMs;   // after the accept popup is up, before the first fake accepts
    };

    // postReserve queues an IVEngineServer::ReserveServerForQueuedGame payload for the main thread
    using PostReserveFn = std::function<void(const std::string &payload)>;

    FakeRoster(const Params &params, PostReserveFn postReserve);
    ~FakeRoster();

    FakeRoster(const FakeRoster &) = delete;
    FakeRoster &operator=(const FakeRoster &) = delete;

    // a real client connected: stop refreshing the reservation and stop talking to the server
    void OnMatchStarted();
    // the last real client left: unreserve + reserve again with a fresh roster (stage 0 for everybody)
    void OnMatchEnded();

private:
    struct Impl;
    Impl *m_impl;
};

// ---- client side ------------------------------------------------------------------------------

using ClientNotifyFn = void (*)(void *context);

// hooks ws2_32!WSARecvFrom (the engine's recvfrom ends up there) in the current process (main thread, once). Windows only, no-op elsewhere.
void InstallClientRecvHook();

// who to call when the armed reservation is fully accepted (0x25, stage 2, awaiting 0); nullptr clears
void SetClientNotify(ClientNotifyFn fn, void *context);

// start/stop watching 0x25 datagrams coming from serverIp:serverPort (host byte order)
void ArmClient(uint32_t serverIp, uint16_t serverPort);
void DisarmClient();

} // namespace AcceptTest
