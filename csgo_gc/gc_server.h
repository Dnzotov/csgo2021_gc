#pragma once

#include <memory>
#include <unordered_set>

#include "gc_shared.h"
#include "mm_modes.h"
#include "test_accept.h"

class ServerGC final : public SharedGC
{
public:
    ServerGC();
    ~ServerGC();

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
    void IncrementKillCountAttribute(GCMessageRead &messageRead);

    bool m_sentWelcome{};

    // TEST ONLY (test_accept.h): Q reservation with fake participants, only when configured
    std::unique_ptr<AcceptTest::FakeRoster> m_testRoster;
    const MM::GameMode *m_testMode{};
    bool m_testWaitingForPlayer{}; // roster is armed as soon as the real player's first 0x21 is seen
    uint32_t m_testRealAccountId{};
    std::unordered_set<uint64_t> m_connectedClients; // worker thread only
};
