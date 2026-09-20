package dev.csgogc.mm;

import static org.assertj.core.api.Assertions.assertThat;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasterxml.jackson.databind.JsonNode;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;

/**
 * Real players who search one after the other end up in ONE match (RESEARCH_FINDINGS.md #66). The live symptom: with a fake
 * search of 9 the first real player got a complete Competitive match at once (1 + 9 = 10, server reserved, Match Found), and
 * everybody who pressed Play a few seconds later found it full and waited in a match of their own for a server that was never
 * free: Match Found only for one player, whose Accept then sent him on alone.
 * <p>
 * Now the fake players fill only what is still missing, and only after {@code backend.fake-players.gather-window} passed
 * since the last real player joined. These tests use a window of 10 s on the controllable clock.
 */
class GatherWindowTest extends BackendTestBase {

    private static final Path DB_DIR = tempDir("mm-backend-gather-test");

    @DynamicPropertySource
    static void properties(DynamicPropertyRegistry registry) {
        registry.add("backend.database-path", () -> DB_DIR.resolve("mm.db").toString());
        registry.add("backend.fake-players.gather-window", () -> "PT10S");
    }

    private static final String DUST2 = "\"de_dust2\"";
    private static final String LAKE = "\"de_lake\"";
    private static final String BLACKSITE = "\"dz_blacksite\"";

    private String state(JsonNode search) {
        return search.get("status").asText();
    }

    private String matchId(JsonNode search) {
        return search.get("match").get("match_id").asText();
    }

    /** the roster a srcds asks for (GET /servers/roster): the real accounts and how many fake ones */
    private JsonNode roster(String host, int port) throws Exception {
        return body(mvc.perform(get("/api/v1/servers/roster?address=" + host + "&port=" + port).header(KEY, "test-api-key"))
                .andExpect(status().isOk()));
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

    private void assertNoBodyConnects(String matchId, String... requestIds) throws Exception {
        for (JsonNode m : matches()) {
            if (m.get("id").asText().equals(matchId)) {
                assertThat(m.get("status").asText()).isNotEqualTo("ACCEPTED");
            }
        }
        for (String requestId : requestIds) {
            assertThat(state(poll(requestId))).isNotEqualTo("READY_TO_CONNECT");
        }
    }

    // ------------------------------------------------------------------------------------------ 2 real + fake

    /** 1. 2 real + fake, Competitive: the second player joins the match of the first one and gets its own Match Found */
    @Test
    void theSecondRealPlayerJoinsTheMatchOfTheFirstOne() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);

        JsonNode a = search(1, COMPETITIVE, DUST2, "a");
        assertThat(state(a)).isEqualTo("MATCHED");                    // waiting: the fake players are not there yet
        assertThat(a.get("match").get("players").asInt()).isEqualTo(1);
        assertThat(a.get("assignment")).isNull();                      // no Match Found for one player of ten

        pass(4);
        JsonNode b = search(2, COMPETITIVE, DUST2, "b");
        assertThat(matchId(b)).isEqualTo(matchId(a));                   // the SAME match
        assertThat(state(b)).isEqualTo("MATCHED");
        assertThat(b.get("match").get("players").asInt()).isEqualTo(2);

        pass(8);                                                        // 12 s after A, but only 8 after B: still gathering
        assertThat(state(poll("a"))).isEqualTo("MATCHED");
        pass(3);                                                        // the window since the LAST real player is over

        JsonNode a2 = poll("a");
        JsonNode b2 = poll("b");
        assertThat(state(a2)).isEqualTo("WAITING_ACCEPT");             // both get the Match Found
        assertThat(state(b2)).isEqualTo("WAITING_ACCEPT");
        assertThat(matchId(b2)).isEqualTo(matchId(a2));
        assertThat(a2.get("assignment").get("real_players").asInt()).isEqualTo(2);
        assertThat(a2.get("assignment").get("fake_players").asInt()).isEqualTo(8);   // the pool of 9 gave the 8 that were missing
        assertThat(a2.get("assignment").get("players").size()).isEqualTo(10);
        assertThat(b2.get("assignment").get("server_port").asInt()).isEqualTo(27017);

        // what srcds arms: both real accounts are in the roster, so `awaiting` counts BOTH of them
        JsonNode roster = roster("192.168.1.150", 27017);
        assertThat(realAccounts(roster)).containsExactly(1L, 2L);
        assertThat(roster.get("players").size()).isEqualTo(10);
        assertThat(roster.get("real_players").asInt()).isEqualTo(2);
        assertThat(roster.get("fake_players").asInt()).isEqualTo(8);

        // A accepts first: nobody connects
        JsonNode first = accepted(1, "a");
        assertThat(first.get("accepted").asBoolean()).isTrue();
        assertThat(first.get("match_accepted").asBoolean()).isFalse();
        assertThat(poll("a").get("match").get("accepted_players").asInt()).isEqualTo(1);
        assertThat(poll("a").get("match").get("real_players").asInt()).isEqualTo(2);
        assertNoBodyConnects(matchId(a2), "a", "b");

        // B accepts: now everybody does
        JsonNode last = accepted(2, "b");
        assertThat(last.get("match_accepted").asBoolean()).isTrue();
        assertThat(state(poll("a"))).isEqualTo("READY_TO_CONNECT");
        assertThat(state(poll("b"))).isEqualTo("READY_TO_CONNECT");
    }

    /** 2. 3 real + fake, Competitive */
    @Test
    void threeRealPlayersAndFakePlayersAreOneMatch() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);

        JsonNode a = search(10, COMPETITIVE, DUST2, "a");
        pass(3);
        JsonNode b = search(11, COMPETITIVE, DUST2, "b");
        pass(3);
        JsonNode c = search(12, COMPETITIVE, DUST2, "c");
        assertThat(matchId(b)).isEqualTo(matchId(a));
        assertThat(matchId(c)).isEqualTo(matchId(a));
        assertThat(c.get("match").get("players").asInt()).isEqualTo(3);
        pass(11);

        for (String request : new String[] { "a", "b", "c" }) {
            assertThat(state(poll(request))).isEqualTo("WAITING_ACCEPT");
        }
        JsonNode roster = roster("192.168.1.150", 27017);
        assertThat(realAccounts(roster)).containsExactly(10L, 11L, 12L);
        assertThat(roster.get("fake_players").asInt()).isEqualTo(7);

        assertThat(accepted(10, "a").get("match_accepted").asBoolean()).isFalse();
        assertThat(accepted(11, "b").get("match_accepted").asBoolean()).isFalse();
        assertNoBodyConnects(matchId(a), "a", "b", "c");                // 2 of 3 accepted: nobody connects
        assertThat(poll("c").get("match").get("accepted_players").asInt()).isEqualTo(2);
        assertThat(accepted(12, "c").get("match_accepted").asBoolean()).isTrue();
        for (String request : new String[] { "a", "b", "c" }) {
            assertThat(state(poll(request))).isEqualTo("READY_TO_CONNECT");
        }
    }

    /** 3. 2 real + fake, Wingman */
    @Test
    void twoRealPlayersAndFakePlayersInWingman() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 3, LAKE);

        JsonNode a = search(20, WINGMAN, LAKE, "a");
        pass(5);
        JsonNode b = search(21, WINGMAN, LAKE, "b");
        assertThat(matchId(b)).isEqualTo(matchId(a));
        pass(10);

        JsonNode a2 = poll("a");
        assertThat(state(a2)).isEqualTo("WAITING_ACCEPT");
        assertThat(state(poll("b"))).isEqualTo("WAITING_ACCEPT");
        assertThat(a2.get("assignment").get("real_players").asInt()).isEqualTo(2);
        assertThat(a2.get("assignment").get("fake_players").asInt()).isEqualTo(2);    // 4 = 2 real + 2 of the pool of 3
        assertThat(realAccounts(roster("192.168.1.150", 27018))).containsExactly(20L, 21L);

        assertThat(accepted(21, "b").get("match_accepted").asBoolean()).isFalse();   // B is quicker this time
        assertNoBodyConnects(matchId(a2), "a", "b");
        assertThat(accepted(20, "a").get("match_accepted").asBoolean()).isTrue();
    }

    /** 4. 2+ real + fake, Danger Zone */
    @Test
    void twoRealPlayersAndFakePlayersInDangerZone() throws Exception {
        addServer("192.168.1.150", 27019, "dangerzone", "dz_blacksite");
        addFake("dangerzone", 15, BLACKSITE);

        JsonNode a = search(30, DANGERZONE, BLACKSITE, "a");
        pass(2);
        search(31, DANGERZONE, BLACKSITE, "b");
        pass(2);
        search(32, DANGERZONE, BLACKSITE, "c");
        pass(11);

        JsonNode a2 = poll("a");
        assertThat(state(a2)).isEqualTo("WAITING_ACCEPT");
        assertThat(a2.get("assignment").get("real_players").asInt()).isEqualTo(3);
        assertThat(a2.get("assignment").get("fake_players").asInt()).isEqualTo(13);
        assertThat(a2.get("assignment").get("players").size()).isEqualTo(16);
        assertThat(realAccounts(roster("192.168.1.150", 27019))).containsExactly(30L, 31L, 32L);
        assertThat(state(poll("b"))).isEqualTo("WAITING_ACCEPT");
        assertThat(state(poll("c"))).isEqualTo("WAITING_ACCEPT");
    }

    // ------------------------------------------------------------------------------------------ the window itself

    /** every real player that joins restarts the window: the fake players wait for the LAST one */
    @Test
    void everyRealPlayerRestartsTheWindow() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 3, LAKE);

        search(40, WINGMAN, LAKE, "a");
        pass(8);
        search(41, WINGMAN, LAKE, "b");                                  // 8 s after A: A's window would end in 2 s
        pass(8);
        assertThat(state(poll("a"))).isEqualTo("MATCHED");             // B moved it: 10 s after B
        pass(3);
        assertThat(state(poll("a"))).isEqualTo("WAITING_ACCEPT");
        assertThat(state(poll("b"))).isEqualTo("WAITING_ACCEPT");
    }

    /** real players alone fill a match: nobody waits for the window, no fake player is used */
    @Test
    void enoughRealPlayersNeedNoFakePlayers() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 3, LAKE);

        for (int i = 0; i < 4; i++) {
            search(50 + i, WINGMAN, LAKE, "p" + i);
        }
        assertThat(state(poll("p0"))).isEqualTo("WAITING_ACCEPT");     // 4 real players, no window
        assertThat(poll("p0").get("assignment").get("fake_players").asInt()).isEqualTo(0);
    }

    /** the fake players never make a match on their own and a lone real player is served after the window (single-player test setup) */
    @Test
    void aLoneRealPlayerIsServedAfterTheWindow() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);

        assertThat(state(search(60, COMPETITIVE, DUST2, "a"))).isEqualTo("MATCHED");
        pass(9);
        assertThat(state(poll("a"))).isEqualTo("MATCHED");
        pass(2);
        JsonNode a = poll("a");
        assertThat(state(a)).isEqualTo("WAITING_ACCEPT");
        assertThat(a.get("assignment").get("players").size()).isEqualTo(10);
        assertThat(a.get("assignment").get("real_players").asInt()).isEqualTo(1);
    }

    // ------------------------------------------------------------------------------------------ first and second matchmaking

    /**
     * 5./6. a player who came after the fill is not lost: while the first match waits for its Accept the late player waits in
     * a match of his own, and when the first one is over (nobody accepted / everybody left) the rest of the searches are
     * gathered together with the returned fake players into ONE match - the "second matchmaking" that must work as well as the first.
     */
    @Test
    void aLatePlayerAndTheReturnedPlayersMeetInOneMatchAfterTheFirstIsCancelled() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);

        JsonNode a = search(70, COMPETITIVE, DUST2, "a");
        pass(11);                                                        // A alone: the pool fills it (1 + 9)
        assertThat(state(poll("a"))).isEqualTo("WAITING_ACCEPT");
        String first = matchId(poll("a"));
        JsonNode b = search(71, COMPETITIVE, DUST2, "b");                // too late for the first match
        assertThat(state(b)).isEqualTo("MATCHED");                    // it gathers in a match of its own (no free server for it)
        assertThat(matchId(b)).isNotEqualTo(first);

        pass(26);                                                        // the Accept deadline (25 s) passes: the match is cancelled
        assertThat(state(poll("a"))).isIn("SEARCHING", "MATCHED");
        JsonNode aAgain = poll("a");
        JsonNode bAgain = poll("b");
        assertThat(matchId(aAgain)).isEqualTo(matchId(bAgain));          // together in ONE new match now
        assertThat(matchId(aAgain)).isNotEqualTo(first);

        pass(16);                                                        // the release cooldown of the server + the window
        assertThat(state(poll("a"))).isEqualTo("WAITING_ACCEPT");
        assertThat(state(poll("b"))).isEqualTo("WAITING_ACCEPT");
        JsonNode roster = roster("192.168.1.150", 27017);
        assertThat(realAccounts(roster)).containsExactly(70L, 71L);
        assertThat(roster.get("fake_players").asInt()).isEqualTo(8);
    }

    /** 7. the log line the operator reads: who of the match got Match Found and who accepted (progress of the search) */
    @Test
    void theProgressOfTheMatchSaysHowManyAccepted() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 3, LAKE);
        search(80, WINGMAN, LAKE, "a");
        search(81, WINGMAN, LAKE, "b");
        pass(11);

        JsonNode progress = poll("a").get("match");
        assertThat(progress.get("real_players").asInt()).isEqualTo(2);
        assertThat(progress.get("accepted_players").asInt()).isEqualTo(0);
        accepted(80, "a");
        assertThat(poll("b").get("match").get("accepted_players").asInt()).isEqualTo(1);
        accepted(81, "b");
        assertThat(poll("b").get("match").get("accepted_players").asInt()).isEqualTo(2);
    }
}
