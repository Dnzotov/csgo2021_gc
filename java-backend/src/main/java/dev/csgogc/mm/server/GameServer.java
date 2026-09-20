package dev.csgogc.mm.server;

import java.time.Instant;

/** A registered game server (srcds). The backend, not any client config, is the source of truth for it. */
public record GameServer(
        long id,
        String host,
        int port,
        /** ModeCategory key: competitive, wingman, ... */
        String category,
        String categoryLabel,
        boolean acceptRequired,
        /** players a match of this category needs (0 for the classic modes) */
        int requiredPlayers,
        /** map the server runs, empty = not specified (such a server is only used for searches without maps) */
        String map,
        boolean enabled,
        /** AVAILABLE / RESERVED / BUSY */
        String state,
        /** the match holding the server while RESERVED */
        String reservedMatchId,
        Instant reservedAt,
        Instant lastAssignedAt,
        /** last time the server reported itself (POST /api/v1/servers/state), null = never */
        Instant lastHeartbeatAt,
        /** srcds maxplayers, informational */
        Integer maxPlayers,
        Instant createdAt,
        Instant updatedAt,
        /**
         * a server released from a cancelled Accept match is not handed out before this moment: its game server has to
         * drop the old reservation first (RESEARCH_FINDINGS.md #63); null = free at once
         */
        Instant availableAfter) {
}
