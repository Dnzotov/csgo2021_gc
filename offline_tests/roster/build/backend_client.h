#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Client of the Java matchmaking backend (java-backend/, RESEARCH_FINDINGS.md #47/#48/#49).
//
// The backend decides which dedicated server a search gets; the GC only reports the search and waits for the answer:
//   POST <backend_url>/api/v1/matchmaking/search       {account_id, game_type, mode, game_mode, maps, request_id}
//   GET  <backend_url>/api/v1/matchmaking/search/<id>  polled once a second while the search runs (also its heartbeat)
//   POST <backend_url>/api/v1/matchmaking/cancel       {account_id, request_id}
//   POST <backend_url>/api/v1/matchmaking/accepted     {account_id, request_id}   "the game server said everybody accepted"
// all with the X-Api-Key header from matchmaking.backend_api_key.
//
// Nothing here ever blocks the GC thread or the game: the calls below only update a small state and wake a dedicated
// worker thread, which does the HTTP with short timeouts (connect 1.5 s, whole request 3 s). Backend down / slow /
// answering an error is retried by the worker and logged as [BACKEND]; the results the GC is interested in come back
// through the result handler, called on the worker thread (the GC handler just posts an event to its own thread).
namespace BackendClient
{

struct SearchInfo
{
    uint32_t accountId{};       // ISteamUser::GetSteamID() & 0xFFFFFFFF
    uint32_t gameType{};        // MatchmakingStart.game_type as received: eGame | (mapMask << 8)
    std::string mode;           // MM::GameMode::name, e.g. "competitive"
    std::string gameMode;       // MM::GameMode::serverGameMode, e.g. "scrimcomp2v2"
    std::vector<std::string> maps; // decoded map selection, empty if the mode has none

    // The other members of the lobby (MatchmakingStart.account_ids without our own): only the leader's client sends a search,
    // the backend keeps one for every member and their GCs find it (WatchAccount), RESEARCH_FINDINGS.md #65
    std::vector<uint32_t> partyAccountIds;

    // Skirmish (War Games) only: the client selects modes, not maps. One entry per selected mode with the maps of its
    // group; the backend serves each by the category that fits (armsrace / demolition / skirmish), RESEARCH_FINDINGS.md #54
    struct Variant
    {
        std::string name;               // "armsrace", "demolition", "retakes", ...
        std::string gameMode;           // srcds game_mode from items_game.txt: "gungameprogressive", ...
        std::vector<std::string> maps;
    };
    std::vector<Variant> variants;
};

// the server the backend picked: what the existing 9107 / Accept / QueueConnect flow needs
struct Assignment
{
    std::string matchId;
    std::string serverAddress;  // IPv4 literal
    uint16_t serverPort{};
    std::string map;
    bool acceptRequired{};
    uint32_t requiredPlayers{};

    // Who is in the match (RESEARCH_FINDINGS.md #66): the real accounts, the same list the srcds arms as its roster. Every one
    // of them has to get its own Match Found (its own GC's 9107) and accept; nobody connects before all of them did.
    std::vector<uint32_t> realAccounts;
    uint32_t realPlayers{};
    uint32_t fakePlayers{};
};

struct SearchResult
{
    std::string requestId;
    // SEARCHING, MATCHED (a match is gathering players, no server yet), WAITING_ACCEPT, READY_TO_CONNECT (both carry an
    // assignment), CANCELLED, EXPIRED, REMOVED, COMPLETED (the backend ended the search). A search that had a WAITING_ACCEPT
    // assignment and is SEARCHING / MATCHED again lost its match: the Accept timed out or somebody cancelled it (#63).
    std::string status;
    uint32_t matchPlayers{};    // MATCHED: players gathered so far
    uint32_t matchRequired{};
    uint32_t matchRealPlayers{};     // the real players of the match that have to accept ...
    uint32_t matchAcceptedPlayers{}; // ... and how many of them did (READY_TO_CONNECT = all of them)
    bool hasAssignment{};
    Assignment assignment;

    // ---- a search of a party MEMBER (the leader's client sent it): what the GC needs to follow it like its own search ----
    std::string mode;           // "competitive", ...
    uint32_t gameType{};        // the leader's MatchmakingStart.game_type
    bool partyMember{};         // the backend's search of this account belongs to a party (party_leader_id)
    bool discovered{};          // not a poll result: WatchAccount found the search and now tracks it (delivered first)

    bool IsAssigned() const { return hasAssignment && (status == "WAITING_ACCEPT" || status == "READY_TO_CONNECT"); }
    bool IsEnded() const
    {
        return status == "CANCELLED" || status == "EXPIRED" || status == "REMOVED" || status == "COMPLETED";
    }
};

// text form for handing a result to the GC thread (GCEvent payload)
std::string SerializeResult(const SearchResult &result);
bool DeserializeResult(std::string_view text, SearchResult &result);

// called on the worker thread whenever the state of the search changes (status change, assignment). nullptr removes it
// and waits for a call that is in flight, so the context may be destroyed afterwards.
using ResultHandler = void (*)(void *context, const SearchResult &result);
void SetResultHandler(ResultHandler handler, void *context);

// false when matchmaking.backend_url is empty / invalid: no search can be served then
bool Enabled();

// a search started (9101). steamId only makes the request id unique. Replaces the search being tracked, if any.
void SearchStarted(const SearchInfo &info, uint64_t steamId);

// the tracked search stopped (9102). Does nothing if no search is being tracked.
void SearchCancelled();

// request id of the search being tracked, empty when there is none (used to drop results of an older search)
std::string ActiveRequestId();

// The game server reported that everybody accepted (reservation stage 2, awaiting 0): tell the backend so it moves the match
// from ACCEPTING to ACCEPTED (RESEARCH_FINDINGS.md #63). Retried a few times, the backend answer is only logged. The search
// keeps being polled: the backend, not the game server, decides when EVERY real player accepted - the search turning
// READY_TO_CONNECT is the signal to connect (#65). Does nothing without a tracked search.
void ReportAccepted();

// the client connects now: no need to watch the search any more
void StopPolling();

// A party member's client never sends a search: the leader's does. From now on the worker asks the backend once a second
// whether this account has a search (GET /matchmaking/account/<id>) whenever it tracks none of its own; a search that belongs
// to a party is adopted: it is tracked like a search this client started, and the result handler gets it first with
// `discovered` set (mode / game_type inside) so the GC can set up the search state.
void WatchAccount(uint32_t accountId);

// ---- srcds side: the roster of the match on this game server (RESEARCH_FINDINGS.md #55) ----
// The backend is the source of truth for who takes part in a test match: the real players and the virtual (fake)
// participants of its Fake Players tool. A dedicated server asks for it and arms its reservation with exactly that.
//   GET  <backend_url>/api/v1/servers/roster?address=<ip>&port=<port>        the newest forming / complete match on it
//   POST <backend_url>/api/v1/servers/roster/ready {address, port, match_id}  "the roster is armed": only then does the
//                                                                              backend hand the server to the players
// Both BLOCK for up to 3 s (connect 1.5 s): call them from a worker thread, never from the game thread.
struct RosterPlayer
{
    uint32_t accountId{};
    bool fake{};
};

struct ServerRoster
{
    std::string matchId;
    std::string mode;           // "competitive", ...
    std::string status;         // FORMING (still gathering real players) / READY (complete)
    std::string map;
    bool acceptRequired{};
    uint32_t requiredPlayers{};
    std::vector<RosterPlayer> players; // roster order: real players first, then the fake ones
};

enum class RosterResult
{
    Ok,
    NoMatch,     // the backend answered, this server has no forming / complete match (or is not registered)
    Unreachable, // no answer (backend down, not configured, ...), message says why
    Rejected,    // the backend refused (wrong api key, bad request), message says why
};

RosterResult FetchServerRoster(const std::string &address, uint16_t port, ServerRoster &roster, std::string &message);
bool ConfirmServerRoster(const std::string &address, uint16_t port, const std::string &matchId, std::string &message);

} // namespace BackendClient
