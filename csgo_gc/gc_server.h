#pragma once

#include <memory>
#include <unordered_set>

#include "gc_shared.h"
#include "mm_modes.h"
#include "reservation_keepalive.h"
#include "server_roster.h"
#include "test_accept.h"

class ServerGC final : public SharedGC
{
public:
    ServerGC();
    ~ServerGC();

    // The plain reservation of a classic dedicated server is kept alive by the host thread's per-frame pump
    // (steam_hook.cpp), see reservation_keepalive.h / RESEARCH_FINDINGS.md #60. Host thread only, except
    // NoteClientJoined/Left (any thread) and SetDedicated (before the first ServerHello).
    ReservationKeepAlive::Lease &ReservationLease() { return m_reservationLease; }
    void SetDedicated(bool dedicated) { m_dedicated = dedicated; }
    void NoteClientJoined() { m_reservationLease.ClientJoined(ReservationKeepAlive::Clock::now()); }
    void NoteClientLeft() { m_reservationLease.ClientLeft(ReservationKeepAlive::Clock::now()); }

private:
    void HandleEvent(GCEvent type, uint64_t id, const std::vector<uint8_t> &buffer) override;

    // event handlers
    void HandleMessage(uint32_t type, const void *data, uint32_t size);
    void HandleNetMessage(uint64_t steamId, const void *data, uint32_t size);
    void HandleClientSOCacheUnsubscribe(uint64_t steamId);

    void SendServerWelcome();
    void ReserveServerForOurCookie();
    bool StartAcceptTestRoster();
    void CreateTestRoster(uint32_t realAccountId);
    void OnTestRealPlayerSeen(uint32_t accountId);

    // backend-driven roster (server_roster.h, RESEARCH_FINDINGS.md #55)
    void OnBackendRoster(const std::string &text);
    void ReleaseRoster(const std::string &matchId);
    void ArmRoster(const std::vector<uint32_t> &participants, const std::string &source, bool unreserveFirst);
    void IncrementKillCountAttribute(GCMessageRead &messageRead);

    bool m_sentWelcome{};

    // classic dedicated server: keeps the plain 'G' reservation from expiring (the -gc_mode roster has its own)
    std::atomic<bool> m_dedicated{ false };
    ReservationKeepAlive::Lease m_reservationLease;

    // TEST ONLY (test_accept.h): Q reservation with fake participants, only when configured
    std::unique_ptr<AcceptTest::FakeRoster> m_testRoster;
    const MM::GameMode *m_testMode{};
    bool m_testWaitingForPlayer{}; // roster is armed as soon as the real player's first 0x21 is seen
    uint32_t m_testRealAccountId{};

    // what the backend says the match on this server consists of: the poller asks, the controller decides (GC thread)
    std::unique_ptr<RosterFeed::Poller> m_rosterPoller;
    std::unique_ptr<RosterFeed::Controller> m_rosterController;
    std::unordered_set<uint64_t> m_connectedClients; // worker thread only
};
