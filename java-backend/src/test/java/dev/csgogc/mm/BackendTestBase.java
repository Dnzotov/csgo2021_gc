package dev.csgogc.mm;

import static org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.csrf;
import static org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.user;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.delete;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.put;
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
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.AutoConfigureMockMvc;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.boot.test.context.TestConfiguration;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Import;
import org.springframework.context.annotation.Primary;
import org.springframework.http.MediaType;
import org.springframework.jdbc.core.simple.JdbcClient;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.test.web.servlet.ResultActions;
import org.springframework.test.web.servlet.request.RequestPostProcessor;

/**
 * Shared plumbing of the stage-3 test classes (fake players, map groups): a controllable clock, the admin session, the
 * GC API key and helpers for servers / searches / fake searches. Every subclass gets its own database file.
 */
@SpringBootTest(properties = {
        "backend.admin.username=tester",
        "backend.admin.password=test-password-123",
        "backend.api-key=test-api-key",
        "backend.stale-search-timeout=PT5M",
        "backend.reaper-interval=PT1H",
        "backend.matcher-interval=PT1H",
        // set explicitly: a developer's local config/application.properties is read by the tests too
        "backend.server-reservation-ttl=PT10M",
        "backend.assigned-search-timeout=PT2M",
        "backend.finished-search-retention=PT1H",
        "backend.required-players.competitive=10",
        "backend.required-players.wingman=4",
        "backend.required-players.dangerzone=16",
        "backend.roster-ack-timeout=PT25S",
        "backend.roster-poll-window=PT10S",
        "backend.accept-timeout=PT25S",
        "backend.server-release-cooldown=PT15S",
        // the tests fill a match with fake players at once; the gather window has its own test class (GatherWindowTest)
        "backend.fake-players.gather-window=PT0S"
})
@AutoConfigureMockMvc
@Import(BackendTestBase.ClockConfig.class)
abstract class BackendTestBase {

    static Path tempDir(String name) {
        try {
            return Files.createTempDirectory(name);
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

    static final String KEY = "X-Api-Key";
    static final RequestPostProcessor ADMIN = user("tester").roles("ADMIN");

    // game_type = eGame | mask << 8, the plain eGame is enough for the backend unless a test needs the mask
    static final long CASUAL = 7, DEATHMATCH = 6, COMPETITIVE = 8, WINGMAN = 10, DANGERZONE = 13, SKIRMISH = 12;

    @BeforeEach
    void cleanDatabase() {
        jdbc.sql("DELETE FROM backend_setting").update();
        jdbc.sql("DELETE FROM fake_search").update();
        jdbc.sql("DELETE FROM matchmaking_search").update();
        jdbc.sql("DELETE FROM matchmaking_match").update();
        jdbc.sql("DELETE FROM game_server").update();
    }

    // ------------------------------------------------------------------------------------------ helpers

    JsonNode body(ResultActions actions) throws Exception {
        return json.readTree(actions.andReturn().getResponse().getContentAsString());
    }

    long addServer(String host, int port, String category, String map) throws Exception {
        String request = "{\"host\":\"" + host + "\",\"port\":" + port + ",\"category\":\"" + category + "\",\"map\":\"" + map
                + "\",\"enabled\":true}";
        return body(mvc.perform(post("/admin/api/servers").with(ADMIN).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                .content(request)).andExpect(status().isCreated())).get("id").asLong();
    }

    JsonNode server(long id) throws Exception {
        for (JsonNode s : body(mvc.perform(get("/admin/api/servers").with(ADMIN)))) {
            if (s.get("id").asLong() == id) {
                return s;
            }
        }
        throw new AssertionError("server " + id + " not listed");
    }

    /** POST /search with plain maps, returns the "search" node */
    JsonNode search(long account, long gameType, String maps, String requestId) throws Exception {
        return searchRaw(account, gameType, maps, null, requestId);
    }

    /** variants: the JSON array text of skirmish variants or null */
    JsonNode searchRaw(long account, long gameType, String maps, String variants, String requestId) throws Exception {
        String request = "{\"account_id\":" + account + ",\"game_type\":" + gameType
                + (maps == null ? "" : ",\"maps\":[" + maps + "]")
                + (variants == null ? "" : ",\"variants\":" + variants)
                + ",\"request_id\":\"" + requestId + "\"}";
        return body(mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key")
                .contentType(MediaType.APPLICATION_JSON).content(request)).andExpect(status().isOk())).get("search");
    }

    JsonNode poll(String requestId) throws Exception {
        return body(mvc.perform(get("/api/v1/matchmaking/search/" + requestId).header(KEY, "test-api-key"))
                .andExpect(status().isOk())).get("search");
    }

    void cancel(long account, String requestId) throws Exception {
        mvc.perform(post("/api/v1/matchmaking/cancel").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"account_id\":" + account + ",\"request_id\":\"" + requestId + "\"}")).andExpect(status().isOk());
    }

    /** the GC of a player says: the game server reported everybody accepted (POST /matchmaking/accepted), returns the body */
    JsonNode accepted(long account, String requestId) throws Exception {
        return body(mvc.perform(post("/api/v1/matchmaking/accepted").header(KEY, "test-api-key")
                .contentType(MediaType.APPLICATION_JSON)
                .content("{\"account_id\":" + account + ",\"request_id\":\"" + requestId + "\"}")).andExpect(status().isOk()));
    }

    /** runs the timers like the reaper would */
    void tick() throws Exception {
        mvc.perform(get("/api/v1/matchmaking/searches").header(KEY, "test-api-key")).andExpect(status().isOk());
    }

    // ---- fake searches (admin API)

    long addFake(String mode, int players, String maps) throws Exception {
        return addFake(mode, players, maps, true);
    }

    long addFake(String mode, int players, String maps, boolean enabled) throws Exception {
        return addFake(mode, players, maps, enabled, 0, null);
    }

    /** a Fake Players profile: how many virtual players a match of the mode gets (players), on which maps, [server], [priority] */
    long addFake(String mode, int players, String maps, boolean enabled, int priority, Long serverId) throws Exception {
        String request = "{\"mode\":\"" + mode + "\",\"players\":" + players
                + (maps == null ? "" : ",\"maps\":[" + maps + "]") + ",\"enabled\":" + enabled + ",\"priority\":" + priority
                + (serverId == null ? "" : ",\"server_id\":" + serverId) + "}";
        return body(mvc.perform(post("/admin/api/fake-searches").with(ADMIN).with(csrf())
                .contentType(MediaType.APPLICATION_JSON).content(request)).andExpect(status().isCreated())).get("id").asLong();
    }

    /** the panel: the master switch Fake Players ON / OFF */
    void fakeMaster(boolean on) throws Exception {
        mvc.perform(put("/admin/api/fake-settings").with(ADMIN).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                .content("{\"master\":" + on + "}")).andExpect(status().isOk());
    }

    /** the panel: how long a gathering match waits for more real players before its virtual players are decided */
    void gatherWindow(int seconds) throws Exception {
        mvc.perform(put("/admin/api/fake-settings").with(ADMIN).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                .content("{\"gather_window_seconds\":" + seconds + "}")).andExpect(status().isOk());
    }

    /** GET /admin/api/fake-searches: enabled (tool), master, gather_window_seconds, fake_searches */
    JsonNode fakeOverview() throws Exception {
        return body(mvc.perform(get("/admin/api/fake-searches").with(ADMIN)).andExpect(status().isOk()));
    }

    /** time passes, the timers run */
    void pass(int seconds) throws Exception {
        clock.advance(Duration.ofSeconds(seconds));
        tick();
    }

    JsonNode fake(long id) throws Exception {
        for (JsonNode f : body(mvc.perform(get("/admin/api/fake-searches").with(ADMIN))).get("fake_searches")) {
            if (f.get("id").asLong() == id) {
                return f;
            }
        }
        throw new AssertionError("fake search " + id + " not listed");
    }

    void fakeEnabled(long id, boolean enabled) throws Exception {
        mvc.perform(post("/admin/api/fake-searches/" + id + "/enabled").with(ADMIN).with(csrf())
                .contentType(MediaType.APPLICATION_JSON).content("{\"enabled\":" + enabled + "}")).andExpect(status().isOk());
    }

    void deleteFake(long id) throws Exception {
        mvc.perform(delete("/admin/api/fake-searches/" + id).with(ADMIN).with(csrf())).andExpect(status().isNoContent());
    }

    ResultActions putFake(long id, String request) throws Exception {
        return mvc.perform(put("/admin/api/fake-searches/" + id).with(ADMIN).with(csrf())
                .contentType(MediaType.APPLICATION_JSON).content(request));
    }

    JsonNode matches() throws Exception {
        return body(mvc.perform(get("/admin/api/matches").with(ADMIN)).andExpect(status().isOk()));
    }
}
