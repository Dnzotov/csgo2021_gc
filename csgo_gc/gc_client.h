#pragma once

#include "backend_client.h"
#include "config.h"
#include "gc_shared.h"
#include "inventory.h"

namespace MM
{
struct GameMode;
}

class ClientGC final : public SharedGC
{
public:
    ClientGC(uint64_t steamId);
    ~ClientGC();

private:
    void HandleEvent(GCEvent type, uint64_t id, const std::vector<uint8_t> &buffer) override;

    // event handlers
    void HandleMessage(uint32_t type, const void *data, uint32_t size);
    void HandleNetMessage(const void *data, uint32_t size);
    void HandleSOCacheRequest();

    // send to the local game and the game server we're connected to (if we're connected)
    void SendMessageToGame(bool sendToGameServer, uint32_t type,
        const google::protobuf::MessageLite &message, uint64_t jobId = JobIdInvalid);

    void OnClientHello(GCMessageRead &messageRead);
    void OnMatchmakingStart(GCMessageRead &messageRead);
    void OnMatchmakingStop();
    void OnBackendSearchResult(const std::vector<uint8_t> &buffer);
    void StartServerFlow(const BackendClient::Assignment &assignment, bool alreadyAccepted = false);
    void OnPartySearchDiscovered(const BackendClient::SearchResult &result);
    void TryConnectAfterAccept();
    void OnReservationFullyAccepted();
    void WithdrawAccept(const std::string &reason);
    void EndAccept(const std::string &reason);
    void SendMatchmakingUpdate(int32_t matchmaking);
    void AdjustItemEquippedState(GCMessageRead &messageRead);
    void ClientPlayerDecalSign(GCMessageRead &messageRead);
    void UseItemRequest(GCMessageRead &messageRead);
    void ClientRequestJoinServerData(GCMessageRead &messageRead);
    void SetItemPositions(GCMessageRead &messageRead);
    void IncrementKillCountAttribute(GCMessageRead &messageRead);
    void ApplySticker(GCMessageRead &messageRead);
    void StoreGetUserData(GCMessageRead &messageRead);
    void StorePurchaseInit(GCMessageRead &messageRead);
    void StorePurchaseFinalize(GCMessageRead &messageRead);

    void DeleteItem(GCMessageRead &messageRead);
    void UnlockCrate(GCMessageRead &messageRead);
    void NameItem(GCMessageRead &messageRead);
    void NameBaseItem(GCMessageRead &messageRead);
    void RemoveItemName(GCMessageRead &messageRead);

    void ProcessCasketItemLoadContents(GCMessageRead &messageRead);
    void ProcessCasketItemAdd(GCMessageRead &messageRead);
    void ProcessCasketItemExtract(GCMessageRead &messageRead);

    void BuildMatchmakingHello(CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &message);
    void BuildClientWelcome(CMsgClientWelcome &message, const CMsgCStrike15Welcome &csWelcome,
        const CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &matchmakingHello);
    void SendRankUpdate();

    uint32_t AccountId() const { return m_steamId & 0xffffffff; }

    // TEST ONLY (see test_accept.h): the reservation we handed out for an Accept mode (game_type 8/10/13) and
    // are waiting to be fully accepted, so the second MatchmakingGC2ClientReserve (9107) can repeat it.
    // Only touched from the worker thread.
    struct PendingAccept
    {
        bool active{};
        uint32_t serverIp{};
        uint16_t serverPort{};
        uint32_t eGame{};
        std::string map;
        std::string matchId; // the backend match whose Accept this is (the backend withdraws it when the deadline passes)
        // Connecting needs BOTH (RESEARCH_FINDINGS.md #65): the game server says everybody is at stage 2 (0x25 awaiting 0) AND
        // the backend says every real player accepted (the search is READY_TO_CONNECT). One player accepting early is not enough.
        bool serverAccepted{};
        bool backendAccepted{};
    };

    PendingAccept m_pendingAccept;

    // The search the Java backend is working on for us (9101 seen, no server yet). Filled by OnMatchmakingStart, used by
    // StartServerFlow once the backend assigned a server. Only touched from the worker thread.
    struct ActiveSearch
    {
        bool active{};
        uint32_t gameType{}; // 9101 game_type as received
        uint32_t eGame{};
        const MM::GameMode *mode{};
        std::string fallbackMap; // the map we would advertise if the assignment carried none
        bool partyMember{};      // the search was sent by the lobby leader, this client only follows it
    };

    ActiveSearch m_activeSearch;

    // The search this client follows was sent by the lobby leader (party member): it is not this client's to cancel while its
    // Accept is running - a MatchmakingStop then is the client's own reaction to a lobby state it does not know (RESEARCH_FINDINGS.md #65)
    bool m_followsParty{};

    const uint64_t m_steamId;

    Inventory m_inventory;

    // microtransactions, we only have one going at a time
    uint64_t m_transactionId{};
    std::vector<uint64_t> m_transactionItemIds;
};
