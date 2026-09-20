package dev.csgogc.mm.search;

import java.time.Instant;

/**
 * A row of matchmaking_match. server_host is the IPv4 address handed to the clients (resolved when it was assigned).
 * A match that is still gathering has no server yet: serverId 0, serverHost "", serverPort 0, map "" (the columns stay
 * NOT NULL for the databases that already exist).
 */
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
        Instant endedAt,
        /** ACCEPTING: when the players have to be done accepting, null before / for the classic modes */
        Instant acceptDeadlineAt,
        /** ACCEPTED: when the last real player accepted */
        Instant acceptedAt) {

    public static final long NO_SERVER = 0L;

    public boolean hasServer() {
        return serverId != NO_SERVER;
    }
}
