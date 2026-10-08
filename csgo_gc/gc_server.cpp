#include "stdafx.h"
#include <algorithm>

#include "gc_server.h"
#include "networking_shared.h"
#include "backend_client.h"
#include "gc_const.h"
#include "gc_const_csgo.h"
#include "config.h"
#include "graffiti.h"
#include "inventory.h"

// yuck!! needed for CSteamID (construct full id from account id)
#include "steam/steamclientpublic.h"

ServerGC::ServerGC()
    : m_reservationLease{
        [](const std::string &line) { Platform::Print("[MM] %s\n", line.c_str()); },
        [] {
            ReservationKeepAlive::Lease::Settings settings;
            settings.idleSeconds = GetConfig().ReservationIdleSeconds();
            return settings;
        }() }
{
    // also called from ClientGC's constructor
    Graffiti::Initialize();

    StartThread();

    Platform::Print("ServerGC spawned\n");
}

ServerGC::~ServerGC()
{
    m_reservationLease.Stop(ReservationKeepAlive::Lease::StopReason::Shutdown, ReservationKeepAlive::Clock::now());
    AcceptTest::SetServerSniff(nullptr, nullptr, 0);
    m_rosterPoller.reset(); // its worker thread posts events to us
    m_testRoster.reset();
    StopThread();
    Platform::Print("ServerGC destroyed\n");
}

void ServerGC::HandleEvent(GCEvent type, uint64_t id, const std::vector<uint8_t> &buffer)
{
    switch (type)
    {
    case GCEvent::Message:
        HandleMessage(static_cast<uint32_t>(id), buffer.data(), static_cast<uint32_t>(buffer.size()));
        break;

    case GCEvent::NetMessage:
        HandleNetMessage(id, buffer.data(), static_cast<uint32_t>(buffer.size()));
        break;

    case GCEvent::ClientSOCacheUnsubscribe:
        HandleClientSOCacheUnsubscribe(id);
        break;

    case GCEvent::TestRealPlayerSeen:
        OnTestRealPlayerSeen(static_cast<uint32_t>(id));
        break;

    case GCEvent::BackendRoster:
        OnBackendRoster(std::string(buffer.begin(), buffer.end()));
        break;

    case GCEvent::BackendSkins:
        OnBackendSkins();
        break;

    case GCEvent::ClientAuthenticated:
        OnClientAuthenticated(id);
        break;

    case GCEvent::TestFakesReady:
        if (m_rosterController)
        {
            m_rosterController->OnFakesReady(id != 0);
        }
        break;

    default:
        assert(false);
        break;
    }
}

void ServerGC::HandleMessage(uint32_t type, const void *data, uint32_t size)
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
        case k_EMsgGCServerHello:
            SendServerWelcome();
            ReserveServerForOurCookie();
            break;

        case k_EMsgGCCStrike15_v2_Server2GCClientValidate:
            // server doesn't want a response so ignore
            break;

        case k_EMsgGC_IncrementKillCountAttribute:
            IncrementKillCountAttribute(messageRead);
            break;

        default:
            Platform::Print("ServerGC::HandleMessage: unhandled protobuf message %s)\n",
                MessageName(messageRead.TypeUnmasked()));
            break;
        }
    }
}

void ServerGC::HandleClientSOCacheUnsubscribe(uint64_t steamId)
{
    m_authenticated.erase(steamId);
    m_skinsApplied.erase(steamId);

    if (m_connectedClients.erase(steamId) && m_connectedClients.empty() && m_testRoster)
    {
        // TEST ONLY: the last real client left, start over with a fresh roster for the next match
        m_testRoster->OnMatchEnded();
    }

    if (m_connectedClients.empty() && m_rosterController)
    {
        // a roster change the backend announced while a player was on the server can be applied now
        m_rosterController->OnPlayersChanged();
    }

    Platform::Print("HandleClientSOCacheUnsubscribe: %llu\n", steamId);

    CMsgSOCacheUnsubscribed message;
    message.mutable_owner_soid()->set_type(SoIdTypeSteamId);
    message.mutable_owner_soid()->set_id(steamId);

    GCMessageWrite write{ k_ESOMsg_CacheUnsubscribed, message };
    PostToHost(HostEvent::Message, write.TypeMasked(), write.Data(), write.Size());
}

template<typename T>
static bool ValidateMessageOwnerSOID(GCMessageRead &messageRead, uint64_t steamId, std::optional<GCMessageWrite> &)
{
    T message;
    if (!messageRead.ReadProtobuf(message))
    {
        P2P_PRINT("[P2P][ERROR] SOCache request parse failed steamid=%llu type=%u\n", steamId, messageRead.TypeUnmasked());
        Platform::Print("ValidateMessageOwnerSOID %llu: parsing failed\n", steamId);
        return false;
    }

    if (message.owner_soid().type() != SoIdTypeSteamId
        || message.owner_soid().id() != steamId)
    {
        P2P_PRINT("[P2P][ERROR] HandleNetMessage rejected steamid=%llu type=%u reason=owner soid mismatch (message has %llu)\n",
            steamId, messageRead.TypeUnmasked(), static_cast<uint64_t>(message.owner_soid().id()));
        Platform::Print("ValidateMessageOwnerSOID %llu: steam id mismatch (message has %llu)\n",
            steamId, message.owner_soid().id());
        return false;
    }

    return true;
}

// FIXME: made up
constexpr int MaxServerSOCacheItems = 64;

static bool RemoveUnequippedItems(CMsgSOCacheSubscribed &message, int &itemCount)
{
    bool modified = false;

    for (auto it = message.mutable_objects()->begin(); it != message.mutable_objects()->end(); it++)
    {
        if (it->type_id() != SOTypeItem)
        {
            continue;
        }

        for (auto obj = it->mutable_object_data()->begin(); obj != it->mutable_object_data()->end(); )
        {
            CSOEconItem item;
            if (!item.ParseFromString(*obj) || !item.equipped_state_size())
            {
                obj = it->mutable_object_data()->erase(obj);
                modified = true;
            }
            else
            {
                obj++;
                itemCount++;
            }
        }
    }

    return modified;
}

template<>
bool ValidateMessageOwnerSOID<CMsgSOCacheSubscribed>(GCMessageRead &messageRead, uint64_t steamId, std::optional<GCMessageWrite> &sanitized)
{
    CMsgSOCacheSubscribed message;
    if (!messageRead.ReadProtobuf(message))
    {
        P2P_PRINT("[P2P][ERROR] SOCache parse failed steamid=%llu type=%u\n", steamId, messageRead.TypeUnmasked());
        Platform::Print("ValidateMessageOwnerSOID %llu: parsing failed\n", steamId);
        return false;
    }

    if (message.owner_soid().type() != SoIdTypeSteamId
        || message.owner_soid().id() != steamId)
    {
        P2P_PRINT("[P2P][ERROR] SOCache rejected steamid=%llu reason=owner soid mismatch (message has %llu)\n",
            steamId, static_cast<uint64_t>(message.owner_soid().id()));
        Platform::Print("ValidateMessageOwnerSOID %llu: steam id mismatch (message has %llu)\n",
            steamId, message.owner_soid().id());
        return false;
    }

    size_t oldSize = message.ByteSizeLong();

    int itemCount = 0;
    bool modified = RemoveUnequippedItems(message, itemCount);

    P2P_PRINT("[P2P][SERVER] SOCache parsed steamid=%llu items=%d (equipped, after the original filter) removed_unequipped=%d\n",
        steamId, itemCount, modified ? 1 : 0);

    if (itemCount > MaxServerSOCacheItems)
    {
        P2P_PRINT("[P2P][ERROR] SOCache rejected steamid=%llu reason=%d items > max %d\n", steamId, itemCount, MaxServerSOCacheItems);
        Platform::Print("Client %llu socache has %d items (max allowed %d), ignoring\n", steamId, itemCount, MaxServerSOCacheItems);
        return false;
    }

    if (modified)
    {
        Platform::Print("SOCache from %llu had to be cleaned up (%zu -> %zu bytes)\n", steamId, oldSize, message.ByteSizeLong());
        sanitized.emplace(k_ESOMsg_CacheSubscribed, message);
    }

    return true;
}

void ServerGC::HandleNetMessage(uint64_t steamId, const void *data, uint32_t size)
{
    if (m_testRoster && m_connectedClients.insert(steamId).second && m_connectedClients.size() == 1)
    {
        // TEST ONLY: first real client made it onto the server, stop refreshing the reservation
        m_testRoster->OnMatchStarted();
    }

    Platform::Print("HandleNetMessage: %llu, %u bytes\n", steamId, size);

    P2P_PRINT("[P2P][SERVER] HandleNetMessage from=%llu type=%u size=%u\n", steamId, P2PPeekMaskedType(data, size), size);

    GCMessageRead validate{ 0, data, size };
    if (!validate.IsValid())
    {
        P2P_PRINT("[P2P][ERROR] HandleNetMessage rejected message from=%llu size=%u reason=invalid GC message header\n", steamId, size);
        assert(false);
        return;
    }

    if (!validate.IsProtobuf())
    {
        P2P_PRINT("[P2P][ERROR] HandleNetMessage rejected message from=%llu type=%u reason=not protobuf\n", steamId,
            validate.TypeUnmasked());
        // all the allowed messages are protobuf based
        Platform::Print("ServerGC: ignoring non protobuf message %u from %llu\n",
            validate.TypeUnmasked(), steamId);
        return;
    }

    // validate the type and contents
    bool isValid = false;
    std::optional<GCMessageWrite> sanitized;

    switch (validate.TypeUnmasked())
    {
    case k_ESOMsg_Create:
    case k_ESOMsg_Update:
    case k_ESOMsg_Destroy:
        isValid = ValidateMessageOwnerSOID<CMsgSOSingleObject>(validate, steamId, sanitized);
        break;

    case k_ESOMsg_CacheSubscribed:
        P2P_PRINT("[P2P][SERVER] SOCache received from=%llu size=%u\n", steamId, size);
        isValid = ValidateMessageOwnerSOID<CMsgSOCacheSubscribed>(validate, steamId, sanitized);
        break;

    case k_ESOMsg_UpdateMultiple:
        isValid = ValidateMessageOwnerSOID<CMsgSOMultipleObjects>(validate, steamId, sanitized);
        break;

    case k_EMsgGCItemAcknowledged:
        isValid = true;
        break;
    }

    if (!isValid)
    {
        P2P_PRINT("[P2P][ERROR] HandleNetMessage rejected message from=%llu type=%u (unknown type or failed validation)\n",
            steamId, validate.TypeUnmasked());
        Platform::Print("ServerGC: ignoring net message %u from %llu\n",
            validate.TypeUnmasked(), steamId);
        return;
    }

    if (!m_sentWelcome)
    {
        // FIXME: ideally we'd sent this on steam logon, instead of on demand...
        Platform::Print("Sending server welcome due to net message\n");
        SendServerWelcome();
    }

    if (sanitized.has_value())
    {
        // pass the sanitized message
        P2P_PRINT("[P2P][SERVER] forwarding sanitized message to server.dll steamid=%llu type=%u size=%u\n", steamId,
            sanitized->TypeMasked() & ~ProtobufMask, static_cast<uint32_t>(sanitized->Size()));
        PostToHost(HostEvent::Message, sanitized->TypeMasked(), sanitized->Data(), sanitized->Size());
    }
    else
    {
        // otherwise the old message was fine
        P2P_PRINT("[P2P][SERVER] forwarding message to server.dll steamid=%llu type=%u size=%u\n", steamId,
            validate.TypeUnmasked(), size);
        PostToHost(HostEvent::Message, validate.TypeMasked(), data, size);
    }
}

void ServerGC::SendServerWelcome()
{
    // we don't care about anything in this message, just reply

    CMsgCStrike15Welcome csWelcome;
    csWelcome.set_gscookieid(GameServerCookieId);

    CMsgClientWelcome welcome;
    welcome.set_version(0);
    welcome.set_game_data(csWelcome.SerializeAsString());
    welcome.set_rtime32_gc_welcome_timestamp(static_cast<uint32_t>(time(nullptr)));

    GCMessageWrite write{ k_EMsgGCServerWelcome, welcome };
    PostToHost(HostEvent::Message, write.TypeMasked(), write.Data(), write.Size());

    m_sentWelcome = true;
}

// EXPERIMENTAL: makes this dedicated/listen server accept A2S_RESERVE_CHECK for the same fixed
// GameServerCookieId our own ClientGC hands out in MatchmakingGC2ClientReserve (gc_client.cpp)
// and ClientRequestJoinServerData -- see RESEARCH_FINDINGS.md #26-#32. There's no real backend/9105
// telling us when to reserve, so we just arm the reservation unconditionally as soon as the local
// server.dll says hello to us -- any client presenting the same hardcoded cookie can then pass the
// reservation check. GameServerCookieId is a compile-time constant shared by construction: both
// ClientGC and ServerGC are built into the same csgo_gc.dll, so as long as the identical DLL build
// is deployed to both the client and this server, the cookie is guaranteed identical -- no separate
// config parameter needed.
//
// Payload format is the confirmed-minimal 'G' form from RESEARCH_FINDINGS.md #27/#29.3
// ("G<cookie_hex>,<matchid_hex>,1:", no player-list brackets -- the source explicitly documents
// this form as not carrying a player list, and we have no real account/match data on the server
// side to put there anyway). match_id has no real backing value here either, so it falls back to
// the cookie itself, same as the client side and the same fallback project446 uses (#28.2).
// The actual IVEngineServer::ReserveServerForQueuedGame(...) call happens on the main thread via
// the existing HostEvent::ReserveServerForQueuedGame bridge (steam_hook.cpp), reusing
// ResolveVEngineServer()/DispatchReserveServerForQueuedGame() as-is -- see RESEARCH_FINDINGS.md #32.
//
// UPDATE (#59/#60): the engine drops a plain reservation ~21-26 s after it was made (sv_mmqueue_reservation_timeout), so
// the single reservation of the first ServerHello left every later search with 0x25 awaiting=127 ("Failed to connect to
// the match"). A dedicated server without -gc_mode now hands the reservation to ReservationKeepAlive::Lease, which the
// host thread's per-frame pump (steam_hook.cpp) drives: it reserves at once (HostEvent::ReservationKeepAlive) and again
// every 8 s while the server is in use or recently was. A listen server is not reserved at all (#61).
void ServerGC::ReserveServerForOurCookie()
{
    // TEST ONLY: once the Accept test roster runs it owns the reservation (refresh included)
    if (m_testRoster || m_testWaitingForPlayer || StartAcceptTestRoster())
    {
        return;
    }

    if (m_dedicated)
    {
        // classic dedicated server (RESEARCH_FINDINGS.md #59/#60): the engine drops a plain reservation ~21-26 s after
        // it was made, so it is not made once but kept alive by the host thread's per-frame pump (steam_hook.cpp),
        // which reserves right away as well -- same payload, same ReserveServerForQueuedGame, same host thread
        Platform::Print("[MM] Queueing server reservation keep-alive on host thread\n");
        PostToHost(HostEvent::ReservationKeepAlive, GameServerCookieId, nullptr, 0);
        return;
    }

    // Listen server (Training / bots / a local game of csgo.exe): never reserved (RESEARCH_FINDINGS.md #61). Nobody is
    // assigned to it (the backend hands out dedicated servers), and a reservation cookie on the engine's `sv` makes
    // CBaseServer::ConnectClient reject the local player (his cl_session is 0): "mismatching cookie from loopback" ->
    // "Invalid user info". This point is only reached once the listen server's game server is logged on to Steam, which is
    // why it was not the cause of the observed failures (client side reservation, see ClientGC::StartServerFlow).
    Platform::Print("[MM] listen server: no matchmaking reservation\n");
}

// TEST ONLY: -gc_mode <mode> on the srcds command line switches the reservation
// from the plain 'G' (awaiting always 0, no Accept) to a queued 'Q' roster of the mode's required players (mm_modes.h):
// the real player plus deterministic fake participants that exist only as roster entries and answer through real
// 0x21 datagrams (AcceptTest::FakeRoster). One srcds serves ONE mode (the roster is built once per cookie); run one
// srcds per mode. The real player's AccountID is learned automatically from the first 0x21 it sends (its SteamID
// comes from ISteamUser::GetSteamID() on the client). The address the fakes send to is the srcds' own (-ip or loopback).
// Which srcds a player is sent to is decided by the Java backend (RESEARCH_FINDINGS.md #49), not by this.
// Returns false (=> old 'G' behavior) when the mode is unset or not an accept mode.
bool ServerGC::StartAcceptTestRoster()
{
    const GCConfig &config = GetConfig();
    if (config.TestAcceptMode().empty())
    {
        return false;
    }

    const MM::GameMode *mode = MM::FindGameModeByName(config.TestAcceptMode());
    if (!mode || !mode->acceptRequired || !mode->supported || mode->requiredPlayers == 0)
    {
        Platform::Print("[MM-ACCEPT] -gc_mode '%.*s' is not a supported accept mode "
            "(competitive/wingman/dangerzone/scrimcomp5v5), using the plain reservation\n",
            static_cast<int>(config.TestAcceptMode().size()), config.TestAcceptMode().data());
        return false;
    }

    m_testMode = mode;

    Platform::Print("[MM-ACCEPT] srcds mode=%s: required players (roster) = %u, srcds should run game_type %s game_mode %s "
        "(gamemodes.txt maxplayers %u), port %u\n",
        mode->name, mode->requiredPlayers, mode->serverGameType, mode->serverGameMode, mode->serverMaxPlayers,
        config.DedicatedServerPort());

    // -ip/-port is where the game server listens; the backend may know it under another address (-backend_ip/-backend_port)
    if (config.BackendServerAddress() != config.DedicatedServerAddress() || config.BackendServerPort() != config.DedicatedServerPort())
    {
        Platform::Print("[MM-ACCEPT] game server listens on %s:%u, the backend roster is asked for %s:%u (-backend_ip/-backend_port)\n",
            config.DedicatedServerAddress().c_str(), config.DedicatedServerPort(), config.BackendServerAddress().c_str(),
            config.BackendServerPort());
    }

    // learn it from the first 0x21 of the real client; its retries (every ~1 s for the reservation timeout) succeed
    // as soon as the roster is armed
    m_testWaitingForPlayer = true;
    AcceptTest::SetServerSniff(
        [](void *context, uint32_t accountId)
        {
            static_cast<ServerGC *>(context)->PostToGC(GCEvent::TestRealPlayerSeen, accountId, nullptr, 0);
        },
        this, GameServerCookieId);

    Platform::Print("[MM-ACCEPT] waiting for the real player's first 0x21 to learn its AccountID\n");

    // The backend is the source of truth for the roster of a test match (RESEARCH_FINDINGS.md #55): ask it, once a
    // second, what the match on this server consists of. The sniff above stays as the fallback (backend not
    // reachable, no match for this server, a roster that is not the mode's size).
    RosterFeed::Controller::Host host;
    host.arm = [this](const std::vector<uint32_t> &participants, const std::string &source, bool unreserveFirst)
    {
        ArmRoster(participants, source, unreserveFirst);
    };
    host.confirm = [this](const std::string &matchId)
    {
        if (m_rosterPoller)
        {
            m_rosterPoller->Confirm(matchId);
        }
    };
    host.release = [this](const std::string &matchId) { ReleaseRoster(matchId); };
    host.playerOnServer = [this] { return !m_connectedClients.empty(); };
    m_rosterController = std::make_unique<RosterFeed::Controller>(mode->name, mode->requiredPlayers, std::move(host));

    if (BackendClient::Enabled() && !m_rosterPoller)
    {
        m_rosterPoller = std::make_unique<RosterFeed::Poller>(config.BackendServerAddress(), config.BackendServerPort(),
            [this](const RosterFeed::Snapshot &snapshot)
            {
                const std::string text = RosterFeed::Serialize(snapshot);
                PostToGC(GCEvent::BackendRoster, 0, text.data(), static_cast<uint32_t>(text.size()));
            },
            [this](const SkinSync::MatchSnapshots &snapshots)
            {
                {
                    std::lock_guard lock{ m_skinMutex };
                    m_pendingSkins = snapshots;
                }
                PostToGC(GCEvent::BackendSkins, 0, nullptr, 0);
            });
    }

    return true;
}

void ServerGC::CreateTestRoster(uint32_t realAccountId)
{
    const GCConfig &config = GetConfig();

    AcceptTest::FakeRoster::Params params;
    params.cookie = GameServerCookieId;
    params.realAccountId = realAccountId;
    params.rosterSize = m_testMode->requiredPlayers;
    params.modeName = m_testMode->name;
    params.serverAddress = config.DedicatedServerAddress();
    params.serverPort = config.DedicatedServerPort();
    params.acceptDelayMs = config.TestFakeAcceptDelayMs();
    params.source = "legacy (first 0x21 of the real player)";
    params.onFakesReady = [this](bool ready) { PostToGC(GCEvent::TestFakesReady, ready ? 1 : 0, nullptr, 0); };

    // the same participants the driver builds for itself, so a backend roster of the same content is recognized
    std::vector<uint32_t> participants{ realAccountId };
    for (uint32_t i = 1; i < params.rosterSize; i++)
    {
        participants.push_back(AcceptTest::FakeAccountId(i));
    }

    // ReserveServerForQueuedGame has to run on the main thread, so it goes through the usual host event queue
    m_testRoster = std::make_unique<AcceptTest::FakeRoster>(params,
        [this](const std::string &payload)
        {
            PostToHost(HostEvent::ReserveServerForQueuedGame, 0, payload.data(), static_cast<uint32_t>(payload.size()));
        });

    if (m_rosterController)
    {
        m_rosterController->OnLegacyArmed(participants);
    }
}

void ServerGC::OnTestRealPlayerSeen(uint32_t accountId)
{
    if (m_testRoster)
    {
        if (m_rosterController && !m_rosterController->InArmedRoster(accountId))
        {
            Platform::Print("[MM-ACCEPT] another real player (AccountID %u) sent 0x21, it is not in the armed roster (%s) -- ignored\n",
                accountId, m_rosterController->ArmedSource().c_str());
        }

        return;
    }

    if (!m_testWaitingForPlayer || !m_testMode)
    {
        return;
    }

    if (m_rosterController && m_rosterController->UsingBackendRoster())
    {
        // the backend has a complete roster for this server and it is armed by the roster controller (or postponed
        // while another player is on the server): the sniffed player belongs to it, no need for the legacy roster
        Platform::Print("[MM-ACCEPT] real player %u sent 0x21, the backend's roster (match %s) is in charge\n", accountId,
            m_rosterController->MatchId().c_str());
        return;
    }

    if (m_rosterPoller && m_rosterController && !m_rosterController->HasSnapshot())
    {
        // A fresh srcds: the first 0x21 is here before the backend answered its first roster poll. Arming the legacy roster now
        // would give this match ONE real player (whoever sent the first 0x21) and drop the others, and the backend's roster would
        // then replace it after a 12 s unreserve: no popup on the first search, a normal one on the second (RESEARCH_FINDINGS.md
        // #66). Wait for the answer (OnBackendRoster decides).
        m_testPendingSniff = accountId;
        Platform::Print("[MM-ACCEPT] real player %u sent 0x21 before the backend answered the first roster poll: waiting for it\n",
            accountId);
        return;
    }

    Platform::Print("[MM-ACCEPT] real player detected from its 0x21: AccountID %u, arming the roster\n", accountId);
    m_testWaitingForPlayer = false;
    m_testRealAccountId = accountId;
    CreateTestRoster(accountId);
}

// ---- backend-driven roster (server_roster.h, RESEARCH_FINDINGS.md #55) ------------------------------------------

// the poller's answer changed
// The backend delivered the EquippedSkinSnapshots of the match on this server (GET /servers/roster -> skin_snapshots). They are
// kept per player for the weapon/skin phase; nothing is applied here. Only safe identifiers are logged.
void ServerGC::OnBackendSkins()
{
    SkinSync::MatchSnapshots skins;
    {
        std::lock_guard lock{ m_skinMutex };
        skins = std::move(m_pendingSkins);
        m_pendingSkins = {};
    }

    if (skins.matchId.empty())
    {
        if (!m_matchSkins.matchId.empty())
        {
            Platform::Print("[SKIN_SYNC] match %s is gone: its %zu snapshot(s) are dropped\n", m_matchSkins.matchId.c_str(),
                m_matchSkins.players.size());
        }
        m_matchSkins = {};
        return;
    }

    Platform::Print("[SKIN_SYNC] snapshot delivered to server: match=%s players_with_snapshot=%zu players_without=%zu\n",
        skins.matchId.c_str(), skins.players.size(), skins.missing.size());
    for (const SkinSync::PlayerSnapshot &player : skins.players)
    {
        Platform::Print("[SKIN_SYNC]   account=%u steamid=%llu items=%zu\n", player.accountId,
            static_cast<unsigned long long>(player.steamId64), player.items.size());
        for (const SkinSync::Item &item : player.items)
        {
            Platform::Print("[SKIN_SYNC]     %s\n", SkinSync::Describe(item).c_str());
        }
    }
    for (uint32_t account : skins.missing)
    {
        Platform::Print("[SKIN_SYNC]   account=%u has no snapshot (default skins)\n", account);
    }

    m_matchSkins = std::move(skins);

    // a snapshot that arrived after its player connected is applied now (once per connection)
    for (uint64_t steamId : std::vector<uint64_t>(m_authenticated.begin(), m_authenticated.end()))
    {
        if (!m_skinsApplied.count(steamId))
        {
            ApplySkins(steamId);
        }
    }
}

void ServerGC::OnClientAuthenticated(uint64_t steamId)
{
    m_authenticated.insert(steamId);
    ApplySkins(steamId);
}

// Phase D: hands the equipped items of the player that connected as `steamId` to the game server (server.dll) as the SOCache it
// reads a player's items from -- the same message a client's P2P SOCache used to be turned into, now built from the backend's
// snapshot (MatchSkins()). The player is matched by SteamID/account id, never by roster position; every item keeps the equipped
// slot of the snapshot, so the knife stays the knife. What server.dll does with it afterwards is outside the project's view.
void ServerGC::ApplySkins(uint64_t steamId)
{
    const SkinSync::ApplyPlan plan = SkinSync::PlanForPlayer(m_matchSkins, steamId);
    Platform::Print("[SKIN_SYNC] applying snapshot: steamid=%llu account=%u match=%s\n", static_cast<unsigned long long>(steamId),
        plan.accountId, m_matchSkins.matchId.empty() ? "(none yet)" : m_matchSkins.matchId.c_str());

    if (!plan.playerMatched)
    {
        // either the player has no snapshot, or the roster has not reached this server yet (it is applied when it does)
        Platform::Print("[SKIN_SYNC] application result=skipped (%s)\n", plan.reason.c_str());
        return;
    }

    Platform::Print("[SKIN_SYNC] player matched: account=%u steamid=%llu\n", plan.accountId, static_cast<unsigned long long>(plan.steamId64));
    for (const std::string &rejected : plan.rejected)
    {
        Platform::Print("[SKIN_SYNC] item rejected: %s\n", rejected.c_str());
    }

    if (plan.items.empty())
    {
        Platform::Print("[SKIN_SYNC] application result=skipped (%s)\n", plan.reason.c_str());
        m_skinsApplied.insert(steamId);   // nothing to apply for this connection
        return;
    }

    for (const SkinSync::Item &item : plan.items)
    {
        Platform::Print("[SKIN_SYNC] applying item: %s\n", SkinSync::Describe(item).c_str());
        Platform::Print("[SKIN_SYNC] def_index=%u\n", item.defIndex);
        if (item.hasPaint)
        {
            Platform::Print("[SKIN_SYNC] paint_kit=%u paint_seed=%u paint_wear=%.9g\n", item.paintKit, item.paintSeed,
                static_cast<double>(item.paintWear));
        }
        for (const SkinSync::Equipped &equip : item.equipped)
        {
            Platform::Print("[SKIN_SYNC] equipped_slot=class %u slot %u\n", equip.classId, equip.slotId);
        }
    }

    if (!m_skinSchema)
    {
        m_skinSchema = std::make_unique<ItemSchema>();
    }

    CMsgSOCacheSubscribed message;
    const size_t written = Inventory::BuildServerCache(*m_skinSchema, plan, GetConfig().Level(), message);

    if (!m_sentWelcome)
    {
        SendServerWelcome();
    }

    GCMessageWrite write{ k_ESOMsg_CacheSubscribed, message };
    PostToHost(HostEvent::Message, write.TypeMasked(), write.Data(), write.Size());
    m_skinsApplied.insert(steamId);

    Platform::Print("[SKIN_SYNC] application result=posted to server.dll as SOCache: %zu item(s), %u bytes, owner %llu\n", written,
        write.Size(), static_cast<unsigned long long>(steamId));
}

void ServerGC::OnBackendRoster(const std::string &text)
{
    RosterFeed::Snapshot snapshot;
    if (!RosterFeed::Deserialize(text, snapshot))
    {
        Platform::Print("[MM-ACCEPT] backend roster: unreadable poller event ignored\n");
        return;
    }

    if (m_rosterController)
    {
        m_rosterController->OnSnapshot(snapshot);
    }

    // a player whose 0x21 came before the first answer: the backend's roster is in charge now, or (no match for it / backend
    // down) the legacy roster is armed for him after all
    if (m_testPendingSniff && !m_testRoster)
    {
        const uint32_t pending = m_testPendingSniff;
        m_testPendingSniff = 0;
        OnTestRealPlayerSeen(pending);
    }
}

// the backend has no match on this server any more (cancelled at the Accept / ended) and nobody is on it: drop the reservation
// and stop keeping it alive, RESEARCH_FINDINGS.md #63. The fake driver stays as it is (paused) until the next match replaces it.
void ServerGC::ReleaseRoster(const std::string &matchId)
{
    Platform::Print("[MM-ACCEPT] releasing the reservation of match %s\n", matchId.c_str());
    if (m_testRoster)
    {
        m_testRoster->Release();
    }
}

// arms the reservation with the backend's roster: the real players' AccountIDs and the fake ones it assigned
void ServerGC::ArmRoster(const std::vector<uint32_t> &participants, const std::string &source, bool unreserveFirst)
{
    const GCConfig &config = GetConfig();

    uint32_t real = 0;
    for (uint32_t id : participants)
    {
        if (!AcceptTest::IsFakeAccountId(id))
        {
            real = id;
            break;
        }
    }

    Platform::Print("[MM-ACCEPT] arming the roster of %s (%zu participants)%s\n", source.c_str(), participants.size(),
        unreserveFirst ? ", replacing the roster that is armed" : "");

    m_testRoster.reset(); // stops its thread

    AcceptTest::FakeRoster::Params params;
    params.cookie = GameServerCookieId;
    params.realAccountId = real;
    params.rosterSize = static_cast<uint32_t>(participants.size());
    params.modeName = m_testMode->name;
    params.serverAddress = config.DedicatedServerAddress();
    params.serverPort = config.DedicatedServerPort();
    params.acceptDelayMs = config.TestFakeAcceptDelayMs();
    params.participants = participants;
    params.source = source;
    params.unreserveFirst = unreserveFirst;
    params.onFakesReady = [this](bool ready) { PostToGC(GCEvent::TestFakesReady, ready ? 1 : 0, nullptr, 0); };

    m_testWaitingForPlayer = false;
    m_testRealAccountId = real;

    m_testRoster = std::make_unique<AcceptTest::FakeRoster>(params,
        [this](const std::string &payload)
        {
            PostToHost(HostEvent::ReserveServerForQueuedGame, 0, payload.data(), static_cast<uint32_t>(payload.size()));
        });
}

void ServerGC::IncrementKillCountAttribute(GCMessageRead &messageRead)
{
    CMsgIncrementKillCountAttribute message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgIncrementKillCountAttribute failed, ignoring\n");
        return;
    }

    // just forward it to the killer
    GCMessageWrite messageWrite{ k_EMsgGC_IncrementKillCountAttribute, message };
    CSteamID killerId{ message.killer_account_id(), k_EUniversePublic, k_EAccountTypeIndividual };
    PostToHost(HostEvent::NetMessage, killerId.ConvertToUint64(), messageWrite.Data(), messageWrite.Size());
}
