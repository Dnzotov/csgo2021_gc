#pragma once

#include "gc_const_csgo.h"
#include "item_schema.h" // rarity constants

struct RarityWeight
{
    uint32_t rarity;
    float weight;
};

// for Platform::Print calls
enum LogOutput
{
    LogOutputNone, // don't output anything
    LogOutputConsole, // game console
    LogOutputFile // game console and gc_log.txt
};

class GCConfig
{
public:
    GCConfig();

    // options used by platform layer (bruh)
    LogOutput GetLogOutput() const { return m_logOutput; }

    // options used by steam hook
    uint32_t AppIdOverride() const { return m_appIdOverride; }
    bool ShowCsgoGCServersOnly() const { return m_showCsgoGCServersOnly; }

    RankId CompetitiveRank() const { return m_competitiveRank; }
    int CompetitiveWins() const { return m_competitiveWins; }
    RankId WingmanRank() const { return m_wingmanRank; }
    int WingmanWins() const { return m_wingmanWins; }
    DangerZoneRankId DangerZoneRank() const { return m_dangerZoneRank; }
    int DangerZoneWins() const { return m_dangerZoneWins; }

    bool DestroyUsedItems() const { return m_destroyUsedItems; }

    bool VacBanned() const { return m_vacBanned; }
    int CommendedFriendly() const { return m_commendedFriendly; }
    int CommendedTeaching() const { return m_commendedTeaching; }
    int CommendedLeader() const { return m_commendedLeader; }
    int Level() const { return m_level; }
    int Xp() const { return m_xp; }

    // EXPERIMENTAL, see test_mm.h/RESEARCH_FINDINGS.md #26/#27 -- address/port of the
    // dedicated server used for the manual Casual reservation-check test. NOT the future
    // matchmaking backend (that'll be matchmaking.backend_address/backend_port).
    std::string_view TestServerAddress() const { return m_testServerAddress; }
    uint16_t TestServerPort() const { return m_testServerPort; }

    // TEST ONLY, srcds side, see test_accept.h/RESEARCH_FINDINGS.md #42 -- "competitive", "wingman" or
    // "dangerzone" makes the server reserve a queued ('Q') roster of the real player plus fake
    // participants instead of the plain 'G' reservation. Empty = old Casual behavior.
    std::string_view TestAcceptMode() const { return m_testAcceptMode; }
    // AccountID (steamid64 & 0xffffffff) of the real player, srcds has no way to learn it by itself
    uint32_t TestRealAccountId() const { return m_testRealAccountId; }
    // how long after the Accept popup is up before the first fake participant accepts
    uint32_t TestFakeAcceptDelayMs() const { return m_testFakeAcceptDelayMs; }

    float GetRarityWeight(uint32_t rarity) const;

private:
    LogOutput m_logOutput{ LogOutputConsole };

    // actually default to 4465480 instead of 730, people are going to use old configs
    // and then wonder why the game doesn't work and open an issue on github otherwise
    uint32_t m_appIdOverride{ 4465480 };
    bool m_showCsgoGCServersOnly{ true };

    RankId m_competitiveRank{ RankNone };
    int m_competitiveWins{ 0 };
    RankId m_wingmanRank{ RankNone };
    int m_wingmanWins{ 0 };
    DangerZoneRankId m_dangerZoneRank{ DangerZoneRankNone };
    int m_dangerZoneWins{ 0 };

    bool m_destroyUsedItems{ true };

    bool m_vacBanned{ false };
    int m_commendedFriendly{ 0 };
    int m_commendedTeaching{ 0 };
    int m_commendedLeader{ 0 };
    int m_level{ 0 };
    int m_xp{ 0 };

    std::string m_testServerAddress{ "127.0.0.1" };
    uint16_t m_testServerPort{ 27015 };
    std::string m_testAcceptMode;
    uint32_t m_testRealAccountId{ 0 };
    uint32_t m_testFakeAcceptDelayMs{ 2500 };

    // default to valve weights
    std::vector<RarityWeight> m_rarityWeights{
        { ItemSchema::RarityCommon, 10000000 },
        { ItemSchema::RarityUncommon, 2000000 },
        { ItemSchema::RarityRare, 400000 },
        { ItemSchema::RarityMythical, 80000 },
        { ItemSchema::RarityLegendary, 16000 },
        { ItemSchema::RarityAncient, 3200 },
        { ItemSchema::RarityUnusual, 1280 },
    };
};

const GCConfig &GetConfig();
