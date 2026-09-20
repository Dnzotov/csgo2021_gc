package dev.csgogc.mm.fake;

import java.time.Instant;
import java.util.List;

/** What the admin panel sees of a Fake Players profile. */
public record FakeSearchView(
        long id,
        String mode,
        String modeLabel,
        /** the configured number of virtual players */
        int players,
        List<String> maps,
        boolean enabled,
        /** ON / OFF (the profile's own switch; the master switch is in the settings) */
        String status,
        int priority,
        Long serverId,
        /** host:port and map of the bound server, null when the profile is not bound to one */
        String serverLabel,
        /** the most virtual players the mode can take (capacity - 1: a match needs a real player) */
        int maxPlayers,
        /** the live matches that got their virtual players from this profile */
        List<Used> matches,
        Instant createdAt,
        Instant updatedAt) {

    public record Used(String matchId, String matchStatus, int realPlayers, int fakePlayers, int configured, int capacity,
                       String serverAddress, int serverPort, String map) {
    }
}
