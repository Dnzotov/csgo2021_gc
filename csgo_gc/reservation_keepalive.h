#pragma once

// Server reservation lifecycle of a classic (non-Accept) dedicated server, RESEARCH_FINDINGS.md #59/#60.
//
// The plain 'G' reservation of the fixed GameServerCookieId is what makes a server answer the client's reservation
// check (A2S_RESERVE_CHECK, 0x21) with awaiting=0. The engine drops it by itself: ReserveServerForQueuedGame arms
// m_flReservationExpiryTime = now + sv_mmqueue_reservation_timeout (21 s, engine default), and an empty reserved
// server clears the cookie once that time has passed and sv_hibernate_postgame_delay (5 s) is over. Reserving once
// (k_EMsgGCServerHello) therefore leaves the server unreserved ~21-26 s later and every later search ends in
// 0x25 awaiting=127 / "Failed to connect to the match".
//
// Lease is the refresh: while it is Active the same reservation payload is sent again every RefreshSeconds. It has
// no thread and no clock of its own -- the host thread (srcds main thread, the same per-frame pump that already
// drains HostEvent::ReserveServerForQueuedGame) calls Start/Tick with the current time and the function that
// performs IVEngineServer::ReserveServerForQueuedGame, which is what makes it testable offline.
//
// Not for '-gc_mode' servers (their Q roster has its own keep-alive in AcceptTest::FakeRoster, which owns the
// reservation of such a server) and not for a listen server.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

namespace ReservationKeepAlive
{

using Clock = std::chrono::steady_clock;

// the engine timeout is 21 s (sv_mmqueue_reservation_timeout, allowed 5..180): one refresh every 8 s survives two lost
// refreshes and is the interval AcceptTest::FakeRoster already uses for its Q reservation
constexpr int RefreshSeconds = 8;

// how long an EMPTY server keeps its reservation after it started or its last client left: the server is still meant
// for matchmaking (the backend does not tell it otherwise), but not for ever -- an idle server is released and
// hibernates again. While a client is on the server the lease does not run out; a client joining re-arms a released
// one. matchmaking.reservation_idle_seconds, 0 = no limit.
constexpr uint32_t DefaultIdleSeconds = 30 * 60;

// refresh attempts the engine refuses in a row (sv_shutdown requested, reserved for another cookie, engine not
// resolved) before the lease gives up
constexpr int MaxConsecutiveFailures = 3;

// "G<cookie>,<cookie>,<reserve>:", the same string ServerGC always sent (the cookie doubles as match id)
inline std::string Payload(uint64_t cookie, bool reserve)
{
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "G%llx,%llx,%d:", static_cast<unsigned long long>(cookie),
        static_cast<unsigned long long>(cookie), reserve ? 1 : 0);
    return buffer;
}

// performs IVEngineServer::ReserveServerForQueuedGame(payload) on the calling (host) thread; verbose = log the call
using Reserve = std::function<bool(const std::string &payload, bool verbose)>;
using Log = std::function<void(const std::string &line)>;

class Lease
{
public:
    enum class State
    {
        Inactive, // never started
        Active,   // refreshing
        Stopped,  // see StopReason
    };

    enum class StopReason
    {
        None,
        Idle,     // empty for idleSeconds: the reservation is released (Unreserve) and expires
        Refused,  // the engine refused MaxConsecutiveFailures reservations in a row
        Shutdown, // the server is going away
    };

    struct Settings
    {
        int refreshSeconds{ RefreshSeconds };
        uint32_t idleSeconds{ DefaultIdleSeconds }; // 0 = never idle
        int maxFailures{ MaxConsecutiveFailures };
    };

    explicit Lease(Log log = {}, Settings settings = {})
        : m_log{ std::move(log) }
        , m_settings{ settings }
    {
    }

    // ---- host thread only ----------------------------------------------------------------------------------------

    // reserve now and keep the reservation alive. A second Start with the cookie that is already being kept alive is
    // ignored (one reservation loop, never two).
    void Start(uint64_t cookie, Clock::time_point now, const Reserve &reserve)
    {
        if (m_state == State::Active && m_cookie == cookie)
        {
            return;
        }

        m_cookie = cookie;
        m_state = State::Active;
        m_reason = StopReason::None;
        m_failures = 0;
        m_refreshes = 0;
        m_lastRefresh = now;
        m_rearm = false;
        m_lastActivityMs = Ms(now);

        Say("Reservation keep-alive started cookie=%llx refresh=%ds idle-lease=%s", Cookie(), m_settings.refreshSeconds,
            IdleText().c_str());

        const bool ok = reserve(Payload(m_cookie, true), true);
        if (!ok)
        {
            Failed(now, "the first reservation");
        }
    }

    // once per frame
    void Tick(Clock::time_point now, const Reserve &reserve)
    {
        if (m_state == State::Stopped && m_reason == StopReason::Idle && m_rearm.exchange(false))
        {
            // a client showed up on a server whose lease ran out: it is in use again
            const uint64_t cookie = m_cookie;
            Say("Reservation keep-alive re-armed by client activity cookie=%llx", static_cast<unsigned long long>(cookie));
            m_state = State::Inactive;
            Start(cookie, now, reserve);
            return;
        }

        if (m_state != State::Active)
        {
            return;
        }

        if (m_settings.idleSeconds && m_clients.load() == 0
            && Ms(now) - m_lastActivityMs.load() >= static_cast<int64_t>(m_settings.idleSeconds) * 1000)
        {
            // nobody on the server and nobody came or left for the whole lease: let the reservation go (the engine
            // clears it by itself shortly after; the explicit release makes that immediate)
            reserve(Payload(m_cookie, false), false);
            Stop(StopReason::Idle, now);
            return;
        }

        if (now - m_lastRefresh < std::chrono::seconds(m_settings.refreshSeconds))
        {
            return;
        }

        m_lastRefresh = now;
        m_refreshes++;

        if (reserve(Payload(m_cookie, true), false))
        {
            m_failures = 0;
            Say("Reservation keep-alive refresh cookie=%llx result=1 (#%u)", Cookie(), m_refreshes);
        }
        else
        {
            Say("Reservation keep-alive refresh cookie=%llx result=0 (#%u)", Cookie(), m_refreshes);
            Failed(now, "a refresh");
        }
    }

    void Stop(StopReason reason, Clock::time_point)
    {
        if (m_state != State::Active)
        {
            return;
        }

        m_state = State::Stopped;
        m_reason = reason;
        m_rearm = false; // only a client that shows up AFTER the stop re-arms it (ClientJoined)

        const char *text = "stopped";
        switch (reason)
        {
        case StopReason::Idle: text = "server empty for the idle lease, reservation released"; break;
        case StopReason::Refused: text = "the engine refuses the reservation"; break;
        case StopReason::Shutdown: text = "server shutting down"; break;
        case StopReason::None: break;
        }

        Say("Reservation keep-alive stopped cookie=%llx: %s", Cookie(), text);
    }

    // ---- any thread ----------------------------------------------------------------------------------------------

    // A client authenticated on the server: while any client is on the server the lease does not run out (a match can
    // last longer than the idle lease), and a lease that had run out is re-armed by the next Tick (the server is in use).
    void ClientJoined(Clock::time_point now)
    {
        m_clients++;
        m_lastActivityMs = Ms(now);
        m_rearm = true;
    }

    // A client left: the idle lease counts from now if it was the last one. EndAuthSession also reports sessions that
    // never got to ClientJoined, so the count never goes below zero.
    void ClientLeft(Clock::time_point now)
    {
        int clients = m_clients.load();
        while (clients > 0 && !m_clients.compare_exchange_weak(clients, clients - 1))
        {
        }

        m_lastActivityMs = Ms(now);
    }

    int clients() const { return m_clients; }
    State state() const { return m_state; }
    StopReason reason() const { return m_reason; }
    uint64_t cookie() const { return m_cookie; }
    uint32_t refreshes() const { return m_refreshes; }
    int failures() const { return m_failures; }

private:
    static int64_t Ms(Clock::time_point t)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    }

    unsigned long long Cookie() const { return static_cast<unsigned long long>(m_cookie); }

    std::string IdleText() const
    {
        return m_settings.idleSeconds ? (std::to_string(m_settings.idleSeconds) + "s") : std::string("unlimited");
    }

    void Failed(Clock::time_point now, const char *what)
    {
        if (++m_failures >= m_settings.maxFailures)
        {
            Say("Reservation keep-alive: %s was refused %d times in a row", what, m_failures);
            Stop(StopReason::Refused, now);
        }
    }

    template<typename... Args>
    void Say(const char *format, Args... args)
    {
        if (!m_log)
        {
            return;
        }

        char line[256];
        snprintf(line, sizeof(line), format, args...);
        m_log(line);
    }

    Log m_log;
    Settings m_settings;

    State m_state{ State::Inactive };
    StopReason m_reason{ StopReason::None };
    uint64_t m_cookie{};
    int m_failures{};
    uint32_t m_refreshes{};
    Clock::time_point m_lastRefresh{};

    std::atomic<int64_t> m_lastActivityMs{ 0 };
    std::atomic<bool> m_rearm{ false };
    std::atomic<int> m_clients{ 0 };
};

} // namespace ReservationKeepAlive
