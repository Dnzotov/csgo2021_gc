#include "stdafx.h"
#include <algorithm>

#include "gc_server.h"
#include "backend_client.h"
#include "gc_const.h"
#include "gc_const_csgo.h"
#include "config.h"
#include "graffiti.h"

// yuck!! needed for CSteamID (construct full id from account id)
#include "steam/steamclientpublic.h"

ServerGC::ServerGC()
{
    // also called from ClientGC's constructor
    Graffiti::Initialize();

    StartThread();

    Platform::Print("ServerGC spawned\n");
}

ServerGC::~ServerGC()
{
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
        Platform::Print("ValidateMessageOwnerSOID %llu: parsing failed\n", steamId);
        return false;
    }

    if (message.owner_soid().type() != SoIdTypeSteamId
        || message.owner_soid().id() != steamId)
    {
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
        Platform::Print("ValidateMessageOwnerSOID %llu: parsing failed\n", steamId);
        return false;
    }

    if (message.owner_soid().type() != SoIdTypeSteamId
        || message.owner_soid().id() != steamId)
    {
        Platform::Print("ValidateMessageOwnerSOID %llu: steam id mismatch (message has %llu)\n",
            steamId, message.owner_soid().id());
        return false;
    }

    size_t oldSize = message.ByteSizeLong();

    int itemCount = 0;
    bool modified = RemoveUnequippedItems(message, itemCount);

    if (itemCount > MaxServerSOCacheItems)
    {
        Platform::Print("Client %llu socache has %d items (max allowed %d), ignoring\n", itemCount, MaxServerSOCacheItems);
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

    GCMessageRead validate{ 0, data, size };
    if (!validate.IsValid())
    {
        assert(false);
        return;
    }

    if (!validate.IsProtobuf())
    {
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
        PostToHost(HostEvent::Message, sanitized->TypeMasked(), sanitized->Data(), sanitized->Size());
    }
    else
    {
        // otherwise the old message was fine
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
void ServerGC::ReserveServerForOurCookie()
{
    // TEST ONLY: once the Accept test roster runs it owns the reservation (refresh included)
    if (m_testRoster || m_testWaitingForPlayer || StartAcceptTestRoster())
    {
        return;
    }

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "G%llx,%llx,1:",
        static_cast<unsigned long long>(GameServerCookieId),
        static_cast<unsigned long long>(GameServerCookieId));

    Platform::Print("[MM] Queueing server reservation on host thread\n");
    PostToHost(HostEvent::ReserveServerForQueuedGame, 0, buffer, static_cast<uint32_t>(strlen(buffer)));
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
    host.playerOnServer = [this] { return !m_connectedClients.empty(); };
    m_rosterController = std::make_unique<RosterFeed::Controller>(mode->name, mode->requiredPlayers, std::move(host));

    if (BackendClient::Enabled() && !m_rosterPoller)
    {
        m_rosterPoller = std::make_unique<RosterFeed::Poller>(config.DedicatedServerAddress(), config.DedicatedServerPort(),
            [this](const RosterFeed::Snapshot &snapshot)
            {
                const std::string text = RosterFeed::Serialize(snapshot);
                PostToGC(GCEvent::BackendRoster, 0, text.data(), static_cast<uint32_t>(text.size()));
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

    Platform::Print("[MM-ACCEPT] real player detected from its 0x21: AccountID %u, arming the roster\n", accountId);
    m_testWaitingForPlayer = false;
    m_testRealAccountId = accountId;
    CreateTestRoster(accountId);
}

// ---- backend-driven roster (server_roster.h, RESEARCH_FINDINGS.md #55) ------------------------------------------

// the poller's answer changed
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
