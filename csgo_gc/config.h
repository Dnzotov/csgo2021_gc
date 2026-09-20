#pragma once

#include <vector>

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

    // The client does not know (and does not choose) which dedicated server a search gets: the Java backend does
    // (RESEARCH_FINDINGS.md #49). What is left below concerns the srcds process itself and comes from ITS command line.

    // TEST ONLY, srcds side, see test_accept.h/RESEARCH_FINDINGS.md #42 -- "competitive", "wingman" or
    // "dangerzone" (from -gc_mode <mode> on the srcds command line, one srcds per mode) makes the server reserve a
    // queued ('Q') roster of the real player plus fake participants instead of the plain 'G' reservation.
    // Empty = plain reservation (the classic modes).
    std::string_view TestAcceptMode() const { return m_testAcceptMode; }
    // TEST ONLY, srcds: its own game port (-port on the command line, srcds' default 27015 otherwise) and address
    // (-ip on the command line, else loopback): where its fake participants send their reservation checks to
    uint16_t DedicatedServerPort() const;
    std::string DedicatedServerAddress() const;
    // TEST ONLY, srcds: how long after the Accept popup is up before the first fake participant accepts
    uint32_t TestFakeAcceptDelayMs() const { return 2500; }
    // srcds without -gc_mode (RESEARCH_FINDINGS.md #60, reservation_keepalive.h): the plain reservation of the server is
    // refreshed while clients use the server, and for this many seconds after the last one connected / left (or after
    // the server started); then it is released. matchmaking.reservation_idle_seconds, 0 = no limit.
    uint32_t ReservationIdleSeconds() const { return m_reservationIdleSeconds; }
    // runtime diagnostics of the matchmaking UI flow (test_diag.h), client only, default on; matchmaking.test_diag
    // can still switch it off but it is not part of config.txt any more
    bool TestDiag() const { return m_testDiag; }

    // Java matchmaking backend (java-backend/, RESEARCH_FINDINGS.md #47-#49), client only: the ONLY matchmaking
    // settings of the client. It receives the search and picks the dedicated server (backend_client.h). Empty url = off,
    // and then no search can be served. Plain http:// only, e.g. "http://192.168.1.150:8080"; the key is sent as
    // X-Api-Key (backend.api-key).
    std::string_view BackendUrl() const { return m_backendUrl; }
    std::string_view BackendApiKey() const { return m_backendApiKey; }

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

    std::string m_testAcceptMode;
    bool m_testDiag{ true };
    uint32_t m_reservationIdleSeconds{ 30 * 60 }; // = ReservationKeepAlive::DefaultIdleSeconds

    std::string m_backendUrl;
    std::string m_backendApiKey;

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
