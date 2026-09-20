package dev.csgogc.mm;

import static org.assertj.core.api.Assertions.assertThat;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasterxml.jackson.databind.JsonNode;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;
import org.springframework.http.MediaType;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;
import org.springframework.test.web.servlet.ResultActions;

/**
 * What a Fake Players configuration does to real matchmaking (RESEARCH_FINDINGS.md #67), changed at runtime through the admin
 * API exactly like the panel does it. The two questions are separate: the gather window decides WHO is in the match (the real
 * players that searched within it), the profile of the mode decides HOW MANY virtual players it gets:
 * {@code effectiveFake = min(profile count, capacity - real)}; the capacity of the mode is never changed, never topped up to.
 * The Accept needs every REAL player; the virtual ones have no Accept.
 */
class FakeProfileConfigTest extends BackendTestBase {

    private static final Path DB_DIR = tempDir("mm-backend-profile-test");

    @DynamicPropertySource
    static void properties(DynamicPropertyRegistry registry) {
        registry.add("backend.database-path", () -> DB_DIR.resolve("mm.db").toString());
        registry.add("backend.fake-players.gather-window", () -> "PT10S");   // the window of the properties; tests also set it from the "panel"
    }

    private static final String DUST2 = "\"de_dust2\"";
    private static final String MIRAGE = "\"de_mirage\"";
    private static final String LAKE = "\"de_lake\"";

    private String state(JsonNode search) {
        return search.get("status").asText();
    }

    private String matchId(JsonNode search) {
        return search.get("match").get("match_id").asText();
    }

    private JsonNode assignment(String requestId) throws Exception {
        return poll(requestId).get("assignment");
    }

    private JsonNode roster(String host, int port) throws Exception {
        return body(mvc.perform(get("/api/v1/servers/roster?address=" + host + "&port=" + port).header(KEY, "test-api-key"))
                .andExpect(status().isOk()));
    }

    private ResultActions rosterCall(String host, int port) throws Exception {
        return mvc.perform(get("/api/v1/servers/roster?address=" + host + "&port=" + port).header(KEY, "test-api-key"));
    }

    private ResultActions rosterReady(String host, int port, String matchId) throws Exception {
        return mvc.perform(post("/api/v1/servers/roster/ready").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"address\":\"" + host + "\",\"port\":" + port + ",\"match_id\":\"" + matchId + "\"}"));
    }

    private List<Long> realAccounts(JsonNode roster) {
        List<Long> accounts = new ArrayList<>();
        for (JsonNode player : roster.get("players")) {
            if (!player.get("fake").asBoolean()) {
                accounts.add(player.get("account_id").asLong());
            }
        }
        return accounts;
    }

    /** the matches the search list of the admin API knows: the one of the request */
    private JsonNode matchOf(String matchId) throws Exception {
        for (JsonNode m : matches()) {
            if (m.get("id").asText().equals(matchId)) {
                return m;
            }
        }
        throw new AssertionError("match " + matchId);
    }

    /** mode -> {game type, capacity, map of the server} */
    private static Object[] mode(String key) {
        return switch (key) {
            case "competitive" -> new Object[] { COMPETITIVE, 10, "de_dust2" };
            case "wingman" -> new Object[] { WINGMAN, 4, "de_lake" };
            case "dangerzone" -> new Object[] { DANGERZONE, 16, "dz_blacksite" };
            default -> throw new IllegalArgumentException(key);
        };
    }

    // ------------------------------------------------------------------------------------------ 3 real + configured fake

    /** the scenario of the task: 3 real players (0 s, 3 s, 6 s), 7 fake configured in the panel = 10, one match, ordered accepts */
    @Test
    void threeRealPlayersAndSevenConfiguredFakePlayersAreOneMatchOfTen() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        long profile = addFake("competitive", 7, "\"de_dust2\",\"de_mirage\"");
        gatherWindow(10);

        JsonNode a = search(1, COMPETITIVE, DUST2, "a");
        assertThat(state(a)).isEqualTo("MATCHED");                              // 00 s: the match is gathering, no Match Found yet
        assertThat(a.has("assignment")).isFalse();
        pass(3);
        JsonNode b = search(2, COMPETITIVE, DUST2, "b");                        // 03 s
        pass(3);
        JsonNode c = search(3, COMPETITIVE, DUST2, "c");                        // 06 s
        String matchId = matchId(a);
        assertThat(matchId(b)).isEqualTo(matchId);                              // one match
        assertThat(matchId(c)).isEqualTo(matchId);
        assertThat(c.get("match").get("players").asInt()).isEqualTo(3);
        pass(9);                                                                // 15 s: 9 s after C - the window is not over
        assertThat(state(poll("a"))).isEqualTo("MATCHED");
        pass(2);                                                                // 17 s: it is

        for (String request : new String[] { "a", "b", "c" }) {
            JsonNode found = poll(request);
            assertThat(state(found)).isEqualTo("WAITING_ACCEPT");               // each one has its own assignment (= its 9107)
            assertThat(matchId(found)).isEqualTo(matchId);
            assertThat(found.get("assignment").get("real_players").asInt()).isEqualTo(3);
            assertThat(found.get("assignment").get("fake_players").asInt()).isEqualTo(7);   // exactly the configured number
            assertThat(found.get("assignment").get("players").size()).isEqualTo(10);        // 3 real + 7 fake = 10
        }
        assertThat(jdbc.sql("SELECT COUNT(*) FROM matchmaking_search WHERE assignment_seen_at IS NOT NULL").query(Integer.class).single())
                .isEqualTo(3);                                                  // and every one of them fetched it
        assertThat(realAccounts(roster("192.168.1.150", 27017))).containsExactly(1L, 2L, 3L);
        JsonNode used = fake(profile).get("matches").get(0);
        assertThat(used.get("configured").asInt()).isEqualTo(7);
        assertThat(used.get("fake_players").asInt()).isEqualTo(7);
        assertThat(used.get("capacity").asInt()).isEqualTo(10);

        assertThat(accepted(1, "a").get("match_accepted").asBoolean()).isFalse();   // one Accept: nobody connects
        assertThat(poll("b").get("match").get("accepted_players").asInt()).isEqualTo(1);
        assertThat(state(poll("a"))).isEqualTo("WAITING_ACCEPT");
        assertThat(accepted(2, "b").get("match_accepted").asBoolean()).isFalse();   // two Accepts: nobody connects
        for (String request : new String[] { "a", "b", "c" }) {
            assertThat(state(poll(request))).isEqualTo("WAITING_ACCEPT");
        }
        assertThat(matchOf(matchId).get("status").asText()).isEqualTo("ACCEPTING");
        assertThat(accepted(3, "c").get("match_accepted").asBoolean()).isTrue();    // three: everybody may connect
        for (String request : new String[] { "a", "b", "c" }) {
            assertThat(state(poll(request))).isEqualTo("READY_TO_CONNECT");
        }
    }

    /** 3 real + 2 configured = 5 players; the capacity of the mode (10) stays, the match is not topped up to it */
    @Test
    void threeRealAndTwoConfiguredFakePlayersAreAMatchOfFive() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 2, DUST2);
        gatherWindow(10);
        search(1, COMPETITIVE, DUST2, "a");
        search(2, COMPETITIVE, DUST2, "b");
        search(3, COMPETITIVE, DUST2, "c");
        pass(11);

        JsonNode a = poll("a");
        assertThat(state(a)).isEqualTo("WAITING_ACCEPT");
        assertThat(a.get("assignment").get("players").size()).isEqualTo(5);
        assertThat(a.get("assignment").get("real_players").asInt()).isEqualTo(3);
        assertThat(a.get("assignment").get("fake_players").asInt()).isEqualTo(2);
        assertThat(a.get("assignment").get("required_players").asInt()).isEqualTo(5);   // the roster that srcds arms
        assertThat(a.get("match").get("required_players").asInt()).isEqualTo(10);        // the capacity is not changed
        JsonNode roster = roster("192.168.1.150", 27017);
        assertThat(roster.get("players").size()).isEqualTo(5);
        assertThat(roster.get("required_players").asInt()).isEqualTo(5);
    }

    // ------------------------------------------------------------------------------------------ the count is read at runtime

    @Test
    void changingTheCountInThePanelChangesTheNextMatchOnly() throws Exception {
        addServer("10.0.0.1", 27017, "competitive", "de_dust2");
        addServer("10.0.0.2", 27017, "competitive", "de_dust2");
        addServer("10.0.0.3", 27017, "competitive", "de_dust2");
        long profile = addFake("competitive", 2, DUST2);

        JsonNode first = search(1, COMPETITIVE, DUST2, "a");                       // window 10 s from the properties, a lone player:
        assertThat(state(first)).isEqualTo("MATCHED");
        pass(11);
        assertThat(assignment("a").get("players").size()).isEqualTo(3);           // 1 real + 2 fake

        putFake(profile, "{\"mode\":\"competitive\",\"players\":6,\"maps\":[\"de_dust2\"]}").andExpect(status().isOk());
        search(2, COMPETITIVE, DUST2, "b");
        pass(11);
        assertThat(assignment("b").get("players").size()).isEqualTo(7);           // 1 real + 6 fake: the new setting
        assertThat(assignment("a").get("players").size()).isEqualTo(3);           // the match that was decided keeps its players

        putFake(profile, "{\"mode\":\"competitive\",\"players\":0,\"maps\":[\"de_dust2\"]}").andExpect(status().isOk());
        search(3, COMPETITIVE, DUST2, "c");
        pass(11);
        assertThat(assignment("c").get("players").size()).isEqualTo(1);           // 0 fake: no fake players at all
        assertThat(assignment("c").get("fake_players").asInt()).isZero();
    }

    /** count 0 (profile ON): the match starts with its real players only, after the window */
    @Test
    void zeroFakePlayersStartTheMatchWithTheRealPlayersOnly() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 0, DUST2);
        gatherWindow(10);
        search(1, COMPETITIVE, DUST2, "a");
        search(2, COMPETITIVE, DUST2, "b");
        search(3, COMPETITIVE, DUST2, "c");
        assertThat(state(poll("a"))).isEqualTo("MATCHED");
        pass(11);
        JsonNode a = poll("a");
        assertThat(state(a)).isEqualTo("WAITING_ACCEPT");
        assertThat(a.get("assignment").get("real_players").asInt()).isEqualTo(3);
        assertThat(a.get("assignment").get("fake_players").asInt()).isZero();
        assertThat(a.get("assignment").get("players").size()).isEqualTo(3);
        assertThat(a.get("match").get("required_players").asInt()).isEqualTo(10);
        assertThat(realAccounts(roster("192.168.1.150", 27017))).containsExactly(1L, 2L, 3L);

        accepted(1, "a");
        accepted(2, "b");
        assertThat(state(poll("a"))).isEqualTo("WAITING_ACCEPT");
        assertThat(accepted(3, "c").get("match_accepted").asBoolean()).isTrue();
    }

    /** configured 7 with 8 real of capacity 10: only 2 slots are left, 15 players are impossible; the capacity is not changed */
    @Test
    void theConfiguredCountIsCutToTheFreeSlots() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 7, DUST2);
        gatherWindow(10);
        for (int i = 1; i <= 8; i++) {
            search(i, COMPETITIVE, DUST2, "r" + i);
        }
        pass(11);
        JsonNode a = poll("r1");
        assertThat(state(a)).isEqualTo("WAITING_ACCEPT");
        assertThat(a.get("assignment").get("real_players").asInt()).isEqualTo(8);
        assertThat(a.get("assignment").get("fake_players").asInt()).isEqualTo(2);        // min(7, 10 - 8)
        assertThat(a.get("assignment").get("players").size()).isEqualTo(10);
        assertThat(a.get("match").get("required_players").asInt()).isEqualTo(10);
        assertThat(fake(fakeIds().get(0)).get("matches").get(0).get("configured").asInt()).isEqualTo(7);
    }

    private List<Long> fakeIds() throws Exception {
        List<Long> ids = new ArrayList<>();
        for (JsonNode f : fakeOverview().get("fake_searches")) {
            ids.add(f.get("id").asLong());
        }
        return ids;
    }

    /** the real players fill the match themselves: no window to wait for, no virtual player is added */
    @Test
    void aMatchOfRealPlayersOnlyGetsNoFakePlayers() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 2, LAKE);
        gatherWindow(10);
        for (int i = 1; i <= 4; i++) {
            search(i, WINGMAN, LAKE, "p" + i);
        }
        JsonNode a = poll("p1");
        assertThat(state(a)).isEqualTo("WAITING_ACCEPT");
        assertThat(a.get("assignment").get("real_players").asInt()).isEqualTo(4);
        assertThat(a.get("assignment").get("fake_players").asInt()).isZero();
    }

    // ------------------------------------------------------------------------------------------ switched off

    @Test
    void aDisabledProfileMeansTheRealPlayersWaitWithoutFakePlayers() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        long profile = addFake("wingman", 3, LAKE, false, 0, null);
        search(1, WINGMAN, LAKE, "a");
        pass(60);
        assertThat(state(poll("a"))).isEqualTo("MATCHED");                      // ordinary matchmaking: waiting for real players
        assertThat(poll("a").get("match").get("fake_players").asInt()).isZero();
        assertThat(poll("a").has("assignment")).isFalse();

        fakeEnabled(profile, true);                                             // ON: applies to the match that is waiting
        assertThat(state(poll("a"))).isEqualTo("WAITING_ACCEPT");
        assertThat(poll("a").get("assignment").get("fake_players").asInt()).isEqualTo(3);
    }

    @Test
    void theMasterSwitchOffMeansOrdinaryMatchmaking() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 3, LAKE);
        fakeMaster(false);
        search(1, WINGMAN, LAKE, "a");
        search(2, WINGMAN, LAKE, "b");
        pass(120);
        assertThat(state(poll("a"))).isEqualTo("MATCHED");
        assertThat(poll("a").get("match").get("players").asInt()).isEqualTo(2);         // two real players, nobody else
        assertThat(poll("a").get("match").get("fake_players").asInt()).isZero();
        assertThat(fakeOverview().get("master").asBoolean()).isFalse();

        fakeMaster(true);                                                       // ON again: the profile is applied
        JsonNode a = poll("a");
        assertThat(state(a)).isEqualTo("WAITING_ACCEPT");
        assertThat(a.get("assignment").get("real_players").asInt()).isEqualTo(2);
        assertThat(a.get("assignment").get("fake_players").asInt()).isEqualTo(2);       // min(3, 4 - 2)
    }

    @Test
    void aProfileOfAnotherModeIsNotUsed() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("wingman", 3, null);
        addFake("dangerzone", 15, null);
        search(1, COMPETITIVE, DUST2, "a");
        pass(60);
        assertThat(state(poll("a"))).isEqualTo("MATCHED");
        assertThat(poll("a").get("match").get("fake_players").asInt()).isZero();
    }

    // ------------------------------------------------------------------------------------------ maps and servers

    @Test
    void theProfileMapsChooseTheServer() throws Exception {
        addServer("10.0.0.1", 27017, "competitive", "de_inferno");
        long profile = addFake("competitive", 7, "\"de_dust2\",\"de_mirage\"");
        search(1, COMPETITIVE, "\"de_dust2\",\"de_mirage\",\"de_inferno\",\"de_nuke\"", "a");
        pass(60);
        // the only server runs a map that the profile does not list: the match must not be played there, it keeps waiting
        assertThat(state(poll("a"))).isEqualTo("MATCHED");
        assertThat(poll("a").get("match").get("fake_players").asInt()).isZero();

        long mirage = addServer("10.0.0.2", 27017, "competitive", "de_mirage");   // a server of the profile's maps appears
        JsonNode a = poll("a");
        assertThat(state(a)).isEqualTo("WAITING_ACCEPT");
        assertThat(a.get("assignment").get("map").asText()).isEqualTo("de_mirage");
        assertThat(a.get("assignment").get("server_address").asText()).isEqualTo("10.0.0.2");
        assertThat(a.get("assignment").get("fake_players").asInt()).isEqualTo(7);
        assertThat(server(mirage).get("state").asText()).isEqualTo("RESERVED");
        assertThat(fake(profile).get("matches").get(0).get("map").asText()).isEqualTo("de_mirage");
    }

    @Test
    void theRealPlayersMapsAndTheProfileMapsHaveToOverlap() throws Exception {
        addServer("10.0.0.1", 27017, "competitive", "de_dust2");
        addFake("competitive", 7, "\"de_inferno\"");                            // ... the player wants dust2 only
        search(1, COMPETITIVE, DUST2, "a");
        pass(60);
        assertThat(state(poll("a"))).isEqualTo("MATCHED");
        assertThat(poll("a").get("match").get("fake_players").asInt()).isZero();
    }

    @Test
    void aProfileWithoutMapsAcceptsAnyMapOfTheRealPlayers() throws Exception {
        addServer("10.0.0.1", 27017, "competitive", "de_mirage");
        addFake("competitive", 4, null);
        search(1, COMPETITIVE, "\"de_dust2\",\"de_mirage\"", "a");
        pass(11);
        JsonNode a = poll("a");
        assertThat(state(a)).isEqualTo("WAITING_ACCEPT");
        assertThat(a.get("assignment").get("map").asText()).isEqualTo("de_mirage");
        assertThat(a.get("assignment").get("players").size()).isEqualTo(5);
    }

    @Test
    void aProfileBoundToAServerOnlyUsesThatServer() throws Exception {
        addServer("10.0.0.1", 27017, "competitive", "de_dust2");
        long second = addServer("10.0.0.2", 27017, "competitive", "de_dust2");
        addFake("competitive", 5, DUST2, true, 0, second);
        search(1, COMPETITIVE, DUST2, "a");
        pass(11);
        JsonNode a = poll("a");
        assertThat(state(a)).isEqualTo("WAITING_ACCEPT");
        assertThat(a.get("assignment").get("server_address").asText()).isEqualTo("10.0.0.2");   // not the free first one

        search(2, COMPETITIVE, DUST2, "b");                                    // its server is taken: the other one is free but not its
        pass(11);
        assertThat(state(poll("b"))).isEqualTo("MATCHED");
        assertThat(poll("b").has("assignment")).isFalse();
    }

    // ------------------------------------------------------------------------------------------ priority, several modes

    @Test
    void theProfileWithTheHigherPriorityWins() throws Exception {
        addServer("10.0.0.1", 27017, "competitive", "de_dust2");
        addServer("10.0.0.2", 27017, "competitive", "de_dust2");
        addServer("10.0.0.3", 27017, "competitive", "de_dust2");
        long low = addFake("competitive", 3, DUST2, true, 1, null);
        long high = addFake("competitive", 7, DUST2, true, 5, null);

        search(1, COMPETITIVE, DUST2, "a");
        pass(11);
        assertThat(assignment("a").get("fake_players").asInt()).isEqualTo(7);          // the profiles are not added up: the best one
        assertThat(fake(high).get("matches").size()).isEqualTo(1);
        assertThat(fake(low).get("matches").size()).isZero();

        fakeEnabled(high, false);
        search(2, COMPETITIVE, DUST2, "b");
        pass(11);
        assertThat(assignment("b").get("fake_players").asInt()).isEqualTo(3);          // the next one

        fakeEnabled(high, true);
        long same = addFake("competitive", 4, DUST2, true, 5, null);                   // the same priority: the older profile
        search(3, COMPETITIVE, DUST2, "c");
        pass(11);
        assertThat(assignment("c").get("fake_players").asInt()).isEqualTo(7);
        assertThat(fake(same).get("matches").size()).isZero();
    }

    @Test
    void everyModeHasItsOwnCount() throws Exception {
        addServer("10.0.0.1", 27017, "competitive", "de_dust2");
        addServer("10.0.0.1", 27018, "wingman", "de_lake");
        addServer("10.0.0.1", 27019, "dangerzone", "dz_blacksite");
        addFake("competitive", 7, DUST2);
        addFake("wingman", 2, LAKE);
        addFake("dangerzone", 13, "\"dz_blacksite\"");
        search(1, COMPETITIVE, DUST2, "c");
        search(2, WINGMAN, LAKE, "w");
        search(3, DANGERZONE, "\"dz_blacksite\"", "d");
        pass(11);
        assertThat(assignment("c").get("players").size()).isEqualTo(8);                // 1 + 7
        assertThat(assignment("w").get("players").size()).isEqualTo(3);                // 1 + 2
        assertThat(assignment("d").get("players").size()).isEqualTo(14);               // 1 + 13
    }

    // ------------------------------------------------------------------------------------------ every mode, 1..3 real players

    /**
     * The whole lifecycle for every Accept mode, 1 / 2 / 3 real players and several counts: one match, exactly
     * min(count, capacity - real) virtual players, every real player has its own assignment, and only after the LAST real Accept
     * anybody may connect.
     */
    @ParameterizedTest(name = "{0}: {1} real + {2} configured fake")
    @CsvSource({
            "competitive,1,7", "competitive,2,7", "competitive,3,7", "competitive,3,2", "competitive,3,0", "competitive,3,9",
            "wingman,1,2", "wingman,2,2", "wingman,3,2", "wingman,2,0", "wingman,3,3",
            "dangerzone,1,13", "dangerzone,2,13", "dangerzone,3,13", "dangerzone,3,5", "dangerzone,3,15"
    })
    void everyRealPlayerGetsMatchFoundAndOnlyTheLastAcceptLetsThemConnect(String modeKey, int real, int configured) throws Exception {
        Object[] mode = mode(modeKey);
        long gameType = (Long) mode[0];
        int capacity = (Integer) mode[1];
        String map = "\"" + mode[2] + "\"";
        int fake = Math.min(configured, capacity - real);

        addServer("192.168.1.150", 27017, modeKey, (String) mode[2]);
        addFake(modeKey, configured, map);
        gatherWindow(10);

        for (int i = 0; i < real; i++) {
            JsonNode search = search(100 + i, gameType, map, "r" + i);
            assertThat(state(search)).isEqualTo("MATCHED");                    // gathering: nobody has a Match Found yet
            assertThat(search.has("assignment")).isFalse();
            if (i < real - 1) {
                pass(2);
            }
        }
        pass(11);

        String matchId = null;
        for (int i = 0; i < real; i++) {
            JsonNode found = poll("r" + i);
            assertThat(state(found)).as("real player %d has its Match Found", i).isEqualTo("WAITING_ACCEPT");
            matchId = matchId == null ? matchId(found) : matchId;
            assertThat(matchId(found)).isEqualTo(matchId);                     // ONE backend match
            JsonNode assignment = found.get("assignment");
            assertThat(assignment.get("real_players").asInt()).isEqualTo(real);
            assertThat(assignment.get("fake_players").asInt()).isEqualTo(fake);     // exactly the effective count
            assertThat(assignment.get("players").size()).isEqualTo(real + fake);
            assertThat(found.get("match").get("required_players").asInt()).isEqualTo(capacity);   // capacity untouched
        }
        assertThat(jdbc.sql("SELECT COUNT(*) FROM matchmaking_search WHERE assignment_seen_at IS NOT NULL").query(Integer.class).single())
                .isEqualTo(real);                                              // every real player fetched its own assignment
        JsonNode roster = roster("192.168.1.150", 27017);
        assertThat(roster.get("players").size()).isEqualTo(real + fake);
        List<Long> expected = new ArrayList<>();
        for (int i = 0; i < real; i++) {
            expected.add(100L + i);
        }
        assertThat(realAccounts(roster)).containsExactlyElementsOf(expected);   // the roster srcds arms has every real player

        for (int i = 0; i < real; i++) {
            JsonNode answer = accepted(100 + i, "r" + i);
            boolean last = i == real - 1;
            assertThat(answer.get("accepted").asBoolean()).isTrue();
            assertThat(answer.get("match_accepted").asBoolean()).as("accept %d of %d", i + 1, real).isEqualTo(last);
            for (int k = 0; k < real; k++) {
                assertThat(state(poll("r" + k))).as("player %d after %d accept(s)", k, i + 1)
                        .isEqualTo(last ? "READY_TO_CONNECT" : "WAITING_ACCEPT");   // nobody connects before the last one
            }
            assertThat(poll("r0").get("match").get("accepted_players").asInt()).isEqualTo(i + 1);
            assertThat(poll("r0").get("match").get("real_players").asInt()).isEqualTo(real);
        }
    }

    // ------------------------------------------------------------------------------------------ first / second matchmaking

    /**
     * A srcds that reads its roster from the backend (a fresh one, then the same one after a release): the configured fake
     * players do not hide the hold - the real players get their Match Found only after the srcds armed the roster (both times),
     * and the second matchmaking after the release is the same as the first.
     */
    @Test
    void theFirstAndTheSecondMatchmakingOnTheSameServerBehaveTheSame() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 7, DUST2);
        gatherWindow(10);
        rosterCall("192.168.1.150", 27017).andExpect(status().isNotFound());        // a fresh srcds asks for its roster: nothing yet

        for (int i = 1; i <= 3; i++) {
            search(i, COMPETITIVE, DUST2, "r" + i);
        }
        pass(5);
        rosterCall("192.168.1.150", 27017).andExpect(status().isNotFound());        // srcds asks once a second: still no match
        pass(6);                                                                // the window is over: 3 real + 7 fake, server reserved
        for (int i = 1; i <= 3; i++) {
            assertThat(state(poll("r" + i))).isEqualTo("MATCHED");              // held: the server has not armed the roster yet
            assertThat(poll("r" + i).get("match").get("awaiting_server").asBoolean()).isTrue();
        }
        JsonNode first = roster("192.168.1.150", 27017);
        assertThat(first.get("players").size()).isEqualTo(10);
        assertThat(realAccounts(first)).containsExactly(1L, 2L, 3L);
        rosterReady("192.168.1.150", 27017, first.get("match_id").asText()).andExpect(status().isOk());
        for (int i = 1; i <= 3; i++) {
            assertThat(state(poll("r" + i))).isEqualTo("WAITING_ACCEPT");
        }

        pass(26);                                                               // nobody accepts: the backend cancels the match
        rosterCall("192.168.1.150", 27017).andExpect(status().isNotFound());        // srcds sees no match: it drops the reservation
        for (int i = 1; i <= 3; i++) {
            assertThat(state(poll("r" + i))).isEqualTo("MATCHED");              // the same searches, gathering again
        }
        pass(8);
        rosterCall("192.168.1.150", 27017).andExpect(status().isNotFound());        // still cooling down
        pass(8);                                                                // the window and the release cooldown are over

        JsonNode second = roster("192.168.1.150", 27017);
        assertThat(second.get("match_id").asText()).isNotEqualTo(first.get("match_id").asText());
        assertThat(second.get("players").size()).isEqualTo(10);                 // the same configuration, the same roster
        assertThat(realAccounts(second)).containsExactly(1L, 2L, 3L);
        assertThat(state(poll("r1"))).isEqualTo("MATCHED");                     // held again until srcds armed it
        rosterReady("192.168.1.150", 27017, second.get("match_id").asText()).andExpect(status().isOk());
        for (int i = 1; i <= 3; i++) {
            assertThat(state(poll("r" + i))).isEqualTo("WAITING_ACCEPT");
        }
        accepted(1, "r1");
        accepted(2, "r2");
        assertThat(state(poll("r1"))).isEqualTo("WAITING_ACCEPT");
        assertThat(accepted(3, "r3").get("match_accepted").asBoolean()).isTrue();
    }

    /** a real player who leaves the full match that waits for a server: it gathers again and the virtual players are decided afresh */
    @Test
    void aPlayerWhoLeavesBeforeTheServerIsFoundMakesTheVirtualPlayersBeDecidedAgain() throws Exception {
        long only = addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 3, LAKE);
        assertThat(state(search(1, WINGMAN, LAKE, "a"))).isEqualTo("MATCHED");
        pass(11);
        assertThat(state(poll("a"))).isEqualTo("WAITING_ACCEPT");                   // a holds the only server

        search(2, WINGMAN, LAKE, "c");
        search(3, WINGMAN, LAKE, "d");
        pass(11);                                                               // c + d + 2 fake = 4: full, but no free server
        assertThat(poll("c").get("match").get("status").asText()).isEqualTo("FULL");
        assertThat(poll("c").get("match").get("fake_players").asInt()).isEqualTo(2);

        cancel(3, "d");                                                         // d left: the match gathers again and, the window being
        JsonNode c = poll("c");                                                 // over, decides its virtual players afresh: c + 3 = 4
        assertThat(c.get("match").get("status").asText()).isEqualTo("FULL");
        assertThat(c.get("match").get("fake_players").asInt()).isEqualTo(3);        // not the 2 of the match d was in
        assertThat(c.get("match").get("players").asInt()).isEqualTo(4);
        assertThat(server(only).get("state").asText()).isEqualTo("RESERVED");
    }
}
