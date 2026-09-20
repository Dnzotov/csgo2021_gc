package dev.csgogc.mm;

import static org.assertj.core.api.Assertions.assertThat;
import static org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.csrf;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.put;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasterxml.jackson.databind.JsonNode;
import java.nio.file.Path;
import org.junit.jupiter.api.Test;
import org.springframework.http.MediaType;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;

/**
 * Fake Players from the admin panel (RESEARCH_FINDINGS.md #54, #67): a profile per mode says how many virtual players a match
 * gets, the panel edits it, the next match uses it. This class covers the profile itself (API, validation, settings, the roster
 * of a match); FakeProfileConfigTest covers what a configuration does to real matchmaking.
 */
class FakePlayersIntegrationTest extends BackendTestBase {

    private static final Path DB_DIR = tempDir("mm-backend-fake-test");

    @DynamicPropertySource
    static void database(DynamicPropertyRegistry registry) {
        registry.add("backend.database-path", () -> DB_DIR.resolve("mm.db").toString());
    }

    private static final long REAL = 1050166997L;

    // ------------------------------------------------------------------------------------------ the profile is a setting

    @Test
    void nineFakePlayersAndOneRealPlayerMakeACompetitiveMatchOfTen() throws Exception {
        long serverId = addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        long fakeId = addFake("competitive", 9, "\"de_dust2\",\"de_mirage\"");
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("ON");
        assertThat(fake(fakeId).get("matches").size()).isZero();

        // a profile alone never starts a match nor holds a server
        assertThat(matches().size()).isZero();
        assertThat(server(serverId).get("state").asText()).isEqualTo("AVAILABLE");

        JsonNode real = search(REAL, COMPETITIVE, "\"de_dust2\"", "real-1");
        assertThat(real.get("status").asText()).isEqualTo("WAITING_ACCEPT");
        JsonNode assignment = real.get("assignment");
        assertThat(assignment.get("server_port").asInt()).isEqualTo(27017);
        assertThat(assignment.get("accept_required").asBoolean()).isTrue();
        assertThat(assignment.get("required_players").asInt()).isEqualTo(10);        // the roster that is armed
        assertThat(real.get("match").get("players").asInt()).isEqualTo(10);
        assertThat(real.get("match").get("required_players").asInt()).isEqualTo(10);   // the capacity of the mode
        assertThat(real.get("match").get("fake_players").asInt()).isEqualTo(9);

        // the roster of the match: the real player + 9 virtual ones with deterministic ids 0xFA4E0000 + n
        JsonNode players = assignment.get("players");
        assertThat(players.size()).isEqualTo(10);
        assertThat(players.get(0).get("account_id").asLong()).isEqualTo(REAL);
        assertThat(players.get(0).get("fake").asBoolean()).isFalse();
        for (int i = 1; i <= 9; i++) {
            assertThat(players.get(i).get("account_id").asLong()).isEqualTo(0xFA4E0000L + i);
            assertThat(players.get(i).get("fake").asBoolean()).isTrue();
        }

        // the profile shows the live matches that used it; it stays ON and is used by the next match as well
        JsonNode profile = fake(fakeId);
        assertThat(profile.get("status").asText()).isEqualTo("ON");
        assertThat(profile.get("matches").size()).isEqualTo(1);
        JsonNode used = profile.get("matches").get(0);
        assertThat(used.get("match_id").asText()).isEqualTo(assignment.get("match_id").asText());
        assertThat(used.get("server_address").asText()).isEqualTo("192.168.1.150");
        assertThat(used.get("real_players").asInt()).isEqualTo(1);
        assertThat(used.get("fake_players").asInt()).isEqualTo(9);
        assertThat(used.get("configured").asInt()).isEqualTo(9);
        assertThat(server(serverId).get("state").asText()).isEqualTo("RESERVED");

        JsonNode match = matches().get(0);
        assertThat(match.get("players").asInt()).isEqualTo(10);
        assertThat(match.get("fake_players").asInt()).isEqualTo(9);
        assertThat(match.get("account_ids").size()).isEqualTo(1);   // the real ones only
        assertThat(match.get("roster").size()).isEqualTo(10);
    }

    @Test
    void aCompleteMatchKeepsItsSizeWhenTheRealPlayerSendsStopAtConnect() throws Exception {
        addServer("10.0.0.5", 27018, "wingman", "de_lake");
        long fakeId = addFake("wingman", 3, null);
        search(1, WINGMAN, "\"de_lake\"", "w");
        assertThat(accepted(1, "w").get("match_accepted").asBoolean()).isTrue();   // everybody accepted: the client connects
        cancel(1, "w");                                                    // ... and sends MatchmakingStop when it does
        JsonNode match = matches().get(0);
        assertThat(match.get("status").asText()).isEqualTo("ACCEPTED");
        assertThat(match.get("players").asInt()).isEqualTo(4);
        assertThat(match.get("fake_players").asInt()).isEqualTo(3);
        assertThat(fake(fakeId).get("matches").get(0).get("fake_players").asInt()).isEqualTo(3);
    }

    @Test
    void wingmanTakesThreeFakePlayersAndDangerZoneFifteen() throws Exception {
        addServer("10.0.0.5", 27018, "wingman", "de_lake");
        addServer("10.0.0.5", 27019, "dangerzone", "dz_blacksite");
        addFake("wingman", 3, "\"de_lake\"");
        addFake("dangerzone", 15, "\"dz_blacksite\"");

        JsonNode wingman = search(1, WINGMAN, "\"de_lake\"", "w");
        assertThat(wingman.get("status").asText()).isEqualTo("WAITING_ACCEPT");
        assertThat(wingman.get("match").get("players").asInt()).isEqualTo(4);

        JsonNode dz = search(2, DANGERZONE, "\"dz_blacksite\",\"dz_sirocco\"", "d");
        assertThat(dz.get("status").asText()).isEqualTo("WAITING_ACCEPT");
        assertThat(dz.get("match").get("players").asInt()).isEqualTo(16);
        assertThat(dz.get("assignment").get("players").size()).isEqualTo(16);
    }

    /**
     * gather-window = 0 (this class): the virtual players are decided at once. What happened live before #66 and why the window
     * exists: the first real player gets his match (1 + 9), the second finds it full and reserved and is left in a match of its
     * own - Match Found for one player only. GatherWindowTest / FakeProfileConfigTest are the behaviour with a window.
     */
    @Test
    void withoutAGatherWindowTheSecondRealPlayerIsLeftBehind() throws Exception {
        addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, "\"de_dust2\"");

        JsonNode first = search(1, COMPETITIVE, "\"de_dust2\"", "a");
        assertThat(first.get("status").asText()).isEqualTo("WAITING_ACCEPT");   // 1 + 9 = 10: complete for ONE real player
        JsonNode second = search(2, COMPETITIVE, "\"de_dust2\"", "b");
        assertThat(second.get("status").asText()).isEqualTo("MATCHED");         // in a match of its own, no Match Found
        assertThat(second.get("match").get("match_id").asText()).isNotEqualTo(first.get("match").get("match_id").asText());
        assertThat(second.has("assignment")).isFalse();
    }

    @Test
    void aProfileDoesNotAffectNormalSearches() throws Exception {
        addServer("10.0.0.5", 27016, "casual", "de_dust2");
        addFake("competitive", 9, null);
        JsonNode casual = search(1, CASUAL, "\"de_dust2\"", "c");
        assertThat(casual.get("status").asText()).isEqualTo("READY_TO_CONNECT");   // a classic mode never involves virtual players
        assertThat(casual.get("match").get("fake_players").asInt()).isZero();
    }

    @Test
    void theProfileIsNotUsedUpAndEveryMatchGetsItsFakePlayersAgain() throws Exception {
        addServer("10.0.0.5", 27018, "wingman", "de_lake");
        addServer("10.0.0.6", 27018, "wingman", "de_lake");
        long fakeId = addFake("wingman", 3, "\"de_lake\"");
        JsonNode a = search(1, WINGMAN, "\"de_lake\"", "a");
        JsonNode b = search(2, WINGMAN, "\"de_lake\"", "b");                 // second match on the second server, same profile
        assertThat(a.get("status").asText()).isEqualTo("WAITING_ACCEPT");
        assertThat(b.get("status").asText()).isEqualTo("WAITING_ACCEPT");
        assertThat(b.get("match").get("match_id").asText()).isNotEqualTo(a.get("match").get("match_id").asText());
        assertThat(a.get("assignment").get("fake_players").asInt()).isEqualTo(3);
        assertThat(b.get("assignment").get("fake_players").asInt()).isEqualTo(3);
        assertThat(fake(fakeId).get("matches").size()).isEqualTo(2);
    }

    // ------------------------------------------------------------------------------------------ the panel API

    @Test
    void validationOnlyAcceptModesAndSensiblePlayerCounts() throws Exception {
        long competitive = addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        long wingman = addServer("10.0.0.5", 27018, "wingman", "de_lake");
        String ok = "{\"mode\":\"competitive\",\"players\":0}";
        post("/admin/api/fake-searches", ok).andExpect(status().isCreated());                                      // 0 is a valid count
        post("/admin/api/fake-searches", "{\"mode\":\"competitive\",\"players\":9}").andExpect(status().isCreated());
        post("/admin/api/fake-searches", "{\"mode\":\"competitive\",\"players\":10}").andExpect(status().isBadRequest());   // capacity - 1
        post("/admin/api/fake-searches", "{\"mode\":\"competitive\",\"players\":-1}").andExpect(status().isBadRequest());
        post("/admin/api/fake-searches", "{\"mode\":\"wingman\",\"players\":4}").andExpect(status().isBadRequest());
        post("/admin/api/fake-searches", "{\"mode\":\"dangerzone\",\"players\":16}").andExpect(status().isBadRequest());
        post("/admin/api/fake-searches", "{\"mode\":\"casual\",\"players\":1}").andExpect(status().isBadRequest());   // no gathering
        post("/admin/api/fake-searches", "{\"mode\":\"nope\",\"players\":1}").andExpect(status().isBadRequest());
        post("/admin/api/fake-searches", "{\"mode\":\"competitive\",\"players\":3,\"maps\":[\"bad map\"]}").andExpect(status().isBadRequest());
        post("/admin/api/fake-searches", "{\"mode\":\"competitive\",\"players\":3,\"priority\":5000}").andExpect(status().isBadRequest());

        // a server binding has to be a server of that mode, and its map one of the maps of the profile
        post("/admin/api/fake-searches", "{\"mode\":\"competitive\",\"players\":3,\"server_id\":" + competitive + "}").andExpect(status().isCreated());
        post("/admin/api/fake-searches", "{\"mode\":\"competitive\",\"players\":3,\"server_id\":" + wingman + "}").andExpect(status().isBadRequest());
        post("/admin/api/fake-searches", "{\"mode\":\"competitive\",\"players\":3,\"server_id\":9999}").andExpect(status().isBadRequest());
        post("/admin/api/fake-searches",
                "{\"mode\":\"competitive\",\"players\":3,\"maps\":[\"de_mirage\"],\"server_id\":" + competitive + "}").andExpect(status().isBadRequest());
        post("/admin/api/fake-searches",
                "{\"mode\":\"competitive\",\"players\":3,\"maps\":[\"de_dust2\"],\"server_id\":" + competitive + "}").andExpect(status().isCreated());
    }

    private org.springframework.test.web.servlet.ResultActions post(String url, String content) throws Exception {
        return mvc.perform(org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post(url).with(ADMIN).with(csrf())
                .contentType(MediaType.APPLICATION_JSON).content(content));
    }

    @Test
    void theFakePlayersApiNeedsAnAdminSessionAndACsrfToken() throws Exception {
        mvc.perform(get("/admin/api/fake-searches")).andExpect(status().isUnauthorized());
        mvc.perform(org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post("/admin/api/fake-searches").with(ADMIN)
                .contentType(MediaType.APPLICATION_JSON).content("{\"mode\":\"wingman\",\"players\":2}")).andExpect(status().isForbidden());   // no CSRF
        mvc.perform(put("/admin/api/fake-settings").with(ADMIN).contentType(MediaType.APPLICATION_JSON).content("{\"master\":false}"))
                .andExpect(status().isForbidden());
        mvc.perform(get("/api/v1/matchmaking/searches")).andExpect(status().isUnauthorized());   // and the GC API key does not open it
    }

    @Test
    void editingSwitchingDeletingAndTheListOfProfiles() throws Exception {
        long server = addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        long a = addFake("competitive", 7, "\"de_dust2\"");
        long b = addFake("wingman", 2, null, false, 3, null);
        JsonNode listed = fakeOverview();
        assertThat(listed.get("enabled").asBoolean()).isTrue();
        assertThat(listed.get("master").asBoolean()).isTrue();
        assertThat(listed.get("fake_searches").size()).isEqualTo(2);
        assertThat(fake(a).get("max_players").asInt()).isEqualTo(9);
        assertThat(fake(b).get("max_players").asInt()).isEqualTo(3);
        assertThat(fake(b).get("status").asText()).isEqualTo("OFF");
        assertThat(fake(b).get("priority").asInt()).isEqualTo(3);

        putFake(a, "{\"mode\":\"competitive\",\"players\":2,\"maps\":[\"de_dust2\",\"de_mirage\"],\"priority\":9,\"server_id\":" + server + "}")
                .andExpect(status().isOk());
        JsonNode edited = fake(a);
        assertThat(edited.get("players").asInt()).isEqualTo(2);
        assertThat(edited.get("maps").size()).isEqualTo(2);
        assertThat(edited.get("priority").asInt()).isEqualTo(9);
        assertThat(edited.get("server_id").asLong()).isEqualTo(server);
        assertThat(edited.get("server_label").asText()).contains("10.0.0.5:27017").contains("de_dust2");

        fakeEnabled(b, true);
        assertThat(fake(b).get("status").asText()).isEqualTo("ON");
        fakeEnabled(b, false);
        assertThat(fake(b).get("enabled").asBoolean()).isFalse();
        deleteFake(a);
        assertThat(fakeOverview().get("fake_searches").size()).isEqualTo(1);
        putFake(a, "{\"mode\":\"competitive\",\"players\":1}").andExpect(status().isNotFound());
    }

    /** the panel edits the master switch and the gather window, they are stored in the database */
    @Test
    void theMasterSwitchAndTheGatherWindowAreSettingsOfThePanel() throws Exception {
        assertThat(fakeOverview().get("master").asBoolean()).isTrue();
        fakeMaster(false);
        assertThat(fakeOverview().get("master").asBoolean()).isFalse();
        gatherWindow(25);
        JsonNode settings = fakeOverview();
        assertThat(settings.get("master").asBoolean()).isFalse();            // setting one does not touch the other
        assertThat(settings.get("gather_window_seconds").asInt()).isEqualTo(25);
        mvc.perform(put("/admin/api/fake-settings").with(ADMIN).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                .content("{\"gather_window_seconds\":601}")).andExpect(status().isBadRequest());
        mvc.perform(put("/admin/api/fake-settings").with(ADMIN).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                .content("{\"gather_window_seconds\":-1}")).andExpect(status().isBadRequest());
        fakeMaster(true);
        assertThat(fakeOverview().get("master").asBoolean()).isTrue();
        assertThat(jdbc.sql("SELECT value FROM backend_setting WHERE key = 'fake.gather_window_seconds'").query(String.class).single())
                .isEqualTo("25");
    }

    @Test
    void theSrcdsSideCanFetchTheRosterOfTheMatchOnItsServer() throws Exception {
        addServer("10.0.0.5", 27018, "wingman", "de_lake");
        addFake("wingman", 3, "\"de_lake\"");
        mvc.perform(get("/api/v1/servers/roster?address=10.0.0.5&port=27018").header(KEY, "test-api-key"))
                .andExpect(status().isNotFound());                             // no match on it yet
        search(REAL, WINGMAN, "\"de_lake\"", "w");
        JsonNode roster = body(mvc.perform(get("/api/v1/servers/roster?address=10.0.0.5&port=27018").header(KEY, "test-api-key"))
                .andExpect(status().isOk()));
        assertThat(roster.get("mode").asText()).isEqualTo("wingman");
        assertThat(roster.get("status").asText()).isEqualTo("READY");
        assertThat(roster.get("required_players").asInt()).isEqualTo(4);
        assertThat(roster.get("real_players").asInt()).isEqualTo(1);
        assertThat(roster.get("fake_players").asInt()).isEqualTo(3);
        assertThat(roster.get("players").size()).isEqualTo(4);
        assertThat(roster.get("players").get(0).get("account_id").asLong()).isEqualTo(REAL);
        mvc.perform(get("/api/v1/servers/roster?address=10.9.9.9&port=1").header(KEY, "test-api-key")).andExpect(status().isNotFound());
        mvc.perform(get("/api/v1/servers/roster?address=10.0.0.5&port=27018")).andExpect(status().isUnauthorized());
    }
}
