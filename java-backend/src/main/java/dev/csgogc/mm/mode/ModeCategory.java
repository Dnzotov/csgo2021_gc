package dev.csgogc.mm.mode;

import java.util.Arrays;
import java.util.Optional;

/**
 * The matchmaking categories. Values mirror csgo_gc/mm_modes.cpp (RESEARCH_FINDINGS.md #44/#45/#46.3):
 * {@code eGame} is {@code MatchmakingStart.game_type & 0xF}. Cooperative (eGame 9) is not served by the GC (mm_modes.cpp: supported = false).
 */
public enum ModeCategory {
    COMPETITIVE("competitive", "Competitive", 8, "classic", "competitive", 10),
    WINGMAN("wingman", "Wingman", 10, "classic", "scrimcomp2v2", 4),
    DANGERZONE("dangerzone", "Danger Zone", 13, "freeforall", "survival", 16),
    /** eGame 11: the 5v5 scrimmage, an Accept mode like Competitive (the GC serves it: csgo_gc/mm_modes.cpp) */
    SCRIMCOMP5V5("scrimcomp5v5", "Scrimmage 5v5", 11, "classic", "scrimcomp5v5", 10),
    CASUAL("casual", "Casual", 7, "classic", "casual", 0),
    DEATHMATCH("deathmatch", "Deathmatch", 6, "gungame", "deathmatch", 0),
    ARMSRACE("armsrace", "Arms Race", 4, "gungame", "gungameprogressive", 0),
    DEMOLITION("demolition", "Demolition", 5, "gungame", "gungametrbomb", 0),
    SKIRMISH("skirmish", "Skirmish", 12, "skirmish", "skirmish", 0);

    private final String key;
    private final String label;
    private final int eGame;
    private final String srcdsGameType;
    private final String srcdsGameMode;
    private final int requiredPlayers;

    ModeCategory(String key, String label, int eGame, String srcdsGameType, String srcdsGameMode, int requiredPlayers) {
        this.key = key;
        this.label = label;
        this.eGame = eGame;
        this.srcdsGameType = srcdsGameType;
        this.srcdsGameMode = srcdsGameMode;
        this.requiredPlayers = requiredPlayers;
    }

    /** name used on the wire and in the database ("competitive"), same as MM::GameMode::name in the GC */
    public String key() {
        return key;
    }

    public String label() {
        return label;
    }

    /** MatchmakingStart.game_type &amp; 0xF */
    public int eGame() {
        return eGame;
    }

    public String srcdsGameType() {
        return srcdsGameType;
    }

    public String srcdsGameMode() {
        return srcdsGameMode;
    }

    /** the retail client shows an Accept popup for these (client.dll sub_103EF770) */
    public boolean acceptRequired() {
        return requiredPlayers > 0;
    }

    /** humans a match needs (Accept modes only, 0 otherwise) */
    public int requiredPlayers() {
        return requiredPlayers;
    }

    public static Optional<ModeCategory> fromKey(String key) {
        if (key == null) {
            return Optional.empty();
        }
        String k = key.trim();
        return Arrays.stream(values()).filter(c -> c.key.equalsIgnoreCase(k)).findFirst();
    }

    /**
     * the category of servers that serves a skirmish ("War Games") mode, by its srcds game_mode (items_game.txt
     * skirmish_modes): Arms Race and Demolition have their own server categories, every other skirmish mode
     * (Retakes, Flying Scoutsman, ...) is served by the Skirmish category.
     */
    public static ModeCategory forSkirmishGameMode(String gameMode) {
        if (ARMSRACE.srcdsGameMode.equalsIgnoreCase(gameMode)) {
            return ARMSRACE;
        }
        if (DEMOLITION.srcdsGameMode.equalsIgnoreCase(gameMode)) {
            return DEMOLITION;
        }
        return SKIRMISH;
    }

    public static Optional<ModeCategory> fromEGame(int eGame) {
        return Arrays.stream(values()).filter(c -> c.eGame == eGame).findFirst();
    }
}
