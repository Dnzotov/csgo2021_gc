#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// TEST ONLY -- scaffolding for exercising the retail Accept flow (game_type 8/10/11/13) end to end
// before a real backend exists. See RESEARCH_FINDINGS.md #41/#42/#45. The mode table (roster sizes) lives in
// mm_modes.h.
//
// Two independent halves, one per process:
//
//  * SERVER side (srcds): builds the 'Q' reservation payload for ReserveServerForQueuedGame with
//    the real player's AccountID plus deterministic fake AccountIDs, and runs a small driver
//    (FakeRoster) that makes the fake participants "probe" (A2S_RESERVE_CHECK stage 1) and later
//    "accept" (stage 2) by sending real 0x21 datagrams to the local dedicated server. The engine's
//    own CBaseServer::ReplyReservationCheckRequest then computes awaiting/total exactly like it does
//    for real players -- nothing in the engine is patched. The real player's AccountID is learned from the
//    first 0x21 the real client sends (SetServerSniff), no config needed.
//
//  * CLIENT side (csgo.exe): observes the retail S2A_RESERVE_CHECK_RESPONSE (0x25) datagrams. When the
//    server reports stage 2 with awaiting == 0 (everybody accepted) from the armed reservation
//    address, it notifies ClientGC, which then sends the second MatchmakingGC2ClientReserve (9107).
//    This is the TEST ONLY replacement for the missing retail server->GC->client completion path
//    (ReportGCQueuedMatchStart is a stub in public builds, see RESEARCH_FINDINGS.md #40).
namespace AcceptTest
{

// Fake AccountIDs live far above any real Steam account id (real ones are < 2^31 today), so
// they can never collide with a real player: 0xFA4E0000 ("FAKE") + 1-based index.
constexpr uint32_t FakeAccountBase = 0xFA4E0000u;
inline uint32_t FakeAccountId(uint32_t index) { return FakeAccountBase + index; }
inline bool IsFakeAccountId(uint32_t accountId) { return (accountId & 0xFFFF0000u) == FakeAccountBase; }

// "Q<cookie>,<matchid>,1:[<real>][<fake1>]...[<fakeN-1>]" -- the third field is bReserve (not a count)
std::string BuildQueuedReservationPayload(uint64_t cookie, uint32_t realAccountId, uint32_t rosterSize);
// the same for an explicit roster (the backend's: real AccountIDs and fake ones, in roster order)
std::string BuildQueuedReservationPayload(uint64_t cookie, const std::vector<uint32_t> &participants);
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
        uint32_t rosterSize;       // total, including the real player (== the mode's required players)
        std::string modeName;      // for the log
        std::string serverAddress; // where the dedicated server's game socket is reachable
        uint16_t serverPort;
        uint32_t acceptDelayMs;    // after the accept popup is up, before the first fake accepts

        // ---- backend-driven roster (RESEARCH_FINDINGS.md #55) ----
        // The roster the backend assigned to the match, in roster order: real AccountIDs and fake ones (IsFakeAccountId).
        // Empty = the legacy roster of this driver: realAccountId + FakeAccountId(1 .. rosterSize - 1). The fake entries
        // are the ones this driver makes probe / accept; a real player answers for itself.
        std::vector<uint32_t> participants;
        std::string source;              // for the log: "legacy" / "backend m-xxxxxxxx"
        // a reservation with another roster is still active: unreserve it, wait for the engine to drop the cookie, then arm
        bool unreserveFirst{};
        // called from the driver thread: true = every fake participant is confirmed at stage 1 (the reservation is ready
        // for the real player), false = the roster is being (re)armed
        std::function<void(bool ready)> onFakesReady;
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
    // the match this roster was armed for is gone (the backend cancelled it, RESEARCH_FINDINGS.md #63): unreserve once and
    // stop refreshing the reservation. Unlike OnMatchEnded nothing is armed again; a new roster needs a new FakeRoster.
    void Release();

private:
    struct Impl;
    Impl *m_impl;
};

// srcds: called (from the engine's network thread) with the AccountID of the first real (non-fake) player whose
// 0x21 for our cookie was received. Pass nullptr to clear.
using ServerSniffFn = void (*)(void *context, uint32_t accountId);
void SetServerSniff(ServerSniffFn fn, void *context, uint64_t cookie);

// ---- client side ------------------------------------------------------------------------------

using ClientNotifyFn = void (*)(void *context);

// hooks ws2_32!WSARecvFrom (the engine's recvfrom ends up there) in the current process (main thread, once).
// Client: watches 0x25 (ArmClient); srcds: watches 0x21 (SetServerSniff). Windows only, no-op elsewhere.
void InstallRecvHook();

// who to call when the armed reservation is fully accepted (0x25, stage 2, awaiting 0); nullptr clears
void SetClientNotify(ClientNotifyFn fn, void *context);

// start/stop watching 0x25 datagrams coming from serverIp:serverPort (host byte order)
// expectedPlayers: the mode's required players, checked against the roster size (0x25 total) the server reports
void ArmClient(uint32_t serverIp, uint16_t serverPort, uint32_t expectedPlayers);
void DisarmClient();

} // namespace AcceptTest
