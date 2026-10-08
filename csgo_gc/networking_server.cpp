#include "stdafx.h"
#include "networking_server.h"
#include "gc_message.h"

NetworkingServer::NetworkingServer(ISteamNetworkingMessages *networkingMessages)
    : m_networkingMessages{ networkingMessages }
    , m_sessionRequest{ this, &NetworkingServer::OnSessionRequest }
    , m_sessionFailed{ this, &NetworkingServer::OnSessionFailed }
{
}

bool NetworkingServer::ReceiveMessage(SteamNetworkingMessage_t *&message)
{
    if (!m_networkingMessages->ReceiveMessagesOnChannel(NetMessageChannel, &message, 1))
    {
        return false;
    }

    uint64_t steamId = message->m_identityPeer.GetSteamID64();

    // see if we have a session
    if (!m_clients.Has(steamId))
    {
        Platform::Print("NetworkingServer: ignored message from %llu (no session)\n", steamId);
        P2P_PRINT("[P2P][ERROR] ReceiveMessage dropped message from=%llu size=%u reason=no csgo_gc session\n",
            steamId, static_cast<uint32_t>(message->GetSize()));
        message->Release();
        return false;
    }

    return true;
}

// passive: log the P2P session state with a peer (GetSessionConnectionInfo only reads)
static void LogSessionState(ISteamNetworkingMessages *networkingMessages, uint64_t steamId, const char *when)
{
    SteamNetworkingIdentity identity;
    identity.SetSteamID64(steamId);

    SteamNetConnectionInfo_t info{};
    ESteamNetworkingConnectionState state = networkingMessages->GetSessionConnectionInfo(identity, &info, nullptr);
    P2P_PRINT("[P2P][SERVER] session %s target=%llu state=%s(%d) endReason=%d endDebug='%s'\n",
        when, steamId, P2PConnectionStateName(state), static_cast<int>(state), info.m_eEndReason, info.m_szEndDebug);
}

// helper for SteamNetworkingMessages::SendMessageToUser that attempts to do some kind of error handling
static void SendMessageToUser(ISteamNetworkingMessages *networkingMessages, uint64_t steamId, const void *data, uint32_t size)
{
    SteamNetworkingIdentity identity;
    identity.SetSteamID64(steamId);

    uint32_t type = P2PPeekMaskedType(data, size);
    P2P_PRINT("[P2P][SERVER] SendMessageToUser target=%llu type=%u size=%u channel=%d flags=%d\n",
        steamId, type, size, NetMessageChannel, NetMessageSendFlags);

    EResult result = networkingMessages->SendMessageToUser(
        identity,
        data,
        size,
        NetMessageSendFlags,
        NetMessageChannel);

    P2P_PRINT("[P2P][SERVER] SendMessageToUser result=%d(%s)\n", static_cast<int>(result), P2PResultName(result));
    LogSessionState(networkingMessages, steamId, "after send");

    if (result != k_EResultOK)
    {
        P2P_PRINT("[P2P][ERROR] SendMessageToUser failed target=%llu type=%u result=%d(%s)\n",
            steamId, type, static_cast<int>(result), P2PResultName(result));
        Platform::Print("SendMessageToUser failed for %llu: %d, closing session and trying again\n", steamId, result);

        networkingMessages->CloseChannelWithUser(identity, NetMessageChannel);

        result = networkingMessages->SendMessageToUser(
            identity,
            data,
            size,
            NetMessageSendFlags,
            NetMessageChannel);

        P2P_PRINT("[P2P][SERVER] SendMessageToUser (second attempt of the original logic) result=%d(%s)\n",
            static_cast<int>(result), P2PResultName(result));

        if (result != k_EResultOK)
        {
            // not much we can do in this situation i guess
            P2P_PRINT("[P2P][ERROR] SendMessageToUser second attempt failed target=%llu result=%d(%s)\n",
                steamId, static_cast<int>(result), P2PResultName(result));
            Platform::Print("SendMessageToUser failed for %llu\n", steamId);
        }
    }
}

void NetworkingServer::ClientConnected(uint64_t steamId, const void *ticket, uint32_t ticketSize)
{
    P2P_PRINT("[P2P][SERVER] ClientConnected steamid=%llu authTicketSize=%u\n", steamId, ticketSize);

    if (!m_clients.Add(steamId))
    {
        P2P_PRINT("[P2P][ERROR] ClientConnected steamid=%llu already on the csgo_gc client list, NetworkConnect not sent\n", steamId);
        Platform::Print("got ClientConnected for %llu but they're already on the list! ignoring\n", steamId);
        return;
    }

    // send a message, if the client has csgo_gc installed they will
    // reply with their so cache and we'll add them to our list
    GCMessageWrite messageWrite{ k_EMsgNetworkConnect };
    messageWrite.WriteUint32(ticketSize);
    messageWrite.WriteData(ticket, ticketSize);

    // FIXME: this gets sent when the client is connecting to the server, it's not uncommon for
    // the connection to time out, in which case the player's socache never gets to the server
    SendMessageToUser(m_networkingMessages, steamId, messageWrite.Data(), messageWrite.Size());
}

void NetworkingServer::ClientDisconnected(uint64_t steamId)
{
    LogSessionState(m_networkingMessages, steamId, "at disconnect");
    P2P_PRINT("[P2P][SERVER] ClientDisconnected steamid=%llu\n", steamId);

    if (!m_clients.Remove(steamId))
    {
        P2P_PRINT("[P2P][ERROR] ClientDisconnected steamid=%llu was not on the csgo_gc client list\n", steamId);
        Platform::Print("got ClientDisconnected for %llu but they're not on the list! ignoring\n", steamId);
        return;
    }

    SteamNetworkingIdentity identity;
    identity.SetSteamID64(steamId);
    m_networkingMessages->CloseChannelWithUser(identity, NetMessageChannel);
}

void NetworkingServer::SendMessage(uint64_t steamId, const void *data, uint32_t size)
{
    if (!m_clients.Has(steamId))
    {
        P2P_PRINT("[P2P][ERROR] SendMessage target=%llu not on the csgo_gc client list, message not sent\n", steamId);
        Platform::Print("No csgo_gc session with %llu, not sending message!!!\n", steamId);
        return;
    }

    SendMessageToUser(m_networkingMessages, steamId, data, size);
}

void NetworkingServer::OnSessionRequest(SteamNetworkingMessagesSessionRequest_t *param)
{
    uint64_t steamId = param->m_identityRemote.GetSteamID64();

    P2P_PRINT("[P2P][SESSION][SERVER] request from=%llu knownClient=%d\n", steamId, m_clients.Has(steamId) ? 1 : 0);

    if (!m_clients.Has(steamId))
    {
        Platform::Print("%llu sent a session request, we don't have a csgo_gc session, ignoring...\n", steamId);
        return;
    }

    Platform::Print("%llu sent a session request, we were playing GC with them so accept\n", steamId);

    bool accepted = m_networkingMessages->AcceptSessionWithUser(param->m_identityRemote);
    P2P_PRINT("[P2P][SESSION][SERVER] AcceptSessionWithUser from=%llu result=%d\n", steamId, accepted ? 1 : 0);

    if (!accepted)
    {
        P2P_PRINT("[P2P][ERROR] AcceptSessionWithUser failed from=%llu\n", steamId);
        Platform::Print("AcceptSessionWithUser with %llu failed???\n",
            param->m_identityRemote.GetSteamID64());
    }
}

void NetworkingServer::OnSessionFailed(SteamNetworkingMessagesSessionFailed_t *param)
{
    // don't do anything, rely on the auth session
    P2P_PRINT("[P2P][SESSION][SERVER] failed remote=%llu state=%s(%d) endReason=%d endDebug='%s' desc='%s'\n",
        param->m_info.m_identityRemote.GetSteamID64(), P2PConnectionStateName(param->m_info.m_eState),
        static_cast<int>(param->m_info.m_eState), param->m_info.m_eEndReason, param->m_info.m_szEndDebug,
        param->m_info.m_szConnectionDescription);
    P2P_PRINT("[P2P][ERROR] session with remote=%llu failed endReason=%d\n",
        param->m_info.m_identityRemote.GetSteamID64(), param->m_info.m_eEndReason);
    Platform::Print("OnSessionFailed: %s\n", param->m_info.m_szEndDebug);
}
