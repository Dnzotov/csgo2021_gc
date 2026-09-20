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
    std::vector<std::string> releases;
    bool playerOnServer{};
    std::unique_ptr<RosterFeed::Controller> c;

    Rig(const char *mode = "competitive", uint32_t required = 10)
    {
        RosterFeed::Controller::Host host;
        host.arm = [this](const std::vector<uint32_t> &p, const std::string &s, bool r) { arms.push_back({ p, s, r }); };
        host.confirm = [this](const std::string &m) { confirms.push_back(m); };
        host.release = [this](const std::string &m) { releases.push_back(m); };
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

    printf("== a roster that is not an Accept match of this srcds' mode is not armed and the players are not held\n");
    {
        Rig r;
        r.c->OnSnapshot(Match("m-1", "READY", "competitive", Roster({ REAL }, 0), false));   // no Accept
        Expect(r.arms.empty() && r.confirms == std::vector<std::string>{ "m-1" } && !r.c->UsingBackendRoster(), "no Accept: legacy roster, confirmed at once");
        r.c->OnSnapshot(Match("m-1", "READY", "competitive", Roster({ REAL }, 0), false));
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

    printf("== #66/#67: the backend's roster is armed as it is, whatever its size (the Fake Players profile decides the fake count)\n");
    {
        const uint32_t B = 0x3e9846d6u, C = 0x3e9846d7u;

        // 2 real + 2 fake on a competitive srcds (10): the size does not fit, but the legacy roster would drop the 2nd player
        Rig r;
        auto roster = Roster({ REAL, B }, 2);
        r.c->OnSnapshot(Match("m-5", "READY", "competitive", roster));
        Expect(r.arms.size() == 1 && r.arms[0].participants == roster, "2 real players in a roster of another size: armed as it is");
        Expect(r.c->UsingBackendRoster(), "the backend roster is in charge (the legacy one is not armed on the first 0x21)");
        Expect(r.c->InArmedRoster(REAL) && r.c->InArmedRoster(B), "both real players are in the armed roster");
        Expect(r.confirms.empty(), "not confirmed before the fake players are at stage 1");
        r.c->OnFakesReady(true);
        Expect(r.confirms == std::vector<std::string>{ "m-5" }, "confirmed once they are");

        // 3 real players of a roster that has the right size are armed as before
        Rig s3;
        auto full = Roster({ REAL, B, C }, 7);
        s3.c->OnSnapshot(Match("m-6", "READY", "competitive", full));
        Expect(s3.arms.size() == 1 && s3.arms[0].participants == full, "3 real + 7 fake: armed");

        // the wrong srcds mode and no-Accept matches still are not armed, however many real players
        Rig w("wingman", 4);
        w.c->OnSnapshot(Match("m-7", "READY", "competitive", Roster({ REAL, B }, 8)));
        Expect(w.arms.empty(), "2 real players of a competitive match on a wingman srcds: not armed");
        Rig k;
        k.c->OnSnapshot(Match("m-8", "READY", "competitive", Roster({ REAL, B }, 2), false));
        Expect(k.arms.empty(), "2 real players of a match that needs no Accept: not armed");

        // ONE real player + 3 configured fake on a 10 player mode: the configured 4 players are armed, not a legacy roster of 10
        Rig one;
        auto four = Roster({ REAL }, 3);
        one.c->OnSnapshot(Match("m-9", "READY", "competitive", four));
        Expect(one.arms.size() == 1 && one.arms[0].participants == four && one.confirms.empty(), "1 real + 3 fake: a roster of 4 is armed");
        Expect(one.c->UsingBackendRoster(), "and it is the backend's");

        // 3 real + 2 fake = 5 of a capacity of 10, and 3 real + 0 fake = 3: exactly the configured numbers
        Rig five;
        auto r5 = Roster({ REAL, B, C }, 2);
        five.c->OnSnapshot(Match("m-10", "READY", "competitive", r5));
        Expect(five.arms.size() == 1 && five.arms[0].participants == r5 && five.arms[0].participants.size() == 5, "3 real + 2 fake: a roster of 5");
        Rig none;
        auto r3 = Roster({ REAL, B, C }, 0);
        none.c->OnSnapshot(Match("m-11", "READY", "competitive", r3));
        Expect(none.arms.size() == 1 && none.arms[0].participants == r3, "3 real + 0 fake: a roster of 3 (nothing is added)");
        none.c->OnFakesReady(true);
        Expect(none.confirms == std::vector<std::string>{ "m-11" }, "confirmed as soon as the driver says it is ready (no fake to wait for)");
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

    printf("== the backend cancelled the match (Accept timeout): the armed reservation is released, once\n");
    {
        Rig r;
        auto roster = Roster({ REAL }, 9);
        r.c->OnSnapshot(Match("m-1", "READY", "competitive", roster));
        r.c->OnFakesReady(true);
        Snapshot none; none.state = Snapshot::State::NoMatch; none.message = "no match on this server";
        r.c->OnSnapshot(none);
        Expect(r.releases == std::vector<std::string>{ "m-1" }, "NoMatch after an armed backend match: released, naming that match");
        Expect(!r.c->Armed() && !r.c->UsingBackendRoster() && r.c->ArmedMatchId().empty(), "nothing is armed any more");
        r.c->OnSnapshot(none);
        r.c->OnPlayersChanged();
        Expect(r.releases.size() == 1, "polling NoMatch again (or a player event) releases nothing more");

        // the next match (same players again) arms afresh: nothing to unreserve first, confirmed only when the fakes are ready
        r.c->OnSnapshot(Match("m-2", "READY", "competitive", roster));
        Expect(r.arms.size() == 2 && !r.arms[1].replacing && r.arms[1].participants == roster, "the next match arms afresh, replacing nothing");
        Expect(r.confirms == std::vector<std::string>{ "m-1" }, "and is not confirmed before its fakes are ready");
        r.c->OnFakesReady(true);
        Expect(r.confirms == std::vector<std::string>({ "m-1", "m-2" }), "confirmed when they are");
    }

    printf("== ... but never while a player is on the server, for the legacy roster, or when the backend is just unreachable\n");
    {
        Rig r;
        r.c->OnSnapshot(Match("m-1", "READY", "competitive", Roster({ REAL }, 9)));
        r.playerOnServer = true;
        Snapshot none; none.state = Snapshot::State::NoMatch;
        r.c->OnSnapshot(none);
        Expect(r.releases.empty() && r.c->Armed(), "a player is on the server: the match is being played, kept");
        r.playerOnServer = false;
        r.c->OnPlayersChanged();
        Expect(r.releases == std::vector<std::string>{ "m-1" }, "released once the player left and the backend still has no match");

        Rig l;
        l.c->OnLegacyArmed(Roster({ REAL }, 9));
        l.c->OnSnapshot(none);
        Expect(l.releases.empty() && l.c->Armed(), "the legacy roster is not the backend's to take away");

        Rig u;
        u.c->OnSnapshot(Match("m-1", "READY", "competitive", Roster({ REAL }, 9)));
        Snapshot down; down.state = Snapshot::State::Unavailable;
        u.c->OnSnapshot(down);
        Expect(u.releases.empty() && u.c->Armed(), "backend unreachable: nothing is released");
    }

    printf("== a new match with the same roster while the old one is armed is a new match, not a release\n");
    {
        Rig r;
        auto roster = Roster({ REAL }, 9);
        r.c->OnSnapshot(Match("m-1", "READY", "competitive", roster));
        r.c->OnFakesReady(true);
        r.c->OnSnapshot(Match("m-2", "READY", "competitive", roster));
        Expect(r.releases.empty() && r.c->ArmedMatchId() == "m-2", "no NoMatch in between: kept, and it belongs to the new match now");
        Snapshot none; none.state = Snapshot::State::NoMatch;
        r.c->OnSnapshot(none);
        Expect(r.releases == std::vector<std::string>{ "m-2" }, "and it is the new match that is released when that goes");
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
