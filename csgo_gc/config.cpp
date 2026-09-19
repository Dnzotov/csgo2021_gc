#include "stdafx.h"
#include "config.h"
#include "keyvalue.h"
#include "random.h"

// "-name value" from the process command line
static std::string CommandLineValue(std::string_view name)
{
    std::string commandLine = Platform::CommandLine();

    size_t position = 0;
    std::string previous;
    while (position < commandLine.size())
    {
        while (position < commandLine.size() && commandLine[position] == ' ')
        {
            position++;
        }

        size_t end = commandLine.find(' ', position);
        if (end == std::string::npos)
        {
            end = commandLine.size();
        }

        std::string token = commandLine.substr(position, end - position);
        if (previous == name)
        {
            return token;
        }

        previous = token;
        position = end;
    }

    return {};
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

    // TEST ONLY: -gc_mode <mode> on the (srcds) command line wins over matchmaking.test_accept_mode
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
        m_testServerAddress = std::string(matchmaking->GetString("test_server_address", m_testServerAddress));
        m_testServerPort = matchmaking->GetNumber("test_server_port", m_testServerPort);
        m_testAcceptMode = std::string(matchmaking->GetString("test_accept_mode", m_testAcceptMode));
        m_testRealAccountId = matchmaking->GetNumber("test_real_account_id", m_testRealAccountId);
        m_testFakeAcceptDelayMs = matchmaking->GetNumber("test_fake_accept_delay_ms", m_testFakeAcceptDelayMs);
        m_testDiag = matchmaking->GetNumber("test_diag", m_testDiag);

        const KeyValue *ports = matchmaking->GetSubkey("test_server_ports");
        if (ports)
        {
            for (const KeyValue &subkey : *ports)
            {
                m_testServerPorts.emplace_back(std::string(subkey.Name()), FromString<uint16_t>(subkey.String()));
            }
        }
    }

    applyCommandLine();
}

uint16_t GCConfig::TestServerPortForMode(std::string_view modeName) const
{
    for (const auto &entry : m_testServerPorts)
    {
        if (entry.first == modeName)
        {
            return entry.second;
        }
    }

    return m_testServerPort;
}

uint16_t GCConfig::DedicatedServerPort() const
{
    std::string port = CommandLineValue("-port");
    if (!port.empty())
    {
        return FromString<uint16_t>(port);
    }

    return m_testServerPort;
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
