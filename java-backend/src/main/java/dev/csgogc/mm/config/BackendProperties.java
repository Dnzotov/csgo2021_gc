package dev.csgogc.mm.config;

import java.time.Duration;
import java.util.Map;
import org.springframework.boot.context.properties.ConfigurationProperties;
import org.springframework.boot.context.properties.bind.DefaultValue;

/**
 * All backend settings ({@code backend.*}). Environment variables work through Spring's relaxed binding, e.g.
 * {@code BACKEND_ADMIN_PASSWORD}, {@code BACKEND_API_KEY}, {@code BACKEND_DATABASE_PATH}.
 */
@ConfigurationProperties(prefix = "backend")
public record BackendProperties(
        @DefaultValue("./data/matchmaking.db") String databasePath,
        @DefaultValue Admin admin,
        /** shared secret the GC sends in the X-Api-Key header; empty = the GC-facing endpoints are open */
        @DefaultValue("") String apiKey,
        /** a search that is not polled for this long is expired (GC or game crashed) */
        @DefaultValue("PT15M") Duration staleSearchTimeout,
        @DefaultValue("PT10S") Duration reaperInterval,
        /** how long ended searches (cancelled/expired/removed/completed) stay visible in the panel */
        @DefaultValue("PT1H") Duration finishedSearchRetention,
        @DefaultValue Login login,
        /** a server handed to a complete match stays RESERVED this long, then it is AVAILABLE again (no srcds heartbeat yet) */
        @DefaultValue("PT10M") Duration serverReservationTtl,
        /** an assigned search (WAITING_ACCEPT / READY_TO_CONNECT) is completed after this long */
        @DefaultValue("PT2M") Duration assignedSearchTimeout,
        /** how often the matcher looks for a server / for more players */
        @DefaultValue("PT1S") Duration matcherInterval,
        /**
         * real players a match of an Accept category needs, by category key; defaults are the retail numbers
         * (competitive 10, wingman 4, dangerzone 16). A test setup where the srcds side supplies the missing
         * participants (its test roster) sets e.g. backend.required-players.competitive=1.
         */
        @DefaultValue Map<String, Integer> requiredPlayers,
        /**
         * An Accept match on a game server that reads its roster from this backend (it polls GET /api/v1/servers/roster)
         * is handed to the players only after that server confirmed it armed the roster (POST /servers/roster/ready), so
         * the retail reservation check already succeeds on the first try. If the confirmation does not come within this
         * time the players get the server anyway.
         */
        @DefaultValue("PT25S") Duration rosterAckTimeout,
        /** a game server that asked for its roster this recently counts as "reads its roster from the backend" */
        @DefaultValue("PT10S") Duration rosterPollWindow,
        /** TEST tool: virtual players added from the admin panel (RESEARCH_FINDINGS.md #54) */
        @DefaultValue FakePlayers fakePlayers) {

    /**
     * {@code backend.fake-players.enabled=false} switches the whole tool off (production): the panel section is inert,
     * the API refuses new fake searches and the matcher never looks at them.
     */
    public record FakePlayers(@DefaultValue("true") boolean enabled) {
    }

    public record Admin(@DefaultValue("") String username, @DefaultValue("") String password) {
    }

    public record Login(
            @DefaultValue("5") int maxFailures,
            @DefaultValue("PT5M") Duration failureWindow,
            @DefaultValue("PT5M") Duration lockDuration) {
    }
}
