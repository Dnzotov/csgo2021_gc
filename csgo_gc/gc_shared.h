#pragma once

#include "gc_message.h"

enum class HostEvent
{
    Message, // id contains the message type, buffer contains the payload
    NetMessage, // id contains the recipient steam id, buffer contains the payload
    MicroTransactionResponse, // runs MicroTxnAuthorizationResponse_t, no arguments
    ReserveServerForQueuedGame, // id unused, buffer contains the IVEngineServer::ReserveServerForQueuedGame payload string (see RESEARCH_FINDINGS.md #28-#30)
    ReservationKeepAlive, // dedicated server, no -gc_mode: id = the reservation cookie, start reserving it and keep it alive (reservation_keepalive.h, RESEARCH_FINDINGS.md #60)
};

enum class GCEvent
{
    Message, // id contains the message type, buffer contains the payload
    NetMessage, // id contains the recipient steam id, buffer contains the payload
    SOCacheRequest, // sent to client gc when connected to a gameserver
    ClientSOCacheUnsubscribe, // sent to server gc when a client disconnects, id contains the steam id
    TestRealPlayerSeen, // TEST ONLY: sent to server gc when srcds saw the first 0x21 of a real player, id contains the AccountID
    ReservationFullyAccepted, // TEST ONLY: sent to client gc when the reserved server reports 0x25 stage 2 / awaiting 0 (test_accept.h)
    BackendSearchResult, // sent to client gc by the backend client's worker thread: buffer is BackendClient::SerializeResult (backend_client.h)
    BackendRoster, // sent to server gc by the roster poller: buffer is RosterFeed::Serialize (server_roster.h), the backend's roster of this server's match
    TestFakesReady, // TEST ONLY: sent to server gc by the fake roster driver, id = 1 when every fake participant is at reservation stage 1, 0 while (re)arming
};

struct EventData
{
    int type; // HostEvent or GCEvent
    uint64_t id;
    std::vector<uint8_t> buffer;
};

// shared logic between client and server gcs
class SharedGC
{
public:
    void PostToGC(GCEvent type, uint64_t id, const void *data, uint32_t dataSize);

    void GetHostEvents(std::vector<EventData> &events);

protected:
    // must be manually called by the gc
    void StartThread();
    void StopThread();

    void PostToHost(HostEvent type, uint64_t id, const void *data, uint32_t dataSize);

private:
    virtual void HandleEvent(GCEvent type, uint64_t id, const std::vector<uint8_t> &buffer) = 0;

    void WorkerThread();

    std::mutex m_gcEventMutex;
    std::condition_variable m_cv;
    std::vector<EventData> m_gcEvents;
    std::thread m_thread;
    bool m_stopping{ false };

    std::mutex m_hostEventMutex;
    std::vector<EventData> m_hostEvents;
};

const char *MessageName(uint32_t type);
