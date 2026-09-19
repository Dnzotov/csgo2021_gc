package dev.csgogc.mm.search;

import java.time.Instant;

/** A row of matchmaking_match. server_host is the IPv4 address handed to the clients (resolved when it was assigned). */
public record MatchRecord(
        String id,
        String category,
        long serverId,
        String serverHost,
        int serverPort,
        String map,
        int requiredPlayers,
        boolean acceptRequired,
        MatchStatus status,
        Instant createdAt,
        Instant readyAt,
        Instant endedAt) {
}
