#include "stdafx.h"
#include "config.h"
#include "keyvalue.h"
#include "launch_args.h"
#include "random.h"

// "-name value" from the process command line
static std::string CommandLineValue(std::string_view name)
{
    return LaunchArgs::Value(Platform::CommandLine(), name);
}

constexpr const char *ConfigFilePath = "csgo_gc/config.txt";

const GCConfig &GetConfig()
{
    static GCConfig instance;
    return instance;
}

GCConfig::GCConfig()
{
    KeyValue config{ "config" };

    // TEST ONLY: -gc_mode <mode> on the (srcds) command line
    auto applyCommandLine = [this]
    {
        std::string mode = CommandLineValue("-gc_mode");
        if (!mode.empty())
        {
            m_testAcceptMode = mode;
        }
    };

    if (!config.ParseFromFile(ConfigFilePath))
    {
        applyCommandLine();
        return;
    }

    m_logOutput = config.GetNumber("log_output", m_logOutput);

    m_appIdOverride = config.GetNumber("appid_override", m_appIdOverride);
    m_showCsgoGCServersOnly = config.GetNumber("show_csgo_gc_servers_only", m_showCsgoGCServersOnly);

    const KeyValue *ranks = config.GetSubkey("ranks");
    if (ranks)
    {
        m_competitiveRank = ranks->GetNumber("competitive_rank", m_competitiveRank);
        m_competitiveWins = ranks->GetNumber("competitive_wins", m_competitiveWins);

        m_wingmanRank = ranks->GetNumber("wingman_rank", m_wingmanRank);
        m_wingmanWins = ranks->GetNumber("wingman_wins", m_wingmanWins);

        m_dangerZoneRank = ranks->GetNumber("dangerzone_rank", m_dangerZoneRank);
        m_dangerZoneWins = ranks->GetNumber("dangerzone_wins", m_dangerZoneWins);
    }

    m_destroyUsedItems = config.GetNumber("destroy_used_items", m_destroyUsedItems);

    const KeyValue *rarityWeights = config.GetSubkey("rarity_weights");
    if (rarityWeights)
    {
        m_rarityWeights.clear();
        m_rarityWeights.reserve(rarityWeights->SubkeyCount());

        for (const KeyValue &subkey : *rarityWeights)
        {
            RarityWeight weight;
            weight.rarity = FromString<uint32_t>(subkey.Name());
            weight.weight = FromString<float>(subkey.String());
            m_rarityWeights.push_back(weight);
        }
    }

    m_vacBanned = config.GetNumber("vac_banned", m_vacBanned);
    m_commendedFriendly = config.GetNumber("cmd_friendly", m_commendedFriendly);
    m_commendedTeaching = config.GetNumber("cmd_teaching", m_commendedTeaching);
    m_commendedLeader = config.GetNumber("cmd_leader", m_commendedLeader);
    m_level = config.GetNumber("player_level", m_level);
    m_xp = config.GetNumber("player_cur_xp", m_xp);

    const KeyValue *matchmaking = config.GetSubkey("matchmaking");
    if (matchmaking)
    {
        m_testDiag = matchmaking->GetNumber("test_diag", m_testDiag);
        m_reservationIdleSeconds = matchmaking->GetNumber("reservation_idle_seconds", m_reservationIdleSeconds);

        m_backendUrl = std::string(matchmaking->GetString("backend_url", m_backendUrl));
        m_backendApiKey = std::string(matchmaking->GetString("backend_api_key", m_backendApiKey));
    }

    applyCommandLine();
}

uint16_t GCConfig::DedicatedServerPort() const
{
    return LaunchArgs::ResolveServerEndpoints(Platform::CommandLine()).gamePort; // -port, srcds' default 27015
}

std::string GCConfig::DedicatedServerAddress() const
{
    return LaunchArgs::ResolveServerEndpoints(Platform::CommandLine()).gameAddress; // -ip, else loopback
}

std::string GCConfig::BackendServerAddress() const
{
    return LaunchArgs::ResolveServerEndpoints(Platform::CommandLine()).backendAddress; // -backend_ip, else -ip
}

uint16_t GCConfig::BackendServerPort() const
{
    return LaunchArgs::ResolveServerEndpoints(Platform::CommandLine()).backendPort; // -backend_port, else -port
}

float GCConfig::GetRarityWeight(uint32_t rarity) const
{
    for (const RarityWeight &weight : m_rarityWeights)
    {
        if (weight.rarity == rarity)
        {
            return weight.weight;
        }
    }

    return 0;
}
