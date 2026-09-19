package dev.csgogc.mm.fake;

import java.time.Instant;
import java.util.List;

/**
 * A row of fake_search: a group of {@code players} virtual matchmaking participants (TEST tool, RESEARCH_FINDINGS.md #54).
 * They only exist in the backend: they fill a match a real player has started, they never start one and never hold a
 * game server on their own.
 */
public record FakeSearch(
        long id,
        String category,
        int players,
        /** maps they accept, empty = any map */
        List<String> maps,
        boolean enabled,
        FakeStatus status,
        String matchId,
        Instant createdAt,
        Instant updatedAt,
        Instant matchedAt) {
}
