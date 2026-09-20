package dev.csgogc.mm;

import static org.hamcrest.Matchers.containsString;
import static org.hamcrest.Matchers.hasSize;
import static org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.csrf;
import static org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.user;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.delete;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.put;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.header;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.redirectedUrl;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

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
import org.springframework.mock.web.MockHttpSession;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.test.web.servlet.MvcResult;

@SpringBootTest(properties = {
        "backend.admin.username=tester",
        "backend.admin.password=test-password-123",
        "backend.api-key=test-api-key",
        "backend.stale-search-timeout=PT5M",
        "backend.reaper-interval=PT1H"
})
@AutoConfigureMockMvc
class BackendIntegrationTest {

    private static final Path DB_DIR = createTempDir();

    @DynamicPropertySource
    static void database(DynamicPropertyRegistry registry) {
        registry.add("backend.database-path", () -> DB_DIR.resolve("test.db").toString());
    }

    private static Path createTempDir() {
        try {
            return Files.createTempDirectory("mm-backend-test");
        } catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }

    /** a clock the tests can move, so the timeout is tested without sleeping */
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

    @BeforeEach
    void clean() {
        jdbc.sql("DELETE FROM matchmaking_search").update();
        jdbc.sql("DELETE FROM matchmaking_match").update();
        jdbc.sql("DELETE FROM game_server").update();
    }

    private static final String KEY = "X-Api-Key";

    private static String search(long account, long gameType, String extra) {
        return "{\"account_id\":" + account + ",\"game_type\":" + gameType + (extra.isEmpty() ? "" : "," + extra) + "}";
    }

    // ------------------------------------------------------------------ public / auth

    @Test
    void healthIsPublic() throws Exception {
        mvc.perform(get("/api/v1/health"))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.status").value("ok"));
    }

    @Test
    void adminPagesAndApisRequireLogin() throws Exception {
        mvc.perform(get("/admin")).andExpect(status().isFound()).andExpect(redirectedUrl("http://localhost/admin/login"));
        mvc.perform(get("/admin/api/servers")).andExpect(status().isUnauthorized());
        mvc.perform(get("/admin/api/categories")).andExpect(status().isUnauthorized());
        mvc.perform(get("/api/v1/matchmaking/searches")).andExpect(status().isUnauthorized());
        mvc.perform(get("/admin/login")).andExpect(status().isOk()).andExpect(
                header().string("Content-Security-Policy", containsString("script-src 'self'")));
        // the panel's static assets are not secret, the panel data is
        mvc.perform(get("/admin/assets/admin.js")).andExpect(status().isOk());
    }

    @Test
    void loginWithCredentialsGivesAccessAndWrongPasswordDoesNot() throws Exception {
        mvc.perform(post("/admin/login").with(csrf()).param("username", "tester").param("password", "nope"))
                .andExpect(status().isFound()).andExpect(redirectedUrl("/admin/login?error"));

        MvcResult ok = mvc.perform(post("/admin/login").with(csrf()).param("username", "tester").param("password", "test-password-123"))
                .andExpect(status().isFound()).andExpect(redirectedUrl("/admin")).andReturn();
        MockHttpSession session = (MockHttpSession) ok.getRequest().getSession(false);

        mvc.perform(get("/admin").session(session)).andExpect(status().isOk())
                .andExpect(header().string("Cache-Control", containsString("no-store")));
        mvc.perform(get("/admin/api/servers").session(session)).andExpect(status().isOk());

        mvc.perform(post("/admin/logout").with(csrf()).session(session)).andExpect(status().isFound());
        mvc.perform(get("/admin/api/servers").session(session)).andExpect(status().isUnauthorized());
    }

    @Test
    void adminWritesNeedCsrfToken() throws Exception {
        String body = "{\"host\":\"10.0.0.1\",\"port\":27015,\"category\":\"casual\"}";
        mvc.perform(post("/admin/api/servers").with(user("tester").roles("ADMIN"))
                .contentType(MediaType.APPLICATION_JSON).content(body)).andExpect(status().isForbidden());
    }

    @Test
    void gcEndpointsNeedTheApiKey() throws Exception {
        String body = search(1050166997L, 520, "");
        mvc.perform(post("/api/v1/matchmaking/search").contentType(MediaType.APPLICATION_JSON).content(body))
                .andExpect(status().isUnauthorized());
        mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "wrong").contentType(MediaType.APPLICATION_JSON).content(body))
                .andExpect(status().isUnauthorized());
        mvc.perform(post("/api/v1/matchmaking/cancel").contentType(MediaType.APPLICATION_JSON).content("{\"account_id\":1}"))
                .andExpect(status().isUnauthorized());
        mvc.perform(get("/api/v1/matchmaking/searches").header(KEY, "wrong")).andExpect(status().isUnauthorized());
        // the API key does not open the admin panel
        mvc.perform(get("/admin/api/servers").header(KEY, "test-api-key")).andExpect(status().isUnauthorized());
        mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key")
                .contentType(MediaType.APPLICATION_JSON).content(body)).andExpect(status().isOk());
        mvc.perform(get("/api/v1/matchmaking/searches").header(KEY, "test-api-key")).andExpect(status().isOk());
    }

    // ------------------------------------------------------------------ active searches

    @Test
    void searchIsStoredAndListed() throws Exception {
        // game_type 520 = competitive (eGame 8) | mask 2 << 8 (de_dust2), as in the live logs
        mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                        .content(search(1050166997L, 520, "\"mode\":\"competitive\",\"game_mode\":\"competitive\",\"maps\":[\"de_dust2\"],\"request_id\":\"r1\"")))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.result").value("created"))
                .andExpect(jsonPath("$.search.account_id").value(1050166997L))
                .andExpect(jsonPath("$.search.mode").value("competitive"))
                .andExpect(jsonPath("$.search.mode_label").value("Competitive"))
                .andExpect(jsonPath("$.search.accept_required").value(true))
                .andExpect(jsonPath("$.search.status").value("SEARCHING"))
                .andExpect(jsonPath("$.search.steam_id64").value("76561199010432725"));

        mvc.perform(get("/api/v1/matchmaking/searches").header(KEY, "test-api-key"))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.count").value(1))
                .andExpect(jsonPath("$.searches", hasSize(1)))
                .andExpect(jsonPath("$.searches[0].maps[0]").value("de_dust2"))
                .andExpect(jsonPath("$.searches[0].game_type").value(520))
                .andExpect(jsonPath("$.searches[0].e_game").value(8));
    }

    @Test
    void minimalBodyIsEnoughAndModeIsDerived() throws Exception {
        mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                        .content(search(42, 519, "")))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.search.mode").value("casual"))
                .andExpect(jsonPath("$.search.game_mode").value("casual"));
    }

    @Test
    void repeatedSearchDoesNotDuplicate() throws Exception {
        String same = search(7, 520, "\"maps\":[\"de_dust2\"]");
        for (int i = 0; i < 3; i++) {
            mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON).content(same))
                    .andExpect(status().isOk())
                    .andExpect(jsonPath("$.result").value(i == 0 ? "created" : "refreshed"));
        }
        // same account, other mode: rewritten in place
        mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                        .content(search(7, 10, "\"maps\":[\"de_lake\"]")))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.result").value("replaced"))
                .andExpect(jsonPath("$.search.mode").value("wingman"));
        // a retry with the same request_id is idempotent, a new request_id restarts the search
        String withId = search(7, 10, "\"maps\":[\"de_lake\"],\"request_id\":\"abc\"");
        mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON).content(withId))
                .andExpect(jsonPath("$.result").value("replaced"));
        mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON).content(withId))
                .andExpect(jsonPath("$.result").value("refreshed"));

        mvc.perform(get("/api/v1/matchmaking/searches").header(KEY, "test-api-key"))
                .andExpect(jsonPath("$.count").value(1))
                .andExpect(jsonPath("$.searches", hasSize(1)));
    }

    @Test
    void invalidSearchRequestsAreRejected() throws Exception {
        String[] bad = {
                search(0, 520, ""),                                    // account 0
                "{\"game_type\":520}",                                  // no account
                search(5, 99, ""),                                      // eGame 99 is not a category
                search(5, 9, ""),                                       // cooperative is not an MVP category
                search(5, 520, "\"mode\":\"wingman\""),                 // mode does not match game_type
                search(5, 520, "\"mode\":\"nonsense\""),
                search(5, 520, "\"maps\":[\"de dust2; drop\"]"),
                "not json"
        };
        for (String body : bad) {
            mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON).content(body))
                    .andExpect(status().isBadRequest());
        }
        mvc.perform(get("/api/v1/matchmaking/searches").header(KEY, "test-api-key")).andExpect(jsonPath("$.count").value(0));
    }

    @Test
    void cancelEndsTheSearch() throws Exception {
        mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content(search(9, 520, "\"request_id\":\"new\"")));

        // a late cancel of an older search must not kill the current one
        mvc.perform(post("/api/v1/matchmaking/cancel").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                        .content("{\"account_id\":9,\"request_id\":\"old\"}"))
                .andExpect(status().isOk()).andExpect(jsonPath("$.cancelled").value(false))
                .andExpect(jsonPath("$.reason").value("request_id_mismatch"));
        mvc.perform(get("/api/v1/matchmaking/searches").header(KEY, "test-api-key")).andExpect(jsonPath("$.count").value(1));

        mvc.perform(post("/api/v1/matchmaking/cancel").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                        .content("{\"account_id\":9}"))
                .andExpect(jsonPath("$.cancelled").value(true));
        mvc.perform(post("/api/v1/matchmaking/cancel").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                        .content("{\"account_id\":9}"))
                .andExpect(status().isOk()).andExpect(jsonPath("$.cancelled").value(false))
                .andExpect(jsonPath("$.reason").value("no_active_search"));

        mvc.perform(get("/api/v1/matchmaking/searches").header(KEY, "test-api-key")).andExpect(jsonPath("$.count").value(0));
        mvc.perform(get("/api/v1/matchmaking/searches?include_finished=true").header(KEY, "test-api-key"))
                .andExpect(jsonPath("$.searches[0].status").value("CANCELLED"));

        // and the player can search again afterwards
        mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                        .content(search(9, 520, "")))
                .andExpect(jsonPath("$.result").value("created"));
    }

    @Test
    void staleSearchesExpire() throws Exception {
        mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content(search(11, 520, "")));
        clock.advance(Duration.ofMinutes(4));
        mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content(search(12, 519, "")));
        mvc.perform(get("/api/v1/matchmaking/searches").header(KEY, "test-api-key")).andExpect(jsonPath("$.count").value(2));

        clock.advance(Duration.ofMinutes(2));   // 11 is now 6 min old (> 5 min), 12 only 2 min
        mvc.perform(get("/api/v1/matchmaking/searches").header(KEY, "test-api-key"))
                .andExpect(jsonPath("$.count").value(1))
                .andExpect(jsonPath("$.searches[0].account_id").value(12));
        mvc.perform(get("/api/v1/matchmaking/searches?include_finished=true").header(KEY, "test-api-key"))
                .andExpect(jsonPath("$.searches", hasSize(2)))
                .andExpect(jsonPath("$.searches[1].status").value("EXPIRED"));

        // a repeated request refreshes the timeout
        clock.advance(Duration.ofMinutes(4));
        mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content(search(12, 519, "")));
        clock.advance(Duration.ofMinutes(4));
        mvc.perform(get("/api/v1/matchmaking/searches").header(KEY, "test-api-key")).andExpect(jsonPath("$.count").value(1));

        // ended rows are pruned after the retention (1 h): 11 (expired earlier) is gone, 12 expires just now and stays
        clock.advance(Duration.ofHours(2));
        mvc.perform(get("/api/v1/matchmaking/searches?include_finished=true").header(KEY, "test-api-key"))
                .andExpect(jsonPath("$.searches", hasSize(1)))
                .andExpect(jsonPath("$.searches[0].account_id").value(12))
                .andExpect(jsonPath("$.searches[0].status").value("EXPIRED"));
        clock.advance(Duration.ofHours(2));
        mvc.perform(get("/api/v1/matchmaking/searches?include_finished=true").header(KEY, "test-api-key"))
                .andExpect(jsonPath("$.searches", hasSize(0)));
    }

    @Test
    void adminCanAddAndEndATestSearch() throws Exception {
        MvcResult added = mvc.perform(post("/admin/api/searches").with(user("tester").roles("ADMIN")).with(csrf())
                        .contentType(MediaType.APPLICATION_JSON).content("{\"account_id\":123,\"mode\":\"wingman\",\"map\":\"de_lake\"}"))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.source").value("admin"))
                .andExpect(jsonPath("$.maps[0]").value("de_lake"))
                .andReturn();
        String id = com.jayway.jsonpath.JsonPath.read(added.getResponse().getContentAsString(), "$.id").toString();

        mvc.perform(delete("/admin/api/searches/" + id).with(user("tester").roles("ADMIN")).with(csrf()))
                .andExpect(status().isOk()).andExpect(jsonPath("$.status").value("REMOVED"));
        mvc.perform(delete("/admin/api/searches/" + id).with(user("tester").roles("ADMIN")).with(csrf()))
                .andExpect(status().isConflict());
        mvc.perform(delete("/admin/api/searches/99999").with(user("tester").roles("ADMIN")).with(csrf()))
                .andExpect(status().isNotFound());
        mvc.perform(get("/api/v1/matchmaking/searches").with(user("tester").roles("ADMIN")))
                .andExpect(jsonPath("$.count").value(0));
    }

    // ------------------------------------------------------------------ game servers

    @Test
    void gameServerCrud() throws Exception {
        var admin = user("tester").roles("ADMIN");

        MvcResult created = mvc.perform(post("/admin/api/servers").with(admin).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                        .content("{\"host\":\"192.168.1.150\",\"port\":27016,\"category\":\"competitive\",\"map\":\"de_dust2\"}"))
                .andExpect(status().isCreated())
                .andExpect(jsonPath("$.category").value("competitive"))
                .andExpect(jsonPath("$.category_label").value("Competitive"))
                .andExpect(jsonPath("$.enabled").value(true))
                .andExpect(jsonPath("$.created_at").exists())
                .andReturn();
        String id = com.jayway.jsonpath.JsonPath.read(created.getResponse().getContentAsString(), "$.id").toString();

        // duplicate host:port
        mvc.perform(post("/admin/api/servers").with(admin).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                        .content("{\"host\":\"192.168.1.150\",\"port\":27016,\"category\":\"casual\"}"))
                .andExpect(status().isConflict());

        // invalid input
        String[] bad = {
                "{\"host\":\"\",\"port\":27016,\"category\":\"casual\"}",
                "{\"host\":\"a b\",\"port\":27016,\"category\":\"casual\"}",
                "{\"host\":\"h\",\"port\":0,\"category\":\"casual\"}",
                "{\"host\":\"h\",\"port\":70000,\"category\":\"casual\"}",
                "{\"host\":\"h\",\"port\":27016,\"category\":\"cooperative\"}",
                "{\"host\":\"h\",\"port\":27016,\"category\":\"casual\",\"map\":\"../x\"}"
        };
        for (String body : bad) {
            mvc.perform(post("/admin/api/servers").with(admin).with(csrf()).contentType(MediaType.APPLICATION_JSON).content(body))
                    .andExpect(status().isBadRequest());
        }

        mvc.perform(put("/admin/api/servers/" + id).with(admin).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                        .content("{\"host\":\"SRCDS.example.com\",\"port\":27017,\"category\":\"wingman\",\"map\":\"de_lake\",\"enabled\":false}"))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$.host").value("srcds.example.com"))
                .andExpect(jsonPath("$.category").value("wingman"))
                .andExpect(jsonPath("$.enabled").value(false));

        mvc.perform(post("/admin/api/servers/" + id + "/enabled").with(admin).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                        .content("{\"enabled\":true}"))
                .andExpect(status().isOk()).andExpect(jsonPath("$.enabled").value(true));

        mvc.perform(get("/admin/api/servers").with(admin)).andExpect(jsonPath("$", hasSize(1)));

        mvc.perform(delete("/admin/api/servers/" + id).with(admin).with(csrf())).andExpect(status().isNoContent());
        mvc.perform(delete("/admin/api/servers/" + id).with(admin).with(csrf())).andExpect(status().isNotFound());
        mvc.perform(put("/admin/api/servers/" + id).with(admin).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                .content("{\"host\":\"h\",\"port\":1,\"category\":\"casual\"}")).andExpect(status().isNotFound());
        mvc.perform(get("/admin/api/servers").with(admin)).andExpect(jsonPath("$", hasSize(0)));
    }

    @Test
    void wrongMethodAndUnknownPathKeepTheirStatus() throws Exception {
        var admin = user("tester").roles("ADMIN");
        mvc.perform(post("/admin/api/servers/1").with(admin).with(csrf()))
                .andExpect(status().isMethodNotAllowed()).andExpect(jsonPath("$.error").value("method_not_allowed"));
        mvc.perform(get("/admin/api/nothing-here").with(admin))
                .andExpect(status().isNotFound()).andExpect(jsonPath("$.error").value("not_found"));
        mvc.perform(post("/admin/api/servers").with(admin).with(csrf()).contentType(MediaType.TEXT_PLAIN).content("x"))
                .andExpect(status().isUnsupportedMediaType());
        // an unauthenticated caller must not learn which paths exist
        mvc.perform(get("/admin/api/nothing-here")).andExpect(status().isUnauthorized());
    }

    @Test
    void categoriesAreTheMvpSet() throws Exception {
        mvc.perform(get("/admin/api/categories").with(user("tester").roles("ADMIN")))
                .andExpect(status().isOk())
                .andExpect(jsonPath("$", hasSize(9)))
                .andExpect(jsonPath("$[?(@.key=='scrimcomp5v5')]", hasSize(1)))   // an Accept mode like Competitive (#65)
                .andExpect(jsonPath("$[?(@.key=='cooperative')]", hasSize(0)))
                .andExpect(jsonPath("$[?(@.key=='dangerzone')].required_players").value(org.hamcrest.Matchers.contains(16)))
                .andExpect(jsonPath("$[?(@.key=='wingman')].required_players").value(org.hamcrest.Matchers.contains(4)))
                .andExpect(jsonPath("$[?(@.key=='competitive')].required_players").value(org.hamcrest.Matchers.contains(10)));
    }
}
