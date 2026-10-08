#include "stdafx.h"
#include "networking_client.h"
#include "gc_client.h"

NetworkingClient::NetworkingClient(ISteamNetworkingMessages *networkingMessages)
    : m_networkingMessages{ networkingMessages }
    , m_sessionRequest{ this, &NetworkingClient::OnSessionRequest }
    , m_sessionFailed{ this, &NetworkingClient::OnSessionFailed }
{
}

void NetworkingClient::Update(ClientGC *gc)
{
    SteamNetworkingMessage_t *message;
    while (m_networkingMessages->ReceiveMessagesOnChannel(NetMessageChannel, &message, 1))
    {
        uint64_t steamId = message->m_identityPeer.GetSteamID64();

        P2P_PRINT("[P2P][CLIENT] HandleMessage from=%llu type=%u size=%u (ourGs=%llu)\n", steamId,
            P2PPeekMaskedType(message->GetData(), static_cast<uint32_t>(message->GetSize())),
            static_cast<uint32_t>(message->GetSize()), m_serverSteamId);

        // pass 0 as type so it gets parsed from the message
        GCMessageRead messageRead{ 0, message->GetData(), message->GetSize() };
        if (!messageRead.IsValid())
        {
            P2P_PRINT("[P2P][ERROR] HandleMessage rejected from=%llu size=%u reason=invalid GC message header\n", steamId,
                static_cast<uint32_t>(message->GetSize()));
            assert(false);
            message->Release();
            continue;
        }

        if (HandleMessage(gc, steamId, messageRead))
        {
            // that was an internal message
            message->Release();
            continue;
        }

        // don't pass messages to the gc unless it's our gameserver
        if (!m_serverSteamId || steamId != m_serverSteamId)
        {
            Platform::Print("NetworkingClient: ignored message from %llu (not our gs %llu)\n", steamId, m_serverSteamId);
            P2P_PRINT("[P2P][ERROR] HandleMessage rejected from=%llu type=%u reason=not our game server (%llu)\n", steamId,
                P2PPeekMaskedType(message->GetData(), static_cast<uint32_t>(message->GetSize())), m_serverSteamId);
            message->Release();
            continue;
        }

        // let the gc have a whack at it
        gc->PostToGC(GCEvent::NetMessage, 0, message->GetData(), message->GetSize());

        message->Release();
    }
}

static bool ValidateTicket(std::unordered_map<uint32_t, AuthTicket> &tickets, uint64_t steamId, const void *data, uint32_t size)
{
    for (auto &pair : tickets)
    {
        if (pair.second.buffer.size() == size && !memcmp(pair.second.buffer.data(), data, size))
        {
            pair.second.steamId = steamId;
            return true;
        }
    }

    return false;
}

bool NetworkingClient::HandleMessage(ClientGC *gc, uint64_t steamId, GCMessageRead &message)
{
    if (message.IsProtobuf())
    {
        // internal messages are not protobuf based
        return false;
    }

    uint32_t typeUnmasked = message.TypeUnmasked();
    if (typeUnmasked == k_EMsgNetworkConnect)
    {
        P2P_PRINT("[P2P][CLIENT] NetworkConnect request from=%llu\n", steamId);
        uint32_t ticketSize = message.ReadUint32();
        const void *ticket = message.ReadData(ticketSize);
        if (!message.IsValid())
        {
            Platform::Print("NetworkingClient: ignored connection from %llu (malfored message)\n", steamId);
            P2P_PRINT("[P2P][ERROR] HandleMessage rejected NetworkConnect from=%llu reason=malformed message (ticketSize=%u)\n",
                steamId, ticketSize);
            return true;
        }

        if (!ValidateTicket(m_tickets, steamId, ticket, ticketSize))
        {
            Platform::Print("NetworkingClient: ignored connection from %llu (ticket mismatch)\n", steamId);
            P2P_PRINT("[P2P][ERROR] HandleMessage rejected NetworkConnect from=%llu reason=auth ticket mismatch (ticketSize=%u, known tickets=%zu)\n",
                steamId, ticketSize, m_tickets.size());
            return true;
        }

        Platform::Print("NetworkingClient: sending socache to %llu\n", steamId);
        P2P_PRINT("[P2P][CLIENT] NetworkConnect accepted from=%llu, posting SOCacheRequest\n", steamId);
        m_serverSteamId = steamId;
        gc->PostToGC(GCEvent::SOCacheRequest, 0, nullptr, 0);

        return true;
    }

    return false;
}

void NetworkingClient::SendMessage(const void *data, uint32_t size)
{
    if (!m_serverSteamId)
    {
        // not connected to a server
        P2P_PRINT("[P2P][ERROR] SendMessage not sent size=%u reason=no game server steamid known\n", size);
        return;
    }

    // mikkotodo check return
    SteamNetworkingIdentity identity;
    identity.SetSteamID64(m_serverSteamId);

    P2P_PRINT("[P2P][CLIENT] Sending SOCache type=%u size=%u target=%llu\n", P2PPeekMaskedType(data, size), size, m_serverSteamId);

    [[maybe_unused]] EResult result = m_networkingMessages->SendMessageToUser(
        identity,
        data,
        size,
        NetMessageSendFlags,
        NetMessageChannel);

    P2P_PRINT("[P2P][CLIENT] Send SOCache result=%d(%s)\n", static_cast<int>(result), P2PResultName(result));
    {
        SteamNetConnectionInfo_t info{};
        ESteamNetworkingConnectionState state = m_networkingMessages->GetSessionConnectionInfo(identity, &info, nullptr);
        P2P_PRINT("[P2P][CLIENT] session after send target=%llu state=%s(%d) endReason=%d endDebug='%s'\n", m_serverSteamId,
            P2PConnectionStateName(state), static_cast<int>(state), info.m_eEndReason, info.m_szEndDebug);
    }
    if (result != k_EResultOK)
    {
        P2P_PRINT("[P2P][ERROR] SOCache send failed result=%d(%s) target=%llu\n", static_cast<int>(result), P2PResultName(result),
            m_serverSteamId);
    }

    assert(result == k_EResultOK);
}

void NetworkingClient::SetAuthTicket(uint32_t handle, const void *data, uint32_t size)
{
    AuthTicket &ticket = m_tickets[handle];
    ticket.steamId = 0;
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
    ticket.buffer.assign(bytes, bytes + size);
}

void NetworkingClient::ClearAuthTicket(uint32_t handle)
{
    auto it = m_tickets.find(handle);
    if (it == m_tickets.end())
    {
        assert(false);
        Platform::Print("NetworkingClient: tried to clear a nonexistent auth ticket???\n");
        return;
    }

    if (it->second.steamId)
    {
        Platform::Print("NetworkingClient: closing p2p session with %llu\n", it->second.steamId);

        // we had a session so close the connection
        SteamNetworkingIdentity identity;
        identity.SetSteamID64(it->second.steamId);
        m_networkingMessages->CloseChannelWithUser(identity, NetMessageChannel);

        // was this our current gameserver? if it was, clear it
        if (it->second.steamId == m_serverSteamId)
        {
            Platform::Print("NetworkingClient: clearing gs identity\n");
            m_serverSteamId = 0;
        }
    }

    m_tickets.erase(it);
}

void NetworkingClient::OnSessionRequest(SteamNetworkingMessagesSessionRequest_t *param)
{
    P2P_PRINT("[P2P][SESSION][CLIENT] request from=%llu isGameServer=%d\n", param->m_identityRemote.GetSteamID64(),
        param->m_identityRemote.GetSteamID().BGameServerAccount() ? 1 : 0);

    if (!param->m_identityRemote.GetSteamID().BGameServerAccount())
    {
        // csgo_gc related connections come from gameservers
        return;
    }

    // accept the connection, we should receive the k_EMsgNetworkConnect message
    bool accepted = m_networkingMessages->AcceptSessionWithUser(param->m_identityRemote);
    P2P_PRINT("[P2P][SESSION][CLIENT] AcceptSessionWithUser from=%llu result=%d\n", param->m_identityRemote.GetSteamID64(),
        accepted ? 1 : 0);
    if (!accepted)
    {
        P2P_PRINT("[P2P][ERROR] AcceptSessionWithUser failed from=%llu\n", param->m_identityRemote.GetSteamID64());
    }
}

void NetworkingClient::OnSessionFailed(SteamNetworkingMessagesSessionFailed_t *param)
{
    Platform::Print("NetworkingClient::OnSessionFailed: %s\n", param->m_info.m_szEndDebug);
    P2P_PRINT("[P2P][SESSION][CLIENT] failed remote=%llu state=%s(%d) endReason=%d endDebug='%s' desc='%s'\n",
        param->m_info.m_identityRemote.GetSteamID64(), P2PConnectionStateName(param->m_info.m_eState),
        static_cast<int>(param->m_info.m_eState), param->m_info.m_eEndReason, param->m_info.m_szEndDebug,
        param->m_info.m_szConnectionDescription);
    P2P_PRINT("[P2P][ERROR] session with remote=%llu failed endReason=%d\n", param->m_info.m_identityRemote.GetSteamID64(),
        param->m_info.m_eEndReason);
}
