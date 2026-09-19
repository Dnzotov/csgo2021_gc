package dev.csgogc.mm.search;

import java.time.Instant;
import java.util.List;

/** What the API and the admin panel see (snake_case on the wire). */
public record SearchView(
        long id,
        long accountId,
        /** SteamID64 derived from the AccountID (individual, public universe) */
        String steamId64,
        /** the raw MatchmakingStart.game_type: eGame | (mapMask << 8) */
        long gameType,
        int eGame,
        /** category key: competitive, wingman, ... */
        String mode,
        String modeLabel,
        boolean acceptRequired,
        String gameMode,
        List<String> maps,
        /** Skirmish: the modes that were selected (each with the maps of its group), empty otherwise */
        List<SearchVariant> variants,
        String requestId,
        String status,
        /** "gc" for real searches, "admin" for searches added from the panel */
        String source,
        Instant startedAt,
        Instant lastSeenAt,
        Instant endedAt,
        long durationSeconds,
        /** progress of the match this search was placed in (forming or complete) */
        MatchProgress match,
        /** the server the player has to connect to; only when the match is complete (WAITING_ACCEPT / READY_TO_CONNECT) */
        Assignment assignment) {

    /** players = real + virtual (fake) participants gathered so far */
    public record MatchProgress(String matchId, String status, int players, int fakePlayers, int requiredPlayers, String map,
                                long serverId,
                                /** complete, but the game server has not confirmed its roster yet: no assignment for now */
                                boolean awaitingServer) {
    }

    /** MatchAssignment: everything the GC needs to run the existing 9107 / Accept / QueueConnect flow */
    public record Assignment(
            String matchId,
            long serverId,
            /** IPv4 address (a hostname of the registry is resolved by the backend) */
            String serverAddress,
            int serverPort,
            String map,
            boolean acceptRequired,
            int requiredPlayers,
            /** who is in the match: the real players and the virtual test players (fake = true) */
            List<RosterEntry> players,
            /** the same, counted: required_players = real_players + fake_players */
            int realPlayers,
            int fakePlayers) {
    }
}
