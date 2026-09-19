package dev.csgogc.mm;

import static org.assertj.core.api.Assertions.assertThat;
import static org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.csrf;
import static org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.user;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.delete;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneId;
import java.time.ZoneOffset;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.AutoConfigureMockMvc;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.boot.test.context.TestConfiguration;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Primary;
import org.springframework.http.MediaType;
import org.springframework.jdbc.core.simple.JdbcClient;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.test.web.servlet.request.RequestPostProcessor;

/** Stage 2: server selection, reservation, gathering players for the Accept modes, server state. */
@SpringBootTest(properties = {
        "backend.admin.username=tester",
        "backend.admin.password=test-password-123",
        "backend.api-key=test-api-key",
        "backend.stale-search-timeout=PT5M",
        "backend.reaper-interval=PT1H",
        "backend.matcher-interval=PT1H",
        // all of these are set explicitly: a developer's local config/application.properties is read by the tests too
        "backend.server-reservation-ttl=PT10M",
        "backend.assigned-search-timeout=PT2M",
        "backend.finished-search-retention=PT1H",
        // test setup: two real players fill a Competitive match (retail: 10), the others use the retail numbers
        "backend.required-players.competitive=2",
        "backend.required-players.wingman=4",
        "backend.required-players.dangerzone=16"
})
@AutoConfigureMockMvc
class MatchmakingIntegrationTest {

    private static final Path DB_DIR = createTempDir();

    @DynamicPropertySource
    static void database(DynamicPropertyRegistry registry) {
        registry.add("backend.database-path", () -> DB_DIR.resolve("mm.db").toString());
    }

    private static Path createTempDir() {
        try {
            return Files.createTempDirectory("mm-backend-match-test");
        } catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }

    static class MutableClock extends Clock {
        private volatile Instant now = Instant.parse("2026-01-01T12:00:00Z");

        void advance(Duration d) {
            now = now.plus(d);
        }

        @Override
        public ZoneId getZone() {
            return ZoneOffset.UTC;
        }

        @Override
        public Clock withZone(ZoneId zone) {
            return this;
        }

        @Override
        public Instant instant() {
            return now;
        }
    }

    @TestConfiguration
    static class ClockConfig {
        @Bean
        @Primary
        MutableClock testClock() {
            return new MutableClock();
        }
    }

    @Autowired MockMvc mvc;
    @Autowired JdbcClient jdbc;
    @Autowired MutableClock clock;
    @Autowired ObjectMapper json;

    private static final String KEY = "X-Api-Key";
    private static final RequestPostProcessor ADMIN = user("tester").roles("ADMIN");

    // game_type values: eGame | mask << 8, plain eGame is enough for the backend
    private static final long CASUAL = 7, DEATHMATCH = 6, COMPETITIVE = 8, WINGMAN = 10, DANGERZONE = 13;

    @BeforeEach
    void clean() {
        jdbc.sql("DELETE FROM matchmaking_search").update();
        jdbc.sql("DELETE FROM matchmaking_match").update();
        jdbc.sql("DELETE FROM game_server").update();
    }

    // ------------------------------------------------------------------------------------------ helpers

    private JsonNode body(org.springframework.test.web.servlet.ResultActions actions) throws Exception {
        return json.readTree(actions.andReturn().getResponse().getContentAsString());
    }

    private long addServer(String host, int port, String category, String map) throws Exception {
        return addServer(host, port, category, map, true, null);
    }

    private long addServer(String host, int port, String category, String map, boolean enabled, String state) throws Exception {
        String request = "{\"host\":\"" + host + "\",\"port\":" + port + ",\"category\":\"" + category + "\",\"map\":\"" + map
                + "\",\"enabled\":" + enabled + (state == null ? "" : ",\"state\":\"" + state + "\"") + "}";
        return body(mvc.perform(post("/admin/api/servers").with(ADMIN).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                .content(request)).andExpect(status().isCreated())).get("id").asLong();
    }

    private JsonNode server(long id) throws Exception {
        for (JsonNode s : body(mvc.perform(get("/admin/api/servers").with(ADMIN)))) {
            if (s.get("id").asLong() == id) {
                return s;
            }
        }
        throw new AssertionError("server " + id + " not listed");
    }

    private void serverAction(long id, String action, String content) throws Exception {
        mvc.perform(post("/admin/api/servers/" + id + "/" + action).with(ADMIN).with(csrf())
                .contentType(MediaType.APPLICATION_JSON).content(content)).andExpect(status().isOk());
    }

    /** POST /search, returns the "search" node */
    private JsonNode search(long account, long gameType, String maps, String requestId) throws Exception {
        String request = "{\"account_id\":" + account + ",\"game_type\":" + gameType
                + (maps == null ? "" : ",\"maps\":[" + maps + "]") + ",\"request_id\":\"" + requestId + "\"}";
        return body(mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key")
                .contentType(MediaType.APPLICATION_JSON).content(request)).andExpect(status().isOk())).get("search");
    }

    /** GET /search/{request_id}, the polling call of the GC */
    private JsonNode poll(String requestId) throws Exception {
        return body(mvc.perform(get("/api/v1/matchmaking/search/" + requestId).header(KEY, "test-api-key"))
                .andExpect(status().isOk())).get("search");
    }

    private void cancel(long account, String requestId) throws Exception {
        mvc.perform(post("/api/v1/matchmaking/cancel").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"account_id\":" + account + ",\"request_id\":\"" + requestId + "\"}")).andExpect(status().isOk());
    }

    // ------------------------------------------------------------------------------------------ classic modes

    @Test
    void classicSearchGetsAnAvailableServerAndDoesNotReserveIt() throws Exception {
        long serverId = addServer("192.168.1.150", 27016, "casual", "de_dust2");

        JsonNode search = search(1050166997L, CASUAL, "\"de_dust2\",\"de_mirage\"", "r-1");
        assertThat(search.get("status").asText()).isEqualTo("READY_TO_CONNECT");
        JsonNode assignment = search.get("assignment");
        assertThat(assignment.get("server_address").asText()).isEqualTo("192.168.1.150");
        assertThat(assignment.get("server_port").asInt()).isEqualTo(27016);
        assertThat(assignment.get("map").asText()).isEqualTo("de_dust2");
        assertThat(assignment.get("accept_required").asBoolean()).isFalse();
        assertThat(assignment.get("required_players").asInt()).isEqualTo(1);
        assertThat(assignment.get("server_id").asLong()).isEqualTo(serverId);

        // a classic mode only points the player at the server: no reservation (RESERVED belongs to the Accept flow)
        JsonNode pointed = server(serverId);
        assertThat(pointed.get("state").asText()).isEqualTo("AVAILABLE");
        assertThat(pointed.hasNonNull("reserved_match_id")).isFalse();
        assertThat(pointed.hasNonNull("last_assigned_at")).isTrue();

        // the GC polls with the request id and gets the same assignment
        JsonNode polled = poll("r-1");
        assertThat(polled.get("status").asText()).isEqualTo("READY_TO_CONNECT");
        assertThat(polled.get("assignment").get("server_port").asInt()).isEqualTo(27016);
    }

    @Test
    void searchWaitsWithoutAServerAndGetsOneWhenItAppears() throws Exception {
        JsonNode waiting = search(1, CASUAL, "\"de_dust2\"", "r-wait");
        assertThat(waiting.get("status").asText()).isEqualTo("SEARCHING");
        assertThat(waiting.has("assignment")).isFalse();
        assertThat(poll("r-wait").get("status").asText()).isEqualTo("SEARCHING");

        addServer("10.0.0.5", 27015, "casual", "de_dust2");           // adding a server triggers the matcher
        JsonNode matched = poll("r-wait");
        assertThat(matched.get("status").asText()).isEqualTo("READY_TO_CONNECT");
        assertThat(matched.get("assignment").get("server_address").asText()).isEqualTo("10.0.0.5");
    }

    @Test
    void modeMustMatch() throws Exception {
        addServer("10.0.0.5", 27015, "wingman", "de_dust2");
        addServer("10.0.0.5", 27016, "deathmatch", "de_dust2");
        assertThat(search(1, CASUAL, "\"de_dust2\"", "r-1").get("status").asText()).isEqualTo("SEARCHING");
        // and the right category finds its own server
        assertThat(search(2, DEATHMATCH, "\"de_dust2\"", "r-2").get("assignment").get("server_port").asInt()).isEqualTo(27016);
    }

    @Test
    void serverMapMustBeOneOfTheSelectedMaps() throws Exception {
        addServer("10.0.0.5", 27015, "casual", "de_inferno");
        assertThat(search(1, CASUAL, "\"de_dust2\",\"de_mirage\"", "r-1").get("status").asText()).isEqualTo("SEARCHING");
        assertThat(search(2, CASUAL, "\"de_mirage\",\"de_inferno\"", "r-2").get("assignment").get("map").asText())
                .isEqualTo("de_inferno");
    }

    @Test
    void aSearchWithoutMapsFitsAnyServerButAServerWithoutMapFitsOnlyThat() throws Exception {
        long unknownMap = addServer("10.0.0.5", 27015, "casual", "");
        assertThat(search(1, CASUAL, "\"de_dust2\"", "r-1").get("status").asText()).isEqualTo("SEARCHING");
        assertThat(search(2, CASUAL, null, "r-2").get("assignment").get("server_id").asLong()).isEqualTo(unknownMap);
    }

    @Test
    void disabledAndBusyServersAreNotUsed() throws Exception {
        long disabled = addServer("10.0.0.5", 27015, "casual", "de_dust2", false, null);
        long busy = addServer("10.0.0.5", 27016, "casual", "de_dust2", true, "BUSY");
        assertThat(search(1, CASUAL, "\"de_dust2\"", "r-1").get("status").asText()).isEqualTo("SEARCHING");

        serverAction(disabled, "enabled", "{\"enabled\":true}");           // enabling hands the waiting search this server
        assertThat(poll("r-1").get("assignment").get("server_port").asInt()).isEqualTo(27015);

        // the enabled server is not exclusive: the next player is sent to it as well, the BUSY one is still skipped
        assertThat(search(2, CASUAL, "\"de_dust2\"", "r-2").get("assignment").get("server_port").asInt()).isEqualTo(27015);
        serverAction(busy, "state", "{\"state\":\"AVAILABLE\"}");
        // the freed server was never assigned, so it is the least recently used one now
        assertThat(search(3, CASUAL, "\"de_dust2\"", "r-3").get("assignment").get("server_port").asInt()).isEqualTo(27016);
    }

    @Test
    void aClassicServerIsNotExclusiveSeveralPlayersAreSentToIt() throws Exception {
        long only = addServer("10.0.0.5", 27015, "casual", "de_dust2");
        JsonNode first = search(1, CASUAL, "\"de_dust2\"", "r-1");
        JsonNode second = search(2, CASUAL, "\"de_dust2\"", "r-2");
        assertThat(first.get("status").asText()).isEqualTo("READY_TO_CONNECT");
        assertThat(second.get("status").asText()).isEqualTo("READY_TO_CONNECT");
        assertThat(second.get("assignment").get("server_id").asLong()).isEqualTo(only);
        assertThat(server(only).get("state").asText()).isEqualTo("AVAILABLE");

        // BUSY (admin / the server itself) is the way to stop sending players to it
        serverAction(only, "state", "{\"state\":\"BUSY\"}");
        assertThat(search(3, CASUAL, "\"de_dust2\"", "r-3").get("status").asText()).isEqualTo("SEARCHING");
    }

    @Test
    void twoServersServeTwoSearchesAndTheLeastRecentlyUsedGoesFirst() throws Exception {
        addServer("10.0.0.5", 27015, "casual", "de_dust2");
        addServer("10.0.0.5", 27016, "casual", "de_dust2");
        int p1 = search(1, CASUAL, "\"de_dust2\"", "r-1").get("assignment").get("server_port").asInt();
        int p2 = search(2, CASUAL, "\"de_dust2\"", "r-2").get("assignment").get("server_port").asInt();
        assertThat(p1).isNotEqualTo(p2);
        // both were used once: the third goes back to the one that was used first
        JsonNode third = search(3, CASUAL, "\"de_dust2\"", "r-3");
        assertThat(third.get("status").asText()).isEqualTo("READY_TO_CONNECT");
        assertThat(third.get("assignment").get("server_port").asInt()).isEqualTo(p1);
    }

    @Test
    void cancellingAClassicSearchNeverChangesTheServerState() throws Exception {
        long serverId = addServer("10.0.0.5", 27015, "casual", "de_dust2");
        search(1, CASUAL, "\"de_dust2\"", "r-1");                          // pointed at the server
        search(2, CASUAL, "\"de_dust2\"", "r-2");                          // the same server, no reservation
        cancel(2, "r-2");
        assertThat(poll("r-2").get("status").asText()).isEqualTo("CANCELLED");
        assertThat(server(serverId).get("state").asText()).isEqualTo("AVAILABLE");
        // an assigned player cancelling (the client also sends a stop when it connects) does not touch the server either
        cancel(1, "r-1");
        assertThat(server(serverId).get("state").asText()).isEqualTo("AVAILABLE");
    }

    @Test
    void aClassicPlayerWhoSearchesAgainSimplyGetsAServerAgain() throws Exception {
        long serverId = addServer("10.0.0.5", 27015, "casual", "de_dust2");
        JsonNode first = search(1, CASUAL, "\"de_dust2\"", "r-1");
        assertThat(first.get("status").asText()).isEqualTo("READY_TO_CONNECT");

        cancel(1, "r-1");                                                   // the attempt failed, the player tries again
        JsonNode second = search(1, CASUAL, "\"de_dust2\"", "r-2");
        assertThat(second.get("status").asText()).isEqualTo("READY_TO_CONNECT");
        assertThat(second.get("assignment").get("server_id").asLong()).isEqualTo(serverId);
        assertThat(second.get("assignment").get("match_id").asText()).isNotEqualTo(first.get("assignment").get("match_id").asText());

        // starting over without a cancel does the same, and somebody else's search is not held back by it
        assertThat(search(1, CASUAL, "\"de_dust2\"", "r-3").get("status").asText()).isEqualTo("READY_TO_CONNECT");
        assertThat(search(2, CASUAL, "\"de_dust2\"", "s-1").get("status").asText()).isEqualTo("READY_TO_CONNECT");
    }

    @Test
    void leavingASharedMatchDoesNotReleaseTheServer() throws Exception {
        long serverId = addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        search(1, COMPETITIVE, "\"de_dust2\"", "a");
        assertThat(search(2, COMPETITIVE, "\"de_dust2\"", "b").get("status").asText()).isEqualTo("WAITING_ACCEPT");

        cancel(1, "a");
        assertThat(search(1, COMPETITIVE, "\"de_dust2\"", "a2").get("status").asText()).isEqualTo("SEARCHING");
        assertThat(server(serverId).get("state").asText()).isEqualTo("RESERVED");
        assertThat(poll("b").get("status").asText()).isEqualTo("WAITING_ACCEPT");
    }

    @Test
    void anAssignedClassicSearchIsCompletedAfterTheTimeoutAndTheServerWasNeverReserved() throws Exception {
        long serverId = addServer("10.0.0.5", 27015, "casual", "de_dust2");
        search(1, CASUAL, "\"de_dust2\"", "r-1");
        clock.advance(Duration.ofMinutes(3));
        mvc.perform(get("/api/v1/matchmaking/searches?include_finished=true").header(KEY, "test-api-key"));  // runs the timers
        assertThat(poll("r-1").get("status").asText()).isEqualTo("COMPLETED");      // assigned-search-timeout PT2M
        assertThat(server(serverId).get("state").asText()).isEqualTo("AVAILABLE");

        clock.advance(Duration.ofMinutes(8));                                        // reservation TTL: only the match record ends
        mvc.perform(get("/api/v1/matchmaking/searches").header(KEY, "test-api-key"));
        assertThat(server(serverId).get("state").asText()).isEqualTo("AVAILABLE");
        assertThat(body(mvc.perform(get("/admin/api/matches").with(ADMIN))).get(0).get("status").asText()).isEqualTo("ENDED");
        assertThat(search(2, CASUAL, "\"de_dust2\"", "r-2").get("status").asText()).isEqualTo("READY_TO_CONNECT");
    }

    @Test
    void aHostnameOfTheRegistryIsResolvedToAnIpv4ForTheClient() throws Exception {
        addServer("localhost", 27015, "casual", "de_dust2");
        assertThat(search(1, CASUAL, "\"de_dust2\"", "r-1").get("assignment").get("server_address").asText()).isEqualTo("127.0.0.1");
    }

    // ------------------------------------------------------------------------------------------ Accept modes

    @Test
    void competitiveGathersPlayersOnOneReservedServerBeforeAnybodyGetsIt() throws Exception {
        long serverId = addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        addServer("10.0.0.5", 27018, "competitive", "de_mirage");

        JsonNode a = search(1, COMPETITIVE, "\"de_dust2\"", "a");
        assertThat(a.get("status").asText()).isEqualTo("MATCHED");                  // server reserved, 1/2 players
        assertThat(a.has("assignment")).isFalse();
        assertThat(a.get("match").get("players").asInt()).isEqualTo(1);
        assertThat(a.get("match").get("required_players").asInt()).isEqualTo(2);
        assertThat(server(serverId).get("state").asText()).isEqualTo("RESERVED");

        // a player who wants another map does not join, the second server is reserved for a second match
        JsonNode other = search(3, COMPETITIVE, "\"de_mirage\"", "c");
        assertThat(other.get("match").get("match_id").asText()).isNotEqualTo(a.get("match").get("match_id").asText());

        // a compatible player completes the first match: both get the same server, the Accept phase starts
        JsonNode b = search(2, COMPETITIVE, "\"de_dust2\",\"de_inferno\"", "b");
        assertThat(b.get("status").asText()).isEqualTo("WAITING_ACCEPT");
        assertThat(b.get("assignment").get("accept_required").asBoolean()).isTrue();
        JsonNode aAgain = poll("a");
        assertThat(aAgain.get("status").asText()).isEqualTo("WAITING_ACCEPT");
        assertThat(aAgain.get("assignment").get("match_id").asText()).isEqualTo(b.get("assignment").get("match_id").asText());
        assertThat(aAgain.get("assignment").get("server_port").asInt()).isEqualTo(27017);
        assertThat(poll("c").get("status").asText()).isEqualTo("MATCHED");            // still waiting for its second player

        // a further player cannot join the finished match and finds no free dust2 server
        assertThat(search(4, COMPETITIVE, "\"de_dust2\"", "d").get("status").asText()).isEqualTo("SEARCHING");
    }

    @Test
    void aFormingMatchFallsApartWhenItsOnlyPlayerCancelsAndTheServerIsFreed() throws Exception {
        long serverId = addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        search(1, COMPETITIVE, "\"de_dust2\"", "a");
        assertThat(server(serverId).get("state").asText()).isEqualTo("RESERVED");
        cancel(1, "a");
        assertThat(server(serverId).get("state").asText()).isEqualTo("AVAILABLE");
        assertThat(server(serverId).has("reserved_match_id")).isFalse();
        JsonNode matches = body(mvc.perform(get("/admin/api/matches").with(ADMIN)));
        assertThat(matches.get(0).get("status").asText()).isEqualTo("CANCELLED");
    }

    @Test
    void aPlayerWhoStopsPollingLeavesTheFormingMatchAndTheServerIsFreed() throws Exception {
        long serverId = addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        search(1, COMPETITIVE, "\"de_dust2\"", "a");
        assertThat(server(serverId).get("state").asText()).isEqualTo("RESERVED");

        clock.advance(Duration.ofMinutes(6));                                        // stale-search-timeout is PT5M
        mvc.perform(get("/api/v1/matchmaking/searches").header(KEY, "test-api-key"));  // runs the timers
        assertThat(poll("a").get("status").asText()).isEqualTo("EXPIRED");
        assertThat(server(serverId).get("state").asText()).isEqualTo("AVAILABLE");

        // a new player starts a fresh match, the expired one is not in it
        JsonNode b = search(2, COMPETITIVE, "\"de_dust2\"", "b");
        assertThat(b.get("status").asText()).isEqualTo("MATCHED");
        assertThat(b.get("match").get("players").asInt()).isEqualTo(1);
    }

    @Test
    void wingmanNeedsFourAndDangerZoneSixteenByDefault() throws Exception {
        addServer("10.0.0.5", 27018, "wingman", "de_lake");
        addServer("10.0.0.5", 27019, "dangerzone", "dz_blacksite");
        for (int i = 1; i <= 3; i++) {
            assertThat(search(i, WINGMAN, "\"de_lake\"", "w" + i).get("status").asText()).isEqualTo("MATCHED");
        }
        assertThat(search(4, WINGMAN, "\"de_lake\"", "w4").get("status").asText()).isEqualTo("WAITING_ACCEPT");
        assertThat(search(20, DANGERZONE, "\"dz_blacksite\",\"dz_sirocco\"", "z1").get("match").get("required_players").asInt())
                .isEqualTo(16);
    }

    // ------------------------------------------------------------------------------------------ polling / server API

    @Test
    void pollingKeepsASearchAliveAndUnknownRequestIdsAre404() throws Exception {
        search(1, CASUAL, "\"de_dust2\"", "r-1");
        search(2, CASUAL, "\"de_dust2\"", "r-2");
        clock.advance(Duration.ofMinutes(4));
        poll("r-1");                                     // r-1 is polled, r-2 is not
        clock.advance(Duration.ofMinutes(4));
        mvc.perform(get("/api/v1/matchmaking/searches").header(KEY, "test-api-key"));   // runs the timers
        assertThat(poll("r-1").get("status").asText()).isEqualTo("SEARCHING");
        assertThat(poll("r-2").get("status").asText()).isEqualTo("EXPIRED");
        mvc.perform(get("/api/v1/matchmaking/search/nope").header(KEY, "test-api-key")).andExpect(status().isNotFound());
        mvc.perform(get("/api/v1/matchmaking/search/r-1")).andExpect(status().isUnauthorized());
    }

    @Test
    void aGameServerCanReportItsStateWithTheApiKey() throws Exception {
        long id = addServer("10.0.0.5", 27015, "casual", "de_dust2");

        mvc.perform(post("/api/v1/servers/state").contentType(MediaType.APPLICATION_JSON)
                .content("{\"address\":\"10.0.0.5\",\"port\":27015}")).andExpect(status().isUnauthorized());
        mvc.perform(post("/api/v1/servers/state").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                        .content("{\"address\":\"10.0.0.5\",\"port\":27015,\"map\":\"de_mirage\",\"state\":\"BUSY\"}"))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.map").value("de_mirage"))
                .andExpect(jsonPath("$.state").value("BUSY"))
                .andExpect(jsonPath("$.last_heartbeat_at").exists());
        mvc.perform(post("/api/v1/servers/state").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"address\":\"10.9.9.9\",\"port\":1}")).andExpect(status().isNotFound());
        mvc.perform(post("/api/v1/servers/state").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"address\":\"10.0.0.5\",\"port\":27015,\"state\":\"RESERVED\"}")).andExpect(status().isBadRequest());

        // the map it now runs decides which searches it gets; AVAILABLE frees a BUSY server
        mvc.perform(post("/api/v1/servers/state").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"address\":\"10.0.0.5\",\"port\":27015,\"state\":\"AVAILABLE\"}")).andExpect(status().isOk());
        assertThat(search(1, CASUAL, "\"de_dust2\"", "r-1").get("status").asText()).isEqualTo("SEARCHING");
        assertThat(search(2, CASUAL, "\"de_mirage\"", "r-2").get("assignment").get("server_id").asLong()).isEqualTo(id);

        // a classic server stays AVAILABLE after an assignment; a reserved (Accept) server is not released by its own
        // "available" report (the assignment is fresh)
        assertThat(server(id).get("state").asText()).isEqualTo("AVAILABLE");
        long accept = addServer("10.0.0.6", 27017, "competitive", "de_dust2");
        search(9, COMPETITIVE, "\"de_dust2\"", "c-1");
        assertThat(server(accept).get("state").asText()).isEqualTo("RESERVED");
        mvc.perform(post("/api/v1/servers/state").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"address\":\"10.0.0.6\",\"port\":27017,\"state\":\"AVAILABLE\"}")).andExpect(status().isOk());
        assertThat(server(accept).get("state").asText()).isEqualTo("RESERVED");
    }

    @Test
    void deletingAReservedServerReturnsItsFormingPlayersToTheQueue() throws Exception {
        long id = addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        search(1, COMPETITIVE, "\"de_dust2\"", "a");
        mvc.perform(delete("/admin/api/servers/" + id).with(ADMIN).with(csrf())).andExpect(status().isNoContent());
        JsonNode a = poll("a");
        assertThat(a.get("status").asText()).isEqualTo("SEARCHING");
        assertThat(a.has("match")).isFalse();
    }

    @Test
    void editingAServerNeverChangesItsState() throws Exception {
        long id = addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        search(1, COMPETITIVE, "\"de_dust2\"", "r-1");                    // Accept mode: the server is RESERVED for the match
        mvc.perform(org.springframework.test.web.servlet.request.MockMvcRequestBuilders.put("/admin/api/servers/" + id)
                        .with(ADMIN).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                        .content("{\"host\":\"10.0.0.5\",\"port\":27017,\"category\":\"competitive\",\"map\":\"de_mirage\",\"enabled\":true,\"max_players\":20}"))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.state").value("RESERVED"))
                .andExpect(jsonPath("$.max_players").value(20));
    }
}
