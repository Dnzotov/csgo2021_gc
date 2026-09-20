package dev.csgogc.mm.fake;

import java.time.Instant;
import java.util.List;

/**
 * A row of fake_search: a Fake Players PROFILE managed from the admin panel (TEST tool, RESEARCH_FINDINGS.md #54, #67).
 * It says how many virtual matchmaking participants a match of {@code category} gets: when the real players of a gathering match
 * are collected (gather window), the backend picks the enabled profile of the mode that fits the match (highest priority first)
 * and adds {@code min(players, capacity - real players)} virtual players. The profile is a setting, not a queue entry: it is not
 * used up by a match, every match of the mode applies it again. The virtual players only exist in the backend; a profile never
 * starts a match and never holds a game server.
 */
public record FakeSearch(
        long id,
        String category,
        /** how many virtual players a match gets (0 = the match starts with its real players only) */
        int players,
        /** maps the match may be played on with this profile, empty = any map */
        List<String> maps,
        boolean enabled,
        /** the profile with the highest priority wins when several fit (ties: the older one) */
        int priority,
        /** when set, only this game server (registry id) serves the matches of this profile; null = any server of the mode */
        Long serverId,
        Instant createdAt,
        Instant updatedAt) {
}
