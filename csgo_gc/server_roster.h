#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// srcds side of the backend-driven roster (RESEARCH_FINDINGS.md #55).
//
// The Java matchmaking backend is the source of truth for who takes part in a test match: the real players and the
// virtual (fake) participants of its Fake Players tool. A dedicated server started with -gc_mode asks it once a second
// (GET /api/v1/servers/roster for its own address:port), arms its reservation with exactly that roster and, when every
// fake participant is confirmed at reservation stage 1, tells the backend (POST /api/v1/servers/roster/ready). Only then
// does the backend give the players their server, so the retail reservation check succeeds on the first try.
//
// The old srcds-local roster (AcceptTest::FakeRoster fed by the first sniffed 0x21) stays as the fallback: backend not
// reachable / no match for this server / a roster whose size is not the mode's.
namespace RosterFeed
{

// what the backend said about this server at one poll
struct Snapshot
{
    enum class State
    {
        NoMatch,     // the backend answered: no forming / complete match on this server
        Match,       // a match with a roster
        Unavailable, // backend not reachable / refused (the reason is in `message`)
    };

    State state{ State::NoMatch };
    std::string message;
    std::string matchId;
    std::string mode;
    std::string status;           // FORMING / READY
    bool acceptRequired{};
    uint32_t requiredPlayers{};
    std::vector<uint32_t> participants; // roster order
    uint32_t fakeCount{};

    bool Complete() const { return state == State::Match && status == "READY"; }
};

// text form for handing a snapshot to the GC thread (GCEvent payload)
std::string Serialize(const Snapshot &snapshot);
bool Deserialize(const std::string &text, Snapshot &snapshot);

// The decision logic of a srcds that reads its roster from the backend. Pure state machine, no threads, no engine: the
// GC thread feeds it the poller's answers and the fake driver's readiness and it calls back what to do. It is a class of
// its own so the whole decision table is tested offline (RESEARCH_FINDINGS.md #55).
//
//   * a COMPLETE match (READY) whose roster has the mode's size and a real player -> arm exactly that roster
//     (replacing another one first), and once every fake participant is at stage 1 tell the backend "armed";
//   * anything else (no match, backend down, a roster of another size, e.g. required-players=1) -> the legacy roster
//     (first sniffed 0x21) stays in charge, and the backend is told right away that there is nothing to wait for;
//   * a roster change is postponed while a player is on the server;
//   * an armed backend match that the backend no longer has (it answers "no match on this server": the Accept timed out or
//     somebody cancelled it) is released: the reservation is dropped, nothing is kept alive, the next match arms afresh.
//     Not while a player is on the server, and never for the legacy roster or when the backend is merely unreachable.
class Controller
{
public:
    struct Host
    {
        // arm the reservation with this roster (roster order); unreserveFirst: another roster is armed at the moment
        std::function<void(const std::vector<uint32_t> &participants, const std::string &source, bool unreserveFirst)> arm;
        // "the roster of this match is armed" (or "nothing to arm, hand the server out")
        std::function<void(const std::string &matchId)> confirm;
        // the backend has no match on this server any more (cancelled at the Accept, ended) although one was armed for it and
        // nobody is on the server: drop the reservation and stop keeping it alive (RESEARCH_FINDINGS.md #63)
        std::function<void(const std::string &matchId)> release;
        std::function<bool()> playerOnServer;
    };

    Controller(std::string modeName, uint32_t requiredPlayers, Host host);

    void OnSnapshot(const Snapshot &snapshot);           // the poller's answer changed
    void OnFakesReady(bool ready);                       // the fake driver: every fake at stage 1 / (re)arming
    void OnLegacyArmed(const std::vector<uint32_t> &participants); // the legacy roster was armed (first sniffed 0x21)
    void OnPlayersChanged();                             // a player left the server: a postponed roster may apply now

    bool UsingBackendRoster() const { return m_usable; } // a complete, fitting roster of the backend is (to be) armed
    bool Armed() const { return m_armed; }
    bool InArmedRoster(uint32_t accountId) const;
    const std::string &ArmedSource() const { return m_armedSource; }
    const std::string &MatchId() const { return m_snapshot.matchId; }
    const std::string &ArmedMatchId() const { return m_armedMatchId; }

private:
    void Apply();
    void ConfirmIfReady();

    const std::string m_modeName;
    const uint32_t m_requiredPlayers;
    const Host m_host;

    Snapshot m_snapshot;
    bool m_usable{};
    bool m_armed{};
    std::vector<uint32_t> m_armedParticipants;
    std::string m_armedSource;
    bool m_fakesReady{};
    std::string m_confirmedMatchId;
    std::string m_armedMatchId;      // the backend match the armed roster belongs to
    bool m_armedFromBackend{};       // the armed roster is a backend match's (not the legacy one)
};

// One worker thread. The handler is called on it whenever the answer changes (not on every poll).
class Poller
{
public:
    using Handler = std::function<void(const Snapshot &snapshot)>;

    Poller(std::string address, uint16_t port, Handler handler);
    ~Poller(); // stops and joins

    Poller(const Poller &) = delete;
    Poller &operator=(const Poller &) = delete;

    // "the roster of this match is armed": POSTed to the backend by the worker thread (retried a few times)
    void Confirm(const std::string &matchId);

private:
    void Run();

    const std::string m_address;
    const uint16_t m_port;
    const Handler m_handler;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stopping{ false };
    bool m_wake{ false }; // a new confirmation arrived: do not wait out the poll interval
    std::vector<std::pair<std::string, int>> m_confirms; // match id, attempts so far
    std::thread m_thread;
};

} // namespace RosterFeed
