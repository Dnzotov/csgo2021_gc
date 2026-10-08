#pragma once

#include <steam/isteamnetworkingmessages.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

constexpr int NetMessageSendFlags = k_nSteamNetworkingSend_Reliable;
constexpr int NetMessageChannel = 7;

// NOTE: these are used as gc message types!
// if they overlap with the game's gc messages, we're doomed
enum ENetworkMsg : uint32_t
{
    // sent by the server to client when they connect, data is the auth ticket
    k_EMsgNetworkConnect = (1u << 31) - 1,
};

// ---- P2P/SOCache diagnostics (Step 0, research/p2p_socache_step0.md): passive helpers, no behavior of their own ----

// "HH:MM:SS.mmm" local time, so server and client logs can be lined up
inline std::string P2PTime()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    time_t t = system_clock::to_time_t(now);
    tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min, local.tm_sec,
        static_cast<int>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000));
    return buffer;
}

// Platform::Print with a time prefix; every use below starts its format with "[P2P][...]"
#define P2P_PRINT(format, ...) Platform::Print("%s " format, P2PTime().c_str(), __VA_ARGS__)

inline const char *P2PConnectionStateName(int state)
{
    switch (state)
    {
    case k_ESteamNetworkingConnectionState_None: return "None";
    case k_ESteamNetworkingConnectionState_Connecting: return "Connecting";
    case k_ESteamNetworkingConnectionState_FindingRoute: return "FindingRoute";
    case k_ESteamNetworkingConnectionState_Connected: return "Connected";
    case k_ESteamNetworkingConnectionState_ClosedByPeer: return "ClosedByPeer";
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: return "ProblemDetectedLocally";
    default: return "Unknown";
    }
}

inline const char *P2PResultName(int result)
{
    switch (result)
    {
    case k_EResultOK: return "OK";
    case k_EResultFail: return "Fail";
    case k_EResultNoConnection: return "NoConnection";
    case k_EResultInvalidParam: return "InvalidParam";
    case k_EResultLimitExceeded: return "LimitExceeded";
    case k_EResultIgnored: return "Ignored";
    case k_EResultBusy: return "Busy";
    default: return "Other";
    }
}

// first 4 bytes of a GC message = masked type (see gc_message.h); 0 if the buffer is too short
inline uint32_t P2PPeekMaskedType(const void *data, uint32_t size)
{
    uint32_t type = 0;
    if (data && size >= sizeof(type))
    {
        memcpy(&type, data, sizeof(type));
    }
    return type;
}
