// Offline test of reservation_keepalive.h (RESEARCH_FINDINGS.md #60): the lifecycle of the plain reservation of a classic
// dedicated server. The real header is compiled; the engine is a small model of CBaseServer's reservation rules
// (baseserver.cpp ReserveServerForQueuedGame / sv_main.cpp UpdateHibernationState) and the clock is a variable.
#include "reservation_keepalive.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace ReservationKeepAlive;
using std::chrono::milliseconds;
using std::chrono::seconds;

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

// The engine's rules that matter here: bReserve=1 with the same cookie (or while unreserved) arms the expiry
// (sv_mmqueue_reservation_timeout = 21 s); an empty reserved server drops the cookie once the expiry has passed and
// sv_hibernate_postgame_delay (5 s) is over; bReserve=0 zeroes the expiry (Unreserve).
struct EngineModel
{
    static constexpr double Timeout = 21.0;
    static constexpr double PostgameDelay = 5.0;

    uint64_t cookie{};
    double expiry{};
    double emptySince{ -1.0 };
    bool clients{};
    bool refuse{}; // sv_ShutDown_WasRequested()

    std::vector<std::string> payloads;
    std::vector<bool> verbose;

    bool Reserve(const std::string &payload, bool isVerbose, double now)
    {
        payloads.push_back(payload);
        verbose.push_back(isVerbose);
        if (refuse)
        {
            return false;
        }

        unsigned long long c = 0, m = 0;
        int reserve = 0;
        sscanf(payload.c_str() + 1, "%llx,%llx,%d:", &c, &m, &reserve);
        if (!c || !m)
        {
            return false;
        }

        if (reserve)
        {
            if (!cookie || cookie == c)
            {
                expiry = now + Timeout;
                cookie = c;
                return true;
            }

            return false;
        }

        if (cookie == c)
        {
            expiry = 0;
            return true;
        }

        return !cookie;
    }

    // UpdateHibernationState, called every frame
    void Frame(double now)
    {
        if (cookie && !clients)
        {
            if (emptySince < 0)
            {
                emptySince = now;
            }

            if (now - emptySince > PostgameDelay && (expiry == 0.0 || expiry < now))
            {
                cookie = 0;
            }
        }
        else
        {
            emptySince = -1.0;
        }
    }

    // ReplyReservationCheckRequest for a plain 'G' reservation: awaiting 0 only while our cookie is reserved
    int Awaiting(uint64_t clientCookie) const { return (cookie && cookie == clientCookie) ? 0 : 127; }
};

constexpr uint64_t Cookie = 0x293A206F6C6C6548ull; // GameServerCookieId

struct Harness
{
    EngineModel engine;
    double t{ 0 };
    Clock::time_point base{ Clock::time_point{} + seconds(1000) };
    std::vector<std::string> log;
    Lease lease;

    explicit Harness(Lease::Settings settings = {})
        : lease{ [this](const std::string &line) { log.push_back(line); }, settings }
    {
    }

    Clock::time_point At(double seconds_) const { return base + milliseconds(static_cast<long long>(seconds_ * 1000)); }

    Reserve Engine()
    {
        return [this](const std::string &payload, bool verbose) { return engine.Reserve(payload, verbose, t); };
    }

    // what the srcds main thread does: one frame (~16 ms), engine frame first, then the pump
    void Run(double seconds_, double step = 0.016)
    {
        const double end = t + seconds_;
        while (t < end)
        {
            t += step;
            engine.Frame(t);
            lease.Tick(At(t), Engine());
        }
    }

    int Count(const char *needle) const
    {
        int n = 0;
        for (const std::string &line : log)
        {
            n += line.find(needle) != std::string::npos ? 1 : 0;
        }

        return n;
    }
};

static void TestReservationIsCreated()
{
    printf("1. the reservation is created (same payload as before)\n");
    Harness h;
    h.lease.Start(Cookie, h.At(0), h.Engine());

    CHECK(h.engine.payloads.size() == 1);
    // byte for byte what ServerGC::ReserveServerForOurCookie always sent
    CHECK(h.engine.payloads[0] == "G293a206f6c6c6548,293a206f6c6c6548,1:");
    CHECK(h.engine.verbose[0]); // the first reservation is logged like every other bridge call
    CHECK(h.engine.Awaiting(Cookie) == 0);
    CHECK(h.lease.state() == Lease::State::Active);
    CHECK(h.lease.cookie() == Cookie);
    CHECK(h.Count("keep-alive started cookie=293a206f6c6c6548") == 1);
}

static void TestKeepAliveAndInterval()
{
    printf("2/3. keep-alive starts, refresh happens at the chosen interval, nothing before\n");
    Harness h;
    h.lease.Start(Cookie, h.At(0), h.Engine());

    h.Run(7.9);
    CHECK(h.engine.payloads.size() == 1); // not yet
    h.Run(0.3);                            // t ~ 8.2
    CHECK(h.engine.payloads.size() == 2);
    CHECK(!h.engine.verbose[1]);           // refreshes are quiet in the bridge, the lease logs them itself
    h.Run(8.0);                            // ~16.2
    CHECK(h.engine.payloads.size() == 3);
    h.Run(43.8);                           // 60 s in total: refreshes at ~8, 16, 24, 32, 40, 48, 56
    CHECK(h.lease.refreshes() == 7);
    CHECK(h.engine.payloads.size() == 8);
    CHECK(h.Count("keep-alive refresh") == 7); // one line per refresh, not per frame

    // 8 s against the engine's 21 s window
    static_assert(RefreshSeconds == 8, "the interval FakeRoster shares");
    static_assert(RefreshSeconds * 2 < 21, "survives one lost refresh");
}

static void TestCookieStaysTheSame()
{
    printf("8. the cookie never changes, every payload is the same reservation\n");
    Harness h;
    h.lease.Start(Cookie, h.At(0), h.Engine());
    h.Run(120.0);

    bool same = true;
    for (const std::string &payload : h.engine.payloads)
    {
        same &= payload == "G293a206f6c6c6548,293a206f6c6c6548,1:";
    }

    CHECK(same);
    CHECK(h.engine.payloads.size() > 10);
    CHECK(h.engine.cookie == Cookie);
}

static void TestTheBugAndTheFix()
{
    printf("   the failure of #59: a one-shot reservation is gone after ~26 s, the kept-alive one is not\n");

    // before: one reservation, nothing else (what ServerGC did)
    {
        EngineModel oneShot;
        oneShot.Reserve(Payload(Cookie, true), true, 0);
        double t = 0;
        while (t < 40.0)
        {
            t += 0.016;
            oneShot.Frame(t);
        }

        CHECK(oneShot.Awaiting(Cookie) == 127); // stage 2 -> 0x25 awaiting=127 -> "Failed to connect to the match"
    }

    // now: the search comes 40 s / 10 min after the server started, on an idle server
    Harness h;
    h.lease.Start(Cookie, h.At(0), h.Engine());
    h.Run(40.0);
    CHECK(h.engine.Awaiting(Cookie) == 0);
    h.Run(560.0); // 10 min
    CHECK(h.engine.Awaiting(Cookie) == 0);
}

static void TestNoSecondReservation()
{
    printf("4. the keep-alive never creates a second, independent reservation\n");
    Harness h;
    h.lease.Start(Cookie, h.At(0), h.Engine());
    h.lease.Start(Cookie, h.At(0.5), h.Engine()); // e.g. a second ServerHello
    h.lease.Start(Cookie, h.At(1.0), h.Engine());
    CHECK(h.engine.payloads.size() == 1);
    CHECK(h.Count("keep-alive started") == 1);

    h.Run(16.5);
    // exactly one loop: 2 refreshes in 16.5 s (plus the start) although Start was called three times
    CHECK(h.engine.payloads.size() == 3);
}

static void TestStop()
{
    printf("5. stop really stops the refresh\n");
    Harness h;
    h.lease.Start(Cookie, h.At(0), h.Engine());
    h.Run(20.0);
    const size_t before = h.engine.payloads.size();
    CHECK(before == 3);

    h.lease.Stop(Lease::StopReason::Shutdown, h.At(h.t));
    CHECK(h.lease.state() == Lease::State::Stopped);
    CHECK(h.lease.reason() == Lease::StopReason::Shutdown);
    h.Run(300.0);
    CHECK(h.engine.payloads.size() == before); // nothing after the stop
    CHECK(h.Count("keep-alive stopped cookie=293a206f6c6c6548: server shutting down") == 1);

    h.lease.Stop(Lease::StopReason::Shutdown, h.At(h.t)); // idempotent: no second line
    CHECK(h.Count("keep-alive stopped") == 1);
}

static void TestIdleLease()
{
    printf("lifecycle: an unused server releases the reservation, activity renews the lease, activity re-arms it\n");
    Lease::Settings settings;
    settings.idleSeconds = 60;

    {
        Harness h{ settings };
        h.lease.Start(Cookie, h.At(0), h.Engine());
        h.Run(59.0);
        CHECK(h.lease.state() == Lease::State::Active);
        h.Run(2.0);
        CHECK(h.lease.state() == Lease::State::Stopped);
        CHECK(h.lease.reason() == Lease::StopReason::Idle);

        // the reservation is released explicitly (Unreserve) and the engine drops the cookie by itself
        CHECK(h.engine.payloads.back() == "G293a206f6c6c6548,293a206f6c6c6548,0:");
        h.Run(30.0);
        CHECK(h.engine.cookie == 0);
        CHECK(h.engine.Awaiting(Cookie) == 127);

        const size_t sent = h.engine.payloads.size();
        h.Run(600.0);
        CHECK(h.engine.payloads.size() == sent); // stopped stays stopped: no immediate re-arm, no refresh
        CHECK(h.lease.state() == Lease::State::Stopped);
        CHECK(h.Count("keep-alive stopped") == 1);
    }

    {
        // activity renews the lease: a client that came and went at t=50 keeps the reservation past t=60, until 50+60
        Harness h{ settings };
        h.lease.Start(Cookie, h.At(0), h.Engine());
        h.Run(50.0);
        h.lease.ClientJoined(h.At(h.t));
        h.lease.ClientLeft(h.At(h.t));
        h.Run(59.0);
        CHECK(h.lease.state() == Lease::State::Active);
        h.Run(2.0);
        CHECK(h.lease.state() == Lease::State::Stopped); // ~111 s: 60 s after the last activity
        CHECK(h.lease.reason() == Lease::StopReason::Idle);
        CHECK(h.Count("keep-alive stopped") == 1);
    }

    {
        // a client that shows up after the lease ran out re-arms it (the server is in use again)
        Harness h{ settings };
        h.lease.Start(Cookie, h.At(0), h.Engine());
        h.Run(70.0);
        CHECK(h.lease.state() == Lease::State::Stopped);
        h.lease.ClientJoined(h.At(h.t));
        h.Run(0.1);
        CHECK(h.lease.state() == Lease::State::Active);
        CHECK(h.Count("re-armed by client activity") == 1);
        CHECK(h.Count("keep-alive started") == 2);
        CHECK(h.engine.payloads.back().back() == ':');
        h.Run(30.0);
        CHECK(h.engine.Awaiting(Cookie) == 0); // reserved again and refreshed
    }

    {
        // idleSeconds = 0: no limit
        Lease::Settings unlimited;
        unlimited.idleSeconds = 0;
        Harness h{ unlimited };
        h.lease.Start(Cookie, h.At(0), h.Engine());
        h.Run(3 * 3600.0, 0.25);
        CHECK(h.lease.state() == Lease::State::Active);
        CHECK(h.engine.Awaiting(Cookie) == 0);
    }

    // the default is a finite lease
    static_assert(DefaultIdleSeconds == 30 * 60, "default idle lease");
    CHECK(Lease::Settings{}.idleSeconds != 0);
}

static void TestEngineRefuses()
{
    printf("lifecycle: the engine refuses the reservation (sv_shutdown requested, other cookie) -> the lease gives up\n");
    Harness h;
    h.lease.Start(Cookie, h.At(0), h.Engine());
    h.engine.refuse = true;
    h.Run(8.5);
    CHECK(h.lease.state() == Lease::State::Active); // 1 failed refresh
    CHECK(h.lease.failures() == 1);
    h.Run(8.0);
    CHECK(h.lease.state() == Lease::State::Active); // 2
    h.Run(8.0);
    CHECK(h.lease.state() == Lease::State::Stopped); // 3 in a row
    CHECK(h.lease.reason() == Lease::StopReason::Refused);

    const size_t sent = h.engine.payloads.size();
    h.Run(120.0);
    CHECK(h.engine.payloads.size() == sent);

    // a success in between resets the count
    Harness g;
    g.lease.Start(Cookie, g.At(0), g.Engine());
    g.engine.refuse = true;
    g.Run(8.5);
    g.Run(8.0);
    CHECK(g.lease.failures() == 2);
    g.engine.refuse = false;
    g.Run(8.0);
    CHECK(g.lease.failures() == 0);
    CHECK(g.lease.state() == Lease::State::Active);

    // the very first reservation failing (engine not resolved yet) is retried by the refresh
    Harness f;
    f.engine.refuse = true;
    f.lease.Start(Cookie, f.At(0), f.Engine());
    CHECK(f.lease.state() == Lease::State::Active);
    f.engine.refuse = false;
    f.Run(8.5);
    CHECK(f.engine.Awaiting(Cookie) == 0);
}

static void TestStartAfterStop()
{
    printf("a stopped lease can be started again\n");
    Harness h;
    h.lease.Start(Cookie, h.At(0), h.Engine());
    h.lease.Stop(Lease::StopReason::Shutdown, h.At(1));
    h.lease.Start(Cookie, h.At(2), h.Engine());
    CHECK(h.lease.state() == Lease::State::Active);
    CHECK(h.Count("keep-alive started") == 2);

    // Tick on a lease that never started does nothing
    Harness idle;
    idle.Run(100.0);
    CHECK(idle.engine.payloads.empty());
    CHECK(idle.lease.state() == Lease::State::Inactive);
}

static void TestLongMatch()
{
    printf("lifecycle: a match longer than the idle lease keeps the reservation, the lease counts from the last client leaving\n");
    Lease::Settings settings;
    settings.idleSeconds = 60;
    Harness h{ settings };
    h.lease.Start(Cookie, h.At(0), h.Engine());
    h.Run(10.0);

    h.lease.ClientJoined(h.At(h.t)); // BeginAuthSession
    h.engine.clients = true;
    h.Run(3 * 3600.0, 0.25);          // a three hour match, far beyond the 60 s lease
    CHECK(h.lease.state() == Lease::State::Active);
    CHECK(h.lease.clients() == 1);
    CHECK(h.engine.Awaiting(Cookie) == 0);

    h.lease.ClientLeft(h.At(h.t));    // EndAuthSession: the server is empty again
    h.engine.clients = false;
    CHECK(h.lease.clients() == 0);
    h.Run(59.0);
    CHECK(h.lease.state() == Lease::State::Active);
    CHECK(h.engine.Awaiting(Cookie) == 0); // the next search still finds a reserved server
    h.Run(2.0);
    CHECK(h.lease.state() == Lease::State::Stopped);
    CHECK(h.lease.reason() == Lease::StopReason::Idle);

    // EndAuthSession for a session that never got to ClientJoined does not push the count below zero
    Harness g;
    g.lease.Start(Cookie, g.At(0), g.Engine());
    g.lease.ClientLeft(g.At(1));
    g.lease.ClientLeft(g.At(2));
    CHECK(g.lease.clients() == 0);
    g.lease.ClientJoined(g.At(3));
    CHECK(g.lease.clients() == 1);
}

static void TestClientsFromAnotherThread()
{
    printf("client notifications from another thread while the host thread ticks\n");
    Harness h;
    h.lease.Start(Cookie, h.At(0), h.Engine());

    std::thread toucher(
        [&]
        {
            for (int i = 0; i < 20000; i++)
            {
                h.lease.ClientJoined(h.At(1.0));
                h.lease.ClientLeft(h.At(1.0));
            }
        });

    h.Run(30.0);
    toucher.join();
    CHECK(h.lease.state() == Lease::State::Active);
    CHECK(h.engine.Awaiting(Cookie) == 0);
}

int main()
{
    TestReservationIsCreated();
    TestKeepAliveAndInterval();
    TestCookieStaysTheSame();
    TestTheBugAndTheFix();
    TestNoSecondReservation();
    TestStop();
    TestIdleLease();
    TestEngineRefuses();
    TestStartAfterStop();
    TestLongMatch();
    TestClientsFromAnotherThread();

    printf("\n%d checks, %d failed\n%s\n", g_checks, g_failed, g_failed ? "FAILED" : "ALL PASSED");
    return g_failed ? 1 : 0;
}
