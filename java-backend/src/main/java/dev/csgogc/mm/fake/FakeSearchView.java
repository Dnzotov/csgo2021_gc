package dev.csgogc.mm.fake;

import java.time.Instant;
import java.util.List;

/** What the admin panel sees of a fake search. */
public record FakeSearchView(
        long id,
        String mode,
        String modeLabel,
        int players,
        List<String> maps,
        boolean enabled,
        String status,
        /** the match it joined and the server that match got; null while it is only searching */
        Joined match,
        Instant createdAt,
        Instant updatedAt) {

    public record Joined(String matchId, String matchStatus, long serverId, String serverAddress, int serverPort, String map,
                         int players, int requiredPlayers) {
    }
}
