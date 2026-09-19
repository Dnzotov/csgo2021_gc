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
};

struct SearchResult
{
    std::string requestId;
    // SEARCHING, MATCHED (server reserved, players missing), WAITING_ACCEPT, READY_TO_CONNECT (both carry an
    // assignment), CANCELLED, EXPIRED, REMOVED, COMPLETED (the backend ended the search)
    std::string status;
    uint32_t matchPlayers{};    // MATCHED: players gathered so far
    uint32_t matchRequired{};
    bool hasAssignment{};
    Assignment assignment;

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
