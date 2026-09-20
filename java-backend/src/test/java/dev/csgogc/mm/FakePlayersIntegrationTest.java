package dev.csgogc.mm;

import static org.assertj.core.api.Assertions.assertThat;
import static org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.csrf;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasterxml.jackson.databind.JsonNode;
import java.nio.file.Path;
import java.time.Duration;
import org.junit.jupiter.api.Test;
import org.springframework.http.MediaType;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;

/**
 * Fake players: virtual matchmaking participants added from the admin panel (RESEARCH_FINDINGS.md #54). Retail player
 * counts (Competitive 10, Wingman 4, Danger Zone 16): the real player plus the fake ones complete the match.
 */
class FakePlayersIntegrationTest extends BackendTestBase {

    private static final Path DB_DIR = tempDir("mm-backend-fake-test");

    @DynamicPropertySource
    static void database(DynamicPropertyRegistry registry) {
        registry.add("backend.database-path", () -> DB_DIR.resolve("mm.db").toString());
    }

    private static final long REAL = 1050166997L;

    // ------------------------------------------------------------------------------------------ filling a match

    @Test
    void nineFakePlayersAndOneRealPlayerMakeACompetitiveMatchOfTen() throws Exception {
        long serverId = addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        long fakeId = addFake("competitive", 9, "\"de_dust2\",\"de_mirage\"");
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("SEARCHING");
        assertThat(fake(fakeId).hasNonNull("match")).isFalse();

        // a fake search alone never starts a match nor holds a server
        assertThat(matches().size()).isZero();
        assertThat(server(serverId).get("state").asText()).isEqualTo("AVAILABLE");

        JsonNode real = search(REAL, COMPETITIVE, "\"de_dust2\"", "real-1");
        assertThat(real.get("status").asText()).isEqualTo("WAITING_ACCEPT");
        JsonNode assignment = real.get("assignment");
        assertThat(assignment.get("server_port").asInt()).isEqualTo(27017);
        assertThat(assignment.get("accept_required").asBoolean()).isTrue();
        assertThat(assignment.get("required_players").asInt()).isEqualTo(10);
        assertThat(real.get("match").get("players").asInt()).isEqualTo(10);
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

        // the fake search shows where it went; the server is reserved for the match as for any Accept match
        JsonNode joined = fake(fakeId);
        assertThat(joined.get("status").asText()).isEqualTo("MATCHED");
        assertThat(joined.get("match").get("match_id").asText()).isEqualTo(assignment.get("match_id").asText());
        assertThat(joined.get("match").get("server_address").asText()).isEqualTo("192.168.1.150");
        assertThat(joined.get("match").get("server_port").asInt()).isEqualTo(27017);
        assertThat(joined.get("match").get("players").asInt()).isEqualTo(10);
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
        assertThat(fake(fakeId).get("match").get("players").asInt()).isEqualTo(4);
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("MATCHED");
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

    @Test
    void severalFakeSearchesAddUpAndOneThatDoesNotFitWaits() throws Exception {
        addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        long seven = addFake("competitive", 7, "\"de_dust2\"");
        long five = addFake("competitive", 5, "\"de_dust2\"");
        long four = addFake("competitive", 4, null);              // no maps = any map

        // 1 real + 7 = 8 leaves 2 places: the searches of 5 and 4 do not fit, a search of 2 would
        JsonNode first = search(REAL, COMPETITIVE, "\"de_dust2\"", "a");
        assertThat(first.get("match").get("players").asInt()).isEqualTo(8);
        assertThat(first.get("status").asText()).isEqualTo("MATCHED");
        assertThat(fake(seven).get("status").asText()).isEqualTo("MATCHED");
        assertThat(fake(five).get("status").asText()).isEqualTo("SEARCHING");
        assertThat(fake(four).get("status").asText()).isEqualTo("SEARCHING");

        long two = addFake("competitive", 2, "\"de_dust2\"");
        assertThat(fake(two).get("status").asText()).isEqualTo("MATCHED");
        assertThat(poll("a").get("status").asText()).isEqualTo("WAITING_ACCEPT");
        assertThat(fake(five).get("status").asText()).isEqualTo("SEARCHING");
    }

    @Test
    void aFakeSearchWaitsForARealMatchAndJoinsWhenItAppears() throws Exception {
        long fakeId = addFake("wingman", 3, "\"de_lake\"");
        JsonNode real = search(1, WINGMAN, "\"de_lake\"", "w");          // no server yet: nothing to join
        assertThat(real.get("status").asText()).isEqualTo("SEARCHING");
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("SEARCHING");

        addServer("10.0.0.5", 27018, "wingman", "de_lake");            // adding a server triggers the matcher
        assertThat(poll("w").get("status").asText()).isEqualTo("WAITING_ACCEPT");
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("MATCHED");
    }

    // ------------------------------------------------------------------------------------------ what a fake search must not do

    @Test
    void aFakeSearchDoesNotAffectNormalSearchesWhenNoMatchIsForming() throws Exception {
        addServer("10.0.0.5", 27016, "casual", "de_dust2");
        long fakeId = addFake("competitive", 9, "\"de_dust2\"");

        JsonNode casual = search(1, CASUAL, "\"de_dust2\"", "c");        // a classic search: nothing to do with them
        assertThat(casual.get("status").asText()).isEqualTo("READY_TO_CONNECT");
        assertThat(casual.get("assignment").get("players").size()).isEqualTo(1);
        assertThat(casual.get("match").get("fake_players").asInt()).isZero();
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("SEARCHING");

        // a real Competitive search without a server just waits, the fake search does not conjure a server
        JsonNode waiting = search(2, COMPETITIVE, "\"de_dust2\"", "w");
        assertThat(waiting.get("status").asText()).isEqualTo("SEARCHING");
    }

    @Test
    void aDisabledFakeSearchNeverJoinsAndAStoppedOneLeavesAFormingMatch() throws Exception {
        addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        long off = addFake("competitive", 9, "\"de_dust2\"", false);
        assertThat(fake(off).get("status").asText()).isEqualTo("STOPPED");
        assertThat(fake(off).get("enabled").asBoolean()).isFalse();

        JsonNode real = search(REAL, COMPETITIVE, "\"de_dust2\"", "a");
        assertThat(real.get("status").asText()).isEqualTo("MATCHED");
        assertThat(real.get("match").get("players").asInt()).isEqualTo(1);

        long five = addFake("competitive", 5, null);
        assertThat(poll("a").get("match").get("players").asInt()).isEqualTo(6);
        fakeEnabled(five, false);                                          // Stop: back to 1/10
        assertThat(fake(five).get("status").asText()).isEqualTo("STOPPED");
        assertThat(fake(five).hasNonNull("match")).isFalse();
        assertThat(poll("a").get("match").get("players").asInt()).isEqualTo(1);
        assertThat(poll("a").get("status").asText()).isEqualTo("MATCHED");

        fakeEnabled(five, true);                                           // Start: joins again
        assertThat(fake(five).get("status").asText()).isEqualTo("MATCHED");
        assertThat(poll("a").get("match").get("players").asInt()).isEqualTo(6);
        assertThat(fake(off).get("status").asText()).isEqualTo("STOPPED"); // the disabled one was never touched
    }

    @Test
    void theMapOfTheServerHasToBeOneOfTheFakeSearchMaps() throws Exception {
        addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        long wrongMap = addFake("competitive", 9, "\"de_mirage\",\"de_inferno\"");
        JsonNode real = search(REAL, COMPETITIVE, "\"de_dust2\"", "a");
        assertThat(real.get("status").asText()).isEqualTo("MATCHED");
        assertThat(fake(wrongMap).get("status").asText()).isEqualTo("SEARCHING");

        putFake(wrongMap, "{\"mode\":\"competitive\",\"players\":9,\"maps\":[\"de_dust2\"]}").andExpect(status().isOk());
        assertThat(fake(wrongMap).get("status").asText()).isEqualTo("MATCHED");
        assertThat(poll("a").get("status").asText()).isEqualTo("WAITING_ACCEPT");
    }

    @Test
    void aFakeSearchOfAnotherModeIsNotUsed() throws Exception {
        addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        long wingman = addFake("wingman", 3, null);
        JsonNode real = search(REAL, COMPETITIVE, "\"de_dust2\"", "a");
        assertThat(real.get("match").get("players").asInt()).isEqualTo(1);
        assertThat(fake(wingman).get("status").asText()).isEqualTo("SEARCHING");
    }

    // ------------------------------------------------------------------------------------------ the life of a match

    @Test
    void aFormingMatchThatFallsApartQueuesTheFakePlayersAgain() throws Exception {
        long serverId = addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        long fakeId = addFake("competitive", 5, null);
        search(REAL, COMPETITIVE, "\"de_dust2\"", "a");
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("MATCHED");

        cancel(REAL, "a");                                                 // the only real player leaves
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("SEARCHING");
        assertThat(fake(fakeId).hasNonNull("match")).isFalse();
        assertThat(server(serverId).get("state").asText()).isEqualTo("AVAILABLE");
        assertThat(matches().get(0).get("status").asText()).isEqualTo("CANCELLED");
    }

    @Test
    void theFakePlayersFinishWithTheMatchAndCanBeStartedAgain() throws Exception {
        long serverId = addServer("10.0.0.5", 27018, "wingman", "de_lake");
        long fakeId = addFake("wingman", 3, "\"de_lake\"");
        search(1, WINGMAN, "\"de_lake\"", "a");
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("MATCHED");
        assertThat(server(serverId).get("state").asText()).isEqualTo("RESERVED");
        accepted(1, "a");                                                  // the Accept went through: the match is ACCEPTED

        // the real player is alone with virtual ones: starting a new search after that ends the old match (it was played)
        search(1, WINGMAN, "\"de_lake\"", "b");
        assertThat(matches().size()).isEqualTo(2);
        int ended = 0;
        for (JsonNode m : matches()) {          // (the clock is frozen: both matches have the same creation time)
            ended += m.get("status").asText().equals("ENDED") ? 1 : 0;
        }
        assertThat(ended).isEqualTo(1);
        // the old match ended (its fake players are COMPLETED and stay out of the new match until the admin starts them)
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("COMPLETED");
        assertThat(poll("b").get("status").asText()).isEqualTo("MATCHED");
        fakeEnabled(fakeId, true);                                         // Start again
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("MATCHED");
        assertThat(poll("b").get("status").asText()).isEqualTo("WAITING_ACCEPT");
    }

    @Test
    void anAcceptThatDidNotGoThroughCancelsTheMatchAndTheFakePlayersSearchAgainOnTheirOwn() throws Exception {
        addServer("10.0.0.5", 27018, "wingman", "de_lake");
        long fakeId = addFake("wingman", 3, "\"de_lake\"");
        search(1, WINGMAN, "\"de_lake\"", "a");                            // WAITING_ACCEPT, nobody accepted

        // starting over (the Accept failed): the old match is CANCELLED, not ENDED - the fake players are searching again
        // and are part of the very next match without anybody pressing Start
        search(1, WINGMAN, "\"de_lake\"", "b");
        int cancelled = 0;
        String cancelledId = null;
        for (JsonNode m : matches()) {
            if (m.get("status").asText().equals("CANCELLED")) {
                cancelled++;
                cancelledId = m.get("id").asText();
            }
        }
        assertThat(cancelled).isEqualTo(1);
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("MATCHED");
        assertThat(fake(fakeId).get("match").get("match_id").asText()).isNotEqualTo(cancelledId);
    }

    @Test
    void theReservationTtlEndsAMatchWithFakePlayersAndFreesTheServer() throws Exception {
        long serverId = addServer("10.0.0.5", 27018, "wingman", "de_lake");
        long fakeId = addFake("wingman", 3, null);
        search(1, WINGMAN, "\"de_lake\"", "a");
        accepted(1, "a");                                                  // the match is ACCEPTED: the TTL ends it
        clock.advance(Duration.ofMinutes(11));
        tick();
        assertThat(server(serverId).get("state").asText()).isEqualTo("AVAILABLE");
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("COMPLETED");
        assertThat(fake(fakeId).get("match").get("match_status").asText()).isEqualTo("ENDED");
    }

    @Test
    void anAdminTakingTheServerAwayCancelsTheMatchAndTheNextOneWaitsForAFreeServer() throws Exception {
        long serverId = addServer("10.0.0.5", 27018, "wingman", "de_lake");
        long fakeId = addFake("wingman", 3, null);
        search(REAL, WINGMAN, "\"de_lake\"", "a");                          // full: the server is RESERVED, Accept is running
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("MATCHED");
        String first = poll("a").get("match").get("match_id").asText();
        mvc.perform(post("/admin/api/servers/" + serverId + "/state").with(ADMIN).with(csrf())
                .contentType(MediaType.APPLICATION_JSON).content("{\"state\":\"BUSY\"}")).andExpect(status().isOk());

        // the first match is cancelled, everybody searches again and gathers a new match, which waits: the only server is BUSY
        JsonNode again = poll("a");
        assertThat(again.get("status").asText()).isEqualTo("MATCHED");
        assertThat(again.get("match").get("match_id").asText()).isNotEqualTo(first);
        assertThat(again.get("match").get("status").asText()).isEqualTo("FULL");
        assertThat(again.has("assignment")).isFalse();
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("MATCHED");
        assertThat(fake(fakeId).get("match").get("match_id").asText()).isEqualTo(again.get("match").get("match_id").asText());
    }

    // ------------------------------------------------------------------------------------------ admin API

    @Test
    void validationOnlyAcceptModesAndSensiblePlayerCounts() throws Exception {
        String url = "/admin/api/fake-searches";
        // classic mode: one search is complete on its own, fake players make no sense
        mvc.perform(post(url).with(ADMIN).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                .content("{\"mode\":\"casual\",\"players\":3}")).andExpect(status().isBadRequest());
        // Competitive is 10: at most 9 fake players (one real player is needed to start it)
        mvc.perform(post(url).with(ADMIN).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                .content("{\"mode\":\"competitive\",\"players\":10}")).andExpect(status().isBadRequest());
        mvc.perform(post(url).with(ADMIN).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                .content("{\"mode\":\"wingman\",\"players\":4}")).andExpect(status().isBadRequest());
        mvc.perform(post(url).with(ADMIN).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                .content("{\"mode\":\"wingman\",\"players\":0}")).andExpect(status().isBadRequest());
        mvc.perform(post(url).with(ADMIN).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                .content("{\"mode\":\"nope\",\"players\":2}")).andExpect(status().isBadRequest());
        mvc.perform(post(url).with(ADMIN).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                .content("{\"mode\":\"wingman\",\"players\":2,\"maps\":[\"de lake!\"]}")).andExpect(status().isBadRequest());
        assertThat(body(mvc.perform(get(url).with(ADMIN))).get("fake_searches").size()).isZero();
    }

    @Test
    void theFakePlayersApiNeedsAnAdminSessionAndACsrfToken() throws Exception {
        mvc.perform(get("/admin/api/fake-searches")).andExpect(status().isUnauthorized());
        mvc.perform(post("/admin/api/fake-searches").with(csrf()).contentType(MediaType.APPLICATION_JSON)
                .content("{\"mode\":\"wingman\",\"players\":2}")).andExpect(status().isUnauthorized());
        mvc.perform(post("/admin/api/fake-searches").with(ADMIN).contentType(MediaType.APPLICATION_JSON)
                .content("{\"mode\":\"wingman\",\"players\":2}")).andExpect(status().isForbidden());   // no CSRF token
        // the GC API key does not open the admin API
        mvc.perform(get("/admin/api/fake-searches").header(KEY, "test-api-key")).andExpect(status().isUnauthorized());
    }

    @Test
    void editingDeletingAndTheListOfFakeSearches() throws Exception {
        long a = addFake("competitive", 9, "\"de_dust2\"");
        long b = addFake("wingman", 3, null, false);
        assertThat(body(mvc.perform(get("/admin/api/fake-searches").with(ADMIN))).get("enabled").asBoolean()).isTrue();
        assertThat(body(mvc.perform(get("/admin/api/fake-searches").with(ADMIN))).get("fake_searches").size()).isEqualTo(2);

        putFake(a, "{\"mode\":\"competitive\",\"players\":6,\"maps\":[\"de_mirage\",\"de_mirage\",\"de_inferno\"]}")
                .andExpect(status().isOk());
        JsonNode edited = fake(a);
        assertThat(edited.get("players").asInt()).isEqualTo(6);
        assertThat(edited.get("maps").size()).isEqualTo(2);            // duplicates removed
        putFake(a, "{\"mode\":\"wingman\",\"players\":6}").andExpect(status().isBadRequest());

        // a fake search that sits in a match has to be stopped before it is changed
        addServer("10.0.0.5", 27017, "competitive", "de_mirage");
        search(REAL, COMPETITIVE, "\"de_mirage\"", "r");
        assertThat(fake(a).get("status").asText()).isEqualTo("MATCHED");
        putFake(a, "{\"mode\":\"competitive\",\"players\":2}").andExpect(status().isConflict());

        deleteFake(b);
        mvc.perform(post("/admin/api/fake-searches/" + b + "/enabled").with(ADMIN).with(csrf())
                .contentType(MediaType.APPLICATION_JSON).content("{\"enabled\":true}")).andExpect(status().isNotFound());
        deleteFake(a);                                                     // deleting one that is in a forming match
        assertThat(poll("r").get("match").get("players").asInt()).isEqualTo(1);
    }

    // ------------------------------------------------------------------------------------------ the srcds side channel

    @Test
    void theSrcdsSideCanFetchTheRosterOfTheMatchOnItsServer() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, null);

        String roster = "/api/v1/servers/roster?address=192.168.1.150&port=27017";
        mvc.perform(get(roster)).andExpect(status().isUnauthorized());                          // API key required
        mvc.perform(get(roster).header(KEY, "test-api-key")).andExpect(status().isNotFound());  // no match on it yet

        search(REAL, COMPETITIVE, "\"de_dust2\"", "a");
        JsonNode body = body(mvc.perform(get(roster).header(KEY, "test-api-key")).andExpect(status().isOk()));
        assertThat(body.get("mode").asText()).isEqualTo("competitive");
        assertThat(body.get("status").asText()).isEqualTo("READY");
        assertThat(body.get("required_players").asInt()).isEqualTo(10);
        assertThat(body.get("real_players").asInt()).isEqualTo(1);
        assertThat(body.get("fake_players").asInt()).isEqualTo(9);
        assertThat(body.get("players").size()).isEqualTo(10);
        assertThat(body.get("players").get(9).get("account_id").asLong()).isEqualTo(0xFA4E0009L);

        mvc.perform(get("/api/v1/servers/roster?address=10.9.9.9&port=1").header(KEY, "test-api-key"))
                .andExpect(status().isNotFound());                                                // not registered
    }
}
