// Decision table of RosterFeed::Controller (the srcds side of the backend-driven roster), RESEARCH_FINDINGS.md #55.
// The REAL server_roster.cpp; the host (FakeRoster / poller / engine) is replaced by a recorder.
#include "stdafx.h"
#include "config.h"
#include "server_roster.h"

#include <cstdarg>

void Platform::Print(const char *format, ...)
{
    va_list ap; va_start(ap, format); printf("      | "); vprintf(format, ap); va_end(ap);
}
std::string Platform::CommandLine() { return {}; }
const GCConfig &GetConfig() { static GCConfig c; return c; }
GCConfig::GCConfig() {}

static int g_failed = 0;
static void Expect(bool ok, const char *what)
{
    printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    g_failed += ok ? 0 : 1;
}

using RosterFeed::Snapshot;

static const uint32_t REAL = 0x3e9846d5u;

static std::vector<uint32_t> Roster(std::initializer_list<uint32_t> real, int fake)
{
    std::vector<uint32_t> ids(real);
    for (int i = 1; i <= fake; i++) ids.push_back(0xFA4E0000u + i);
    return ids;
}

static Snapshot Match(const char *id, const char *status, const char *mode, std::vector<uint32_t> players, bool accept = true)
{
    Snapshot s;
    s.state = Snapshot::State::Match;
    s.matchId = id; s.status = status; s.mode = mode; s.acceptRequired = accept;
    s.requiredPlayers = (uint32_t)players.size();
    s.participants = players;
    for (uint32_t p : players) s.fakeCount += (p & 0xFFFF0000u) == 0xFA4E0000u ? 1 : 0;
    return s;
}

struct Rig
{
    struct Arm { std::vector<uint32_t> participants; std::string source; bool replacing; };
    std::vector<Arm> arms;
    std::vector<std::string> confirms;
    bool playerOnServer{};
    std::unique_ptr<RosterFeed::Controller> c;

    Rig(const char *mode = "competitive", uint32_t required = 10)
    {
        RosterFeed::Controller::Host host;
        host.arm = [this](const std::vector<uint32_t> &p, const std::string &s, bool r) { arms.push_back({ p, s, r }); };
        host.confirm = [this](const std::string &m) { confirms.push_back(m); };
        host.playerOnServer = [this] { return playerOnServer; };
        c = std::make_unique<RosterFeed::Controller>(mode, required, std::move(host));
    }
};

int main()
{
    printf("== no match / backend down: nothing is armed, the legacy roster stays in charge\n");
    {
        Rig r;
        Snapshot none; none.state = Snapshot::State::NoMatch;
        r.c->OnSnapshot(none);
        Snapshot down; down.state = Snapshot::State::Unavailable;
        r.c->OnSnapshot(down);
        Expect(r.arms.empty() && r.confirms.empty() && !r.c->UsingBackendRoster() && !r.c->Armed(), "no arm, no confirm, backend roster not in use");
    }

    printf("== a match that is still gathering real players is not armed\n");
    {
        Rig r;
        r.c->OnSnapshot(Match("m-1", "FORMING", "competitive", Roster({ REAL }, 5)));
        Expect(r.arms.empty() && r.confirms.empty() && !r.c->UsingBackendRoster(), "FORMING: nothing happens");
    }

    printf("== a complete roster of the mode's size is armed as it is, and confirmed once every fake is at stage 1\n");
    {
        Rig r;
        auto roster = Roster({ REAL }, 9);
        r.c->OnSnapshot(Match("m-1", "READY", "competitive", roster));
        Expect(r.arms.size() == 1 && r.arms[0].participants == roster, "armed with exactly the backend's 10 participants (real id first, fake ids 1..9)");
        Expect(r.arms[0].participants[0] == REAL && r.arms[0].participants[9] == 0xFA4E0009u, "the real AccountID and the fake ones come from the backend");
        Expect(!r.arms[0].replacing && r.arms[0].source == "backend match m-1", "first arm: nothing to unreserve, source names the match");
        Expect(r.c->UsingBackendRoster() && r.c->Armed(), "the backend roster is in use");
        Expect(r.confirms.empty(), "not confirmed before the fakes are at stage 1");
        r.c->OnFakesReady(true);
        Expect(r.confirms == std::vector<std::string>{ "m-1" }, "confirmed after every fake is at stage 1");
        r.c->OnFakesReady(true);
        r.c->OnSnapshot(Match("m-1", "READY", "competitive", roster));
        Expect(r.arms.size() == 1 && r.confirms.size() == 1, "polling the same match again changes nothing (no second arm, no second confirm)");
    }

    printf("== the same players search again: the armed roster is kept, the new match is confirmed\n");
    {
        Rig r;
        auto roster = Roster({ REAL }, 9);
        r.c->OnSnapshot(Match("m-1", "READY", "competitive", roster));
        r.c->OnFakesReady(true);
        r.c->OnSnapshot(Match("m-2", "READY", "competitive", roster));
        Expect(r.arms.size() == 1, "no re-arm for an identical roster");
        Expect(r.confirms == std::vector<std::string>({ "m-1", "m-2" }), "the new match is confirmed at once (the fakes are ready already)");
    }

    printf("== a different roster replaces the armed one\n");
    {
        Rig r;
        r.c->OnSnapshot(Match("m-1", "READY", "competitive", Roster({ REAL }, 9)));
        r.c->OnFakesReady(true);
        auto other = Roster({ REAL, 0x11111111u }, 8);
        r.c->OnSnapshot(Match("m-2", "READY", "competitive", other));
        Expect(r.arms.size() == 2 && r.arms[1].participants == other && r.arms[1].replacing, "re-armed with the new roster, the old reservation is unreserved first");
        Expect(r.confirms.size() == 1, "not confirmed until the new fakes are ready");
        r.c->OnFakesReady(false);
        r.c->OnFakesReady(true);
        Expect(r.confirms == std::vector<std::string>({ "m-1", "m-2" }), "confirmed when they are");
    }

    printf("== a roster change waits while a player is on the server\n");
    {
        Rig r;
        r.c->OnSnapshot(Match("m-1", "READY", "competitive", Roster({ REAL }, 9)));
        r.playerOnServer = true;
        auto other = Roster({ 0x22222222u }, 9);
        r.c->OnSnapshot(Match("m-2", "READY", "competitive", other));
        Expect(r.arms.size() == 1, "postponed");
        r.playerOnServer = false;
        r.c->OnPlayersChanged();
        Expect(r.arms.size() == 2 && r.arms[1].participants == other, "applied when the player left");
    }

    printf("== a roster that does not fit the srcds is not armed and the players are not held\n");
    {
        Rig r;
        r.c->OnSnapshot(Match("m-1", "READY", "competitive", Roster({ REAL }, 0)));        // required-players=1 override: 1 participant
        Expect(r.arms.empty() && r.confirms == std::vector<std::string>{ "m-1" } && !r.c->UsingBackendRoster(), "1 participant of 10: legacy roster, confirmed at once");
        r.c->OnSnapshot(Match("m-1", "READY", "competitive", Roster({ REAL }, 0)));
        Expect(r.confirms.size() == 1, "confirmed once only");

        Rig w("wingman", 4);
        w.c->OnSnapshot(Match("m-2", "READY", "competitive", Roster({ REAL }, 9)));
        Expect(w.arms.empty() && w.confirms.size() == 1, "a competitive roster on a wingman srcds: not armed");

        Rig n;
        n.c->OnSnapshot(Match("m-3", "READY", "competitive", Roster({}, 10)));
        Expect(n.arms.empty() && n.confirms.size() == 1, "ten fake and no real player: not armed");

        Rig k;
        k.c->OnSnapshot(Match("m-4", "READY", "competitive", Roster({ REAL }, 9), false));
        Expect(k.arms.empty() && k.confirms.size() == 1, "a match that needs no Accept: not armed");
    }

    printf("== the legacy roster (first sniffed 0x21) and the backend agree\n");
    {
        Rig r;
        auto roster = Roster({ REAL }, 9);
        r.c->OnLegacyArmed(roster);
        Expect(r.c->Armed() && r.c->InArmedRoster(REAL) && !r.c->InArmedRoster(0x99999999u), "the legacy roster is remembered");
        r.c->OnFakesReady(true);
        r.c->OnSnapshot(Match("m-1", "READY", "competitive", roster));
        Expect(r.arms.empty() && r.confirms == std::vector<std::string>{ "m-1" }, "an identical backend roster is adopted: no re-arm, confirmed at once");
        Expect(r.c->ArmedSource() == "backend match m-1", "and it is the backend's from now on");
    }

    printf("== the backend goes away after the roster was armed\n");
    {
        Rig r;
        r.c->OnSnapshot(Match("m-1", "READY", "competitive", Roster({ REAL }, 9)));
        Snapshot down; down.state = Snapshot::State::Unavailable;
        r.c->OnSnapshot(down);
        Expect(r.c->Armed() && !r.c->UsingBackendRoster() && r.confirms.empty(), "stays armed, the backend roster is no longer 'in use', nothing is confirmed");
    }

    printf("== the poller's snapshot survives the trip to the GC thread\n");
    {
        Snapshot s = Match("m-9", "READY", "competitive", Roster({ REAL, 0x11111111u }, 8));
        s.message = "line one\nline two";
        Snapshot back;
        Expect(RosterFeed::Deserialize(RosterFeed::Serialize(s), back) && back.matchId == "m-9" && back.participants == s.participants
            && back.fakeCount == 8 && back.Complete() && back.requiredPlayers == 10 && back.acceptRequired, "serialize / deserialize keeps the roster");
        Snapshot no; no.state = Snapshot::State::NoMatch; no.message = "no forming or ready match";
        Expect(RosterFeed::Deserialize(RosterFeed::Serialize(no), back) && back.state == Snapshot::State::NoMatch && back.participants.empty(), "and an empty answer");
        Expect(!RosterFeed::Deserialize("garbage", back), "garbage is refused");
    }

    printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "ALL PASSED", g_failed);
    return g_failed ? 1 : 0;
}
