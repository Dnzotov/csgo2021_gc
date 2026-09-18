#include "stdafx.h"
#include "gc_client.h"
#include "graffiti.h"
#include "keyvalue.h"

ClientGC::ClientGC(uint64_t steamId)
    : m_steamId{ steamId }
    , m_inventory{ steamId }
{
    // also called from ServerGC's constructor
    Graffiti::Initialize();

    StartThread();

    Platform::Print("ClientGC spawned for user %llu\n", steamId);
}

ClientGC::~ClientGC()
{
    StopThread();
    Platform::Print("ClientGC destroyed\n");
}

void ClientGC::HandleEvent(GCEvent type, uint64_t id, const std::vector<uint8_t> &buffer)
{
    switch (type)
    {
    case GCEvent::Message:
        HandleMessage(static_cast<uint32_t>(id), buffer.data(), static_cast<uint32_t>(buffer.size()));
        break;

    case GCEvent::NetMessage:
        HandleNetMessage(buffer.data(), static_cast<uint32_t>(buffer.size()));
        break;

    case GCEvent::SOCacheRequest:
        HandleSOCacheRequest();
        break;

    default:
        assert(false);
        break;
    }
}

void ClientGC::HandleMessage(uint32_t type, const void *data, uint32_t size)
{
    GCMessageRead messageRead{ type, data, size };
    if (!messageRead.IsValid())
    {
        assert(false);
        return;
    }

    if (messageRead.IsProtobuf())
    {
        switch (messageRead.TypeUnmasked())
        {
        case k_EMsgGCClientHello:
            OnClientHello(messageRead);
            break;

        case k_EMsgGCAdjustItemEquippedState:
            AdjustItemEquippedState(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientPlayerDecalSign:
            ClientPlayerDecalSign(messageRead);
            break;

        case k_EMsgGCUseItemRequest:
            UseItemRequest(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientRequestJoinServerData:
            ClientRequestJoinServerData(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_MatchmakingStart:
            OnMatchmakingStart(messageRead);
            break;

        case k_EMsgGCSetItemPositions:
            SetItemPositions(messageRead);
            break;

        case k_EMsgGCApplySticker:
            ApplySticker(messageRead);
            break;

        case k_EMsgGCStoreGetUserData:
            StoreGetUserData(messageRead);
            break;

        case k_EMsgGCStorePurchaseInit:
            StorePurchaseInit(messageRead);
            break;

        case k_EMsgGCStorePurchaseFinalize:
            StorePurchaseFinalize(messageRead);
            break;

        case k_EMsgGCCasketItemLoadContents:
            ProcessCasketItemLoadContents(messageRead);
            break;

        case k_EMsgGCCasketItemAdd:
            ProcessCasketItemAdd(messageRead);
            break;

        case k_EMsgGCCasketItemExtract:
            ProcessCasketItemExtract(messageRead);
            break;

        default:
            Platform::Print("ClientGC::HandleMessage: unhandled protobuf message %s\n",
                MessageName(messageRead.TypeUnmasked()));
            break;
        }
    }
    else
    {
        switch (messageRead.TypeUnmasked())
        {
        case k_EMsgGCDelete:
            DeleteItem(messageRead);
            break;

        case k_EMsgGCUnlockCrate:
            UnlockCrate(messageRead);
            break;

        case k_EMsgGCNameItem:
            NameItem(messageRead);
            break;

        case k_EMsgGCNameBaseItem:
            NameBaseItem(messageRead);
            break;

        case k_EMsgGCRemoveItemName:
            RemoveItemName(messageRead);
            break;

        default:
            Platform::Print("ClientGC::HandleMessage: unhandled struct message %s\n",
                MessageName(messageRead.TypeUnmasked()));
            break;
        }
    }
}

void ClientGC::HandleNetMessage(const void *data, uint32_t size)
{
    // pass 0 as type so it gets parsed from the message
    GCMessageRead messageRead{ 0, data, size };
    if (!messageRead.IsValid())
    {
        assert(false);
        return;
    }

    if (messageRead.IsProtobuf())
    {
        switch (messageRead.TypeUnmasked())
        {
        case k_EMsgGC_IncrementKillCountAttribute:
            IncrementKillCountAttribute(messageRead);
            return;
        }
    }

    Platform::Print("ClientGC::HandleNetMessage: unhandled protobuf message %s\n",
        MessageName(messageRead.TypeUnmasked()));
}

void ClientGC::HandleSOCacheRequest()
{
    CMsgSOCacheSubscribed message;
    m_inventory.BuildCacheSubscription(message, GetConfig().Level(), true);

    GCMessageWrite messageWrite{ k_ESOMsg_CacheSubscribed, message };
    PostToHost(HostEvent::NetMessage, 0, messageWrite.Data(), messageWrite.Size());
}

void ClientGC::SendMessageToGame(bool sendToGameServer, uint32_t type,
    const google::protobuf::MessageLite &message, uint64_t jobId)
{
    GCMessageWrite messageWrite{ type, message, jobId };

    if (sendToGameServer)
    {
        PostToHost(HostEvent::NetMessage, 0, messageWrite.Data(), messageWrite.Size());
    }

    PostToHost(HostEvent::Message, messageWrite.TypeMasked(), messageWrite.Data(), messageWrite.Size());
}

constexpr uint32_t MakeAddress(uint32_t v1, uint32_t v2, uint32_t v3, uint32_t v4)
{
    return v4 | (v3 << 8) | (v2 << 16) | (v1 << 24);
}

static void BuildCSWelcome(CMsgCStrike15Welcome &message)
{
    // mikkotodo cleanup dox
    message.set_store_item_hash(136617352);
    message.set_timeplayedconsecutively(0);
    message.set_time_first_played(1329845773);
    message.set_last_time_played(1680260376);
    message.set_last_ip_address(MakeAddress(127, 0, 0, 1));
}

void ClientGC::BuildMatchmakingHello(CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &message)
{
    message.set_account_id(AccountId());

    // this is the state of csgo matchmaking in 2024
    message.mutable_global_stats()->set_players_online(0);
    message.mutable_global_stats()->set_servers_online(0);
    message.mutable_global_stats()->set_players_searching(0);
    message.mutable_global_stats()->set_servers_available(0);
    message.mutable_global_stats()->set_ongoing_matches(0);
    message.mutable_global_stats()->set_search_time_avg(0);

    // don't write search_statistics

    message.mutable_global_stats()->set_main_post_url("");

    // bullshit
    // must not exceed the actual client's own ClientVersion (csgo/steam.inf) or it
    // thinks it's outdated and refuses to proceed. Upstream's 13857 is a much later
    // (CS2-era) client version than the Oct 2021 build 1352 this target actually is.
    message.mutable_global_stats()->set_required_appid_version(1352);
    message.mutable_global_stats()->set_pricesheet_version(1680057676); // mikkotodo revisit
    message.mutable_global_stats()->set_twitch_streams_version(2);
    message.mutable_global_stats()->set_active_tournament_eventid(20);
    message.mutable_global_stats()->set_active_survey_id(0);
    // required_appid_version2 (CS2 dual-version transition field) intentionally not
    // set -- doesn't apply to this pre-CS2 2021 target

    message.set_vac_banned(GetConfig().VacBanned());
    message.mutable_commendation()->set_cmd_friendly(GetConfig().CommendedFriendly());
    message.mutable_commendation()->set_cmd_teaching(GetConfig().CommendedTeaching());
    message.mutable_commendation()->set_cmd_leader(GetConfig().CommendedLeader());
    message.set_player_level(GetConfig().Level());
    message.set_player_cur_xp(GetConfig().Xp());
}

void ClientGC::BuildClientWelcome(CMsgClientWelcome &message, const CMsgCStrike15Welcome &csWelcome,
    const CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &matchmakingHello)
{
    // mikkotodo remove dox
    message.set_version(0); // this is accurate
    message.set_game_data(csWelcome.SerializeAsString());
    m_inventory.BuildCacheSubscription(*message.add_outofdate_subscribed_caches(), GetConfig().Level(), false);
    message.mutable_location()->set_latitude(65.0133006f);
    message.mutable_location()->set_longitude(25.4646212f);
    message.mutable_location()->set_country("FI"); // finland
    message.set_game_data2(matchmakingHello.SerializeAsString());
    message.set_rtime32_gc_welcome_timestamp(static_cast<uint32_t>(time(nullptr)));
    message.set_currency(2); // euros
    message.set_txn_country_code("FI"); // finland
}

void ClientGC::SendRankUpdate()
{
    CMsgGCCStrike15_v2_ClientGCRankUpdate message;

    PlayerRankingInfo *rank = message.add_rankings();
    rank->set_account_id(AccountId());
    rank->set_rank_id(GetConfig().CompetitiveRank());
    rank->set_wins(GetConfig().CompetitiveWins());
    rank->set_rank_type_id(RankTypeCompetitive);

    rank = message.add_rankings();
    rank->set_account_id(AccountId());
    rank->set_rank_id(GetConfig().WingmanRank());
    rank->set_wins(GetConfig().WingmanWins());
    rank->set_rank_type_id(RankTypeWingman);

    rank = message.add_rankings();
    rank->set_account_id(AccountId());
    rank->set_rank_id(GetConfig().DangerZoneRank());
    rank->set_wins(GetConfig().DangerZoneWins());
    rank->set_rank_type_id(RankTypeDangerZone);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientGCRankUpdate, message);
}

void ClientGC::OnClientHello(GCMessageRead &messageRead)
{
    CMsgClientHello hello;
    if (!messageRead.ReadProtobuf(hello))
    {
        Platform::Print("Parsing CMsgClientHello failed, ignoring\n");
        return;
    }

    // we don't care about anything in this message, just reply
    CMsgCStrike15Welcome csWelcome;
    BuildCSWelcome(csWelcome);

    CMsgGCCStrike15_v2_MatchmakingGC2ClientHello mmHello;
    BuildMatchmakingHello(mmHello);

    CMsgClientWelcome clientWelcome;
    BuildClientWelcome(clientWelcome, csWelcome, mmHello);

    SendMessageToGame(false, k_EMsgGCClientWelcome, clientWelcome);

    // the real gc sends this a bit later when it has more info to put on it
    // however we have everything at our fingertips so send it right away
    // mikkotodo is this even needed? k_EMsgGCClientWelcome should have it all already
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientHello, mmHello);

    // send all ranks here as well, it's a bit back and forth with real gc
    SendRankUpdate();
}

void ClientGC::AdjustItemEquippedState(GCMessageRead &messageRead)
{
    CMsgAdjustItemEquippedState message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgAdjustItemEquippedState failed, ignoring\n");
        return;
    }

    CMsgSOMultipleObjects update;
    if (!m_inventory.EquipItem(message.item_id(), message.new_class(), message.new_slot(), update))
    {
        // no change
        assert(false);
        return;
    }

    // let the gameserver know, too
    SendMessageToGame(true, k_ESOMsg_UpdateMultiple, update);
}

void ClientGC::ClientPlayerDecalSign(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientPlayerDecalSign message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCCStrike15_v2_ClientPlayerDecalSign failed, ignoring\n");
        return;
    }

    if (!Graffiti::SignMessage(*message.mutable_data()))
    {
        Platform::Print("Could not sign graffiti! it won't appear\n");
        return;
    }

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientPlayerDecalSign, message);
}

void ClientGC::UseItemRequest(GCMessageRead &messageRead)
{
    CMsgUseItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgUseItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject destroy;
    CMsgSOMultipleObjects updateMultiple;
    CMsgGCItemCustomizationNotification notification;

    if (m_inventory.UseItem(message.item_id(), destroy, updateMultiple, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        SendMessageToGame(true, k_ESOMsg_UpdateMultiple, updateMultiple);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
}

// EXPERIMENTAL, see test_mm.h -- parses "a.b.c.d" from config.txt's matchmaking.test_server_address
static uint32_t ParseIpAddress(std::string_view ip)
{
    std::string ipStr{ ip };
    unsigned a, b, c, d;
    if (sscanf(ipStr.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
    {
        Platform::Print("[MM-TEST] Failed to parse matchmaking.test_server_address '%s', falling back to 127.0.0.1\n",
            ipStr.c_str());
        return MakeAddress(127, 0, 0, 1);
    }

    return MakeAddress(a, b, c, d);
}

static void AddressString(uint32_t ip, uint32_t port, char *buffer, size_t bufferSize)
{
    snprintf(buffer, bufferSize,
        "%u.%u.%u.%u:%u\n",
        (ip >> 24) & 0xff,
        (ip >> 16) & 0xff,
        (ip >> 8) & 0xff,
        ip & 0xff,
        port);
}

// Builds the payload for IVEngineServer::ReserveServerForQueuedGame, format confirmed byte-exact
// against both project446's builder and a direct disassembly of CBaseServer::ReserveServerForQueuedGame
// in our retail engine.dll (RESEARCH_FINDINGS.md #28.4/#29.3): "G<cookie_hex>,<matchid_hex>,<accountCount>:[<account_hex>]...".
// cookie == GameServerCookieId (the same value already sent to the client in MatchmakingGC2ClientReserve/
// ClientRequestJoinServerData, so the client's A2S_RESERVE_CHECK will match m_nReservationCookie).
// match_id has no real backing value (no server-side 9105 relay yet) so it falls back to the cookie
// itself -- the same fallback project446 uses when it has no real match id (RESEARCH_FINDINGS.md #28.2).
static std::string BuildReservationPayload(uint32_t localAccountId)
{
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "G%llx,%llx,%u:[%x]",
        static_cast<unsigned long long>(GameServerCookieId),
        static_cast<unsigned long long>(GameServerCookieId),
        1u,
        localAccountId);
    return buffer;
}

void ClientGC::ClientRequestJoinServerData(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientRequestJoinServerData request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::Print("Parsing CMsgGCCStrike15_v2_ClientRequestJoinServerData failed, ignoring\n");
        return;
    }

    CMsgGCCStrike15_v2_ClientRequestJoinServerData response = request;
    response.mutable_res()->set_serverid(request.version());
    response.mutable_res()->set_direct_udp_ip(request.server_ip());
    response.mutable_res()->set_direct_udp_port(request.server_port());
    response.mutable_res()->set_reservationid(GameServerCookieId);

    char addressString[32];
    AddressString(request.server_ip(), request.server_port(), addressString, sizeof(addressString));
    response.mutable_res()->set_server_address(addressString);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientRequestJoinServerData, response);
}

// EXPERIMENTAL: manual native-pipeline test. Reads the real MatchmakingStart (9101) the
// client already sends, and answers with a MatchmakingGC2ClientReserve (9107) pointed at
// config.txt's matchmaking.test_server_address/test_server_port -- this must now be a REAL
// CS:GO 2021 dedicated server, not TestMM (see RESEARCH_FINDINGS.md #27): the reservation
// protobuf only carries one address, used both for A2S_RESERVE_CHECK and the final
// QueueConnect target, so a separate fake responder can never lead to a real connect.
// TestMM (test_mm.h) is deliberately NOT started from here anymore -- it's kept in the
// project for future protocol-only tests, just not wired into this runtime path. Getting
// the real dedicated server to actually answer 0x21 requires
// IVEngineServer::ReserveServerForQueuedGame() to be called server-side -- this now happens
// via BuildReservationPayload()/PostToHost(HostEvent::ReserveServerForQueuedGame, ...) below,
// which DispatchReserveServerForQueuedGame() (steam_hook.cpp) picks up on the main thread
// and turns into the real engine call (RESEARCH_FINDINGS.md #28-#30).
// Casual (eGame=7) only for this first vertical test -- other game types are logged and
// ignored, not because they can't work, but because the Accept flow (game_type in
// {8,9,10,11,13}, see RESEARCH_FINDINGS.md #5/#9) isn't implemented here yet.
void ClientGC::OnMatchmakingStart(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_MatchmakingStart request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::Print("[MM-TEST] Parsing CMsgGCCStrike15_v2_MatchmakingStart failed, ignoring\n");
        return;
    }

    // game_type is composed as (eGame & 0xF) | (eMapGroup << 8), confirmed via live test
    // against the real client (RESEARCH_FINDINGS.md #20) -- not a bare 0-13 value.
    uint32_t eGame = request.game_type() & 0xF;

    Platform::Print(
        "[MM-TEST] MatchmakingStart received: game_type=%u (eGame=%u) accounts=%d prime_only=%d client_version=%u\n",
        request.game_type(), eGame, request.account_ids_size(), request.prime_only(), request.client_version());

    if (eGame != 7)
    {
        Platform::Print("[MM-TEST] eGame=%u is not Casual -- only Casual is handled for now, ignoring\n", eGame);
        return;
    }

    uint32_t testServerIp = ParseIpAddress(GetConfig().TestServerAddress());
    uint16_t testServerPort = GetConfig().TestServerPort();
    const char *testMap = "de_dust2";

    Platform::Print("[MM-TEST] Test server (REAL dedicated server expected): %.*s:%u\n",
        static_cast<int>(GetConfig().TestServerAddress().size()), GetConfig().TestServerAddress().data(), testServerPort);

    Platform::Print("[MM-TEST] Sending MatchmakingGC2ClientReserve: map=%s reservationid=%llx\n",
        testMap, static_cast<unsigned long long>(GameServerCookieId));

    CMsgGCCStrike15_v2_MatchmakingGC2ClientReserve reserve;
    reserve.set_serverid(1);
    reserve.set_direct_udp_ip(testServerIp);
    reserve.set_direct_udp_port(testServerPort);
    reserve.set_reservationid(GameServerCookieId);
    reserve.set_map(testMap);

    char addressString[32];
    AddressString(testServerIp, testServerPort, addressString, sizeof(addressString));
    reserve.set_server_address(addressString);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientReserve, reserve);

    Platform::Print("[MM-TEST] MatchmakingGC2ClientReserve dispatched\n");

    // Bridge the server-side half of the reservation: IVEngineServer::ReserveServerForQueuedGame
    // must be called on the engine's main thread (RESEARCH_FINDINGS.md #29.5), not this GC worker
    // thread, so hand the payload off through the existing SharedGC::PostToHost queue -- it's
    // drained every frame in Hk_SteamAPI_RunCallbacks (steam_hook.cpp), which RESEARCH_FINDINGS.md
    // #30.2 confirms runs on the same thread as the engine's per-frame ticks.
    std::string reservationPayload = BuildReservationPayload(AccountId());
    Platform::Print("[MM] Queueing server reservation on host thread\n");
    PostToHost(HostEvent::ReserveServerForQueuedGame, 0,
        reservationPayload.data(), static_cast<uint32_t>(reservationPayload.size()));
}

void ClientGC::SetItemPositions(GCMessageRead &messageRead)
{
    CMsgSetItemPositions message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgSetItemPositions failed, ignoring\n");
        return;
    }

    std::vector<CMsgItemAcknowledged> acknowledgements;
    acknowledgements.reserve(message.item_positions_size());

    CMsgSOMultipleObjects update;
    if (m_inventory.SetItemPositions(message, acknowledgements, update))
    {
        for (const CMsgItemAcknowledged &acknowledgement : acknowledgements)
        {
            // send these to the server only
            GCMessageWrite messageWrite{ k_EMsgGCItemAcknowledged, acknowledgement };
            PostToHost(HostEvent::NetMessage, 0, messageWrite.Data(), messageWrite.Size());
        }

        SendMessageToGame(true, k_ESOMsg_UpdateMultiple, update);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::IncrementKillCountAttribute(GCMessageRead &messageRead)
{
    CMsgIncrementKillCountAttribute message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgIncrementKillCountAttribute failed, ignoring\n");
        return;
    }

    assert(message.event_type() == 0);

    CMsgSOSingleObject update;
    if (m_inventory.IncrementKillCountAttribute(message.item_id(), message.amount(), update))
    {
        SendMessageToGame(true, k_ESOMsg_Update, update);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::ApplySticker(GCMessageRead &messageRead)
{
    CMsgApplySticker message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgApplySticker failed, ignoring\n");
        return;
    }

    assert(!message.item_item_id() != !message.baseitem_defidx());

    CMsgSOSingleObject update, destroy;
    CMsgGCItemCustomizationNotification notification;

    if (!message.sticker_item_id())
    {
        // scrape
        if (m_inventory.ScrapeSticker(message, update, destroy, notification))
        {
            if (destroy.has_type_id())
            {
                // destroying a default item
                SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
            }

            if (update.has_type_id())
            {
                // if the item got removed (handled above), nothing gets updated
                SendMessageToGame(true, k_ESOMsg_Update, update);
            }

            if (notification.has_request())
            {
                // might get a k_EGCItemCustomizationNotification_RemoveSticker
                SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
            }
        }
        else
        {
            assert(false);
        }
    }
    else if (m_inventory.ApplySticker(message, update, destroy, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        SendMessageToGame(true, k_ESOMsg_Update, update);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::StoreGetUserData(GCMessageRead &messageRead)
{
    CMsgStoreGetUserData message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgStoreGetUserData failed, ignoring\n");
        return;
    }

    KeyValue priceSheet{ "price_sheet" };
    if (!priceSheet.ParseFromFile("csgo_gc/price_sheet.txt"))
    {
        return;
    }

    std::string binaryString;
    binaryString.reserve(1 << 17);
    priceSheet.BinaryWriteToString(binaryString);

    // fuck you idiot
    CMsgStoreGetUserDataResponse response;
    response.set_result(1);
    response.set_price_sheet_version(1729); // what
    *response.mutable_price_sheet() = std::move(binaryString);

    SendMessageToGame(false, k_EMsgGCStoreGetUserDataResponse, response);
}

void ClientGC::StorePurchaseInit(GCMessageRead &messageRead)
{
    CMsgGCStorePurchaseInit message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCStorePurchaseInit failed, ignoring\n");
        return;
    }

    // value doesn't matter
    uint64_t transactionId = Random{}.Integer<uint64_t>();

    assert(!m_transactionId);
    m_transactionId = transactionId;
    m_transactionItemIds.reserve(message.line_items_size()); // rough approx

    // inventory update response
    std::vector<CMsgSOSingleObject> inventoryUpdate;

    for (const auto &item : message.line_items())
    {
        for (uint32_t i = 0; i < item.quantity(); i++)
        {
            uint64_t itemId = m_inventory.PurchaseItem(item.item_def_id(), inventoryUpdate);
            if (!itemId)
            {
                assert(false);
            }
            else
            {
                m_transactionItemIds.push_back(itemId);
            }
        }
    }

    char url[128]; // url doesn't matter, but it needs to be set
    snprintf(url, sizeof(url), "https://checkout.steampowered.com/checkout/approvetxn/%llu/?returnurl=steam", transactionId);

    CMsgGCStorePurchaseInitResponse response;
    response.set_result(1); // success
    response.set_txn_id(transactionId);
    response.set_url(url);
    response.mutable_item_ids()->Assign(m_transactionItemIds.begin(), m_transactionItemIds.end());

    SendMessageToGame(false, k_EMsgGCStorePurchaseInitResponse, response, messageRead.JobId());

    // FIXME: why would the server care???
    for (auto &newItem : inventoryUpdate)
    {
        SendMessageToGame(true, k_ESOMsg_Create, newItem);
    }

    // this will run the steam callback
    PostToHost(HostEvent::MicroTransactionResponse, 0, nullptr, 0);
}

void ClientGC::StorePurchaseFinalize(GCMessageRead &messageRead)
{
    CMsgGCStorePurchaseFinalize message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCStorePurchaseFinalize failed, ignoring\n");
        return;
    }

    assert(m_transactionId);

    CMsgGCStorePurchaseFinalizeResponse response;
    response.set_result(1); // success
    response.mutable_item_ids()->Assign(m_transactionItemIds.begin(), m_transactionItemIds.end());
    SendMessageToGame(false, k_EMsgGCStorePurchaseFinalizeResponse, response, messageRead.JobId());

    // done with this one
    m_transactionId = 0;
}

void ClientGC::DeleteItem(GCMessageRead &messageRead)
{
    // there is data after this, but i don't know what it is
    uint64_t itemId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCDelete failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject destroyed;
    if (m_inventory.RemoveItem(itemId, destroyed))
    {
        // mikkotodo what does the server want to know
        SendMessageToGame(true, k_ESOMsg_Destroy, destroyed);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::UnlockCrate(GCMessageRead &messageRead)
{
    uint64_t keyId = messageRead.ReadUint64();
    uint64_t crateId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCUnlockCrate failed, ignoring\n");
        return;
    }

    Platform::Print("CASE OPENING %llu with %llu\n", crateId, keyId);

    CMsgSOSingleObject destroyCrate, destroyKey, newItem;
    CMsgGCItemCustomizationNotification notification;

    if (m_inventory.UnlockCrate(
            crateId,
            keyId,
            destroyCrate,
            destroyKey,
            newItem,
            notification))
    {
        // mikkotodo what does the server want to know
        SendMessageToGame(true, k_ESOMsg_Destroy, destroyCrate);
        SendMessageToGame(true, k_ESOMsg_Destroy, destroyKey);
        SendMessageToGame(true, k_ESOMsg_Create, newItem);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::NameItem(GCMessageRead &messageRead)
{
    uint64_t nameTagId = messageRead.ReadUint64();
    uint64_t itemId = messageRead.ReadUint64();
    messageRead.ReadData(1); // skip the sentinel
    std::string_view name = messageRead.ReadString();

    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCNameItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject update, destroy;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.NameItem(nameTagId, itemId, name, update, destroy, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Update, update);
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::NameBaseItem(GCMessageRead &messageRead)
{
    uint64_t nameTagId = messageRead.ReadUint64();
    uint32_t defIndex = messageRead.ReadUint32();
    messageRead.ReadData(1); // skip the sentinel
    std::string_view name = messageRead.ReadString();

    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCNameBaseItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject create, destroy;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.NameBaseItem(nameTagId, defIndex, name, create, destroy, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Create, create);
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::RemoveItemName(GCMessageRead &messageRead)
{
    uint64_t itemId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCRemoveItemName failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject update, destroy;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.RemoveItemName(itemId, update, destroy, notification))
    {
        if (update.has_type_id())
        {
            SendMessageToGame(true, k_ESOMsg_Update, update);
        }

        if (destroy.has_type_id())
        {
            SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        }

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::ProcessCasketItemLoadContents(GCMessageRead &messageRead)
{
    CMsgCasketItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgCasketItem failed, ignoring\n");
        return;
    }

    CMsgGCItemCustomizationNotification notification;
    notification.set_request(k_EGCItemCustomizationNotification_CasketContents);
    notification.add_item_id(message.casket_item_id());
    SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
}

void ClientGC::ProcessCasketItemAdd(GCMessageRead &messageRead)
{
    CMsgCasketItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgCasketItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject modifyCasket, modifyItem;
    CMsgGCItemCustomizationNotification notification;

    if (m_inventory.CasketItemAdd(message.casket_item_id(), message.item_item_id(),
            modifyCasket, modifyItem, notification))
    {
        SendMessageToGame(false, k_ESOMsg_Update, modifyItem);
        SendMessageToGame(false, k_ESOMsg_Update, modifyCasket);
        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        // capacity exceeded: notification was already set up by CasketItemAdd
        if (notification.has_request())
        {
            SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
        }
    }
}

void ClientGC::ProcessCasketItemExtract(GCMessageRead &messageRead)
{
    CMsgCasketItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgCasketItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject modifyCasket, modifyItem;
    CMsgGCItemCustomizationNotification notification;

    if (m_inventory.CasketItemExtract(message.casket_item_id(), message.item_item_id(),
            modifyCasket, modifyItem, notification))
    {
        SendMessageToGame(false, k_ESOMsg_Update, modifyItem);
        SendMessageToGame(false, k_ESOMsg_Update, modifyCasket);
        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}
