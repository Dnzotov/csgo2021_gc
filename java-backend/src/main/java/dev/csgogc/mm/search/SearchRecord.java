package dev.csgogc.mm.search;

import java.time.Instant;
import java.util.List;

/** A row of matchmaking_search. */
public record SearchRecord(
        long id,
        long accountId,
        long gameType,
        String category,
        String gameMode,
        List<String> maps,
        /** Skirmish only: the modes the player selected, empty for every other search */
        List<SearchVariant> variants,
        String requestId,
        SearchStatus status,
        String source,
        Instant startedAt,
        Instant lastSeenAt,
        Instant endedAt,
        String matchId,
        Instant matchedAt,
        /** Accept modes: when this player accepted (the GC reports it once the game server says everybody accepted) */
        Instant acceptedAt,
        /** a member of a party: the id of the search row of the party leader, null for the leader and for solo searches */
        Long partyLeaderId,
        /** when this player's GC first fetched the assignment (server data = its Match Found), null = it has not (yet) */
        Instant assignmentSeenAt) {

    public boolean isPartyMember() {
        return partyLeaderId != null;
    }
}
