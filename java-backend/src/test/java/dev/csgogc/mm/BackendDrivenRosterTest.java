package dev.csgogc.mm;

import static org.assertj.core.api.Assertions.assertThat;
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
import org.springframework.test.web.servlet.ResultActions;

/**
 * The backend is the source of truth for the roster of a test match (RESEARCH_FINDINGS.md #55):
 * {@code required_players = real + fake}, the assignment lists exactly those participants, and a game server that reads
 * its roster from here confirms it armed it before the players get the server.
 */
class BackendDrivenRosterTest extends BackendTestBase {

    private static final Path DB_DIR = tempDir("mm-backend-roster-test");

    @DynamicPropertySource
    static void database(DynamicPropertyRegistry registry) {
        registry.add("backend.database-path", () -> DB_DIR.resolve("mm.db").toString());
    }

    private static final long REAL = 1050166997L;

    // ------------------------------------------------------------------------------------------ real + fake = required

    /** the example of the task: fake search 9 x Competitive [de_dust2], real Competitive [de_dust2] -> ONE match of 10 */
    @Test
    void oneRealAndNineFakePlayersAreOneCompetitiveMatchOfTen() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, "\"de_dust2\"");

        JsonNode real = search(REAL, COMPETITIVE, "\"de_dust2\"", "r");
        assertThat(matches().size()).isEqualTo(1);
        assertRoster(real.get("assignment"), 10, 1, 9);
        assertThat(matches().get(0).get("players").asInt()).isEqualTo(10);
        assertThat(matches().get(0).get("fake_players").asInt()).isEqualTo(9);
        assertThat(matches().get(0).get("account_ids").size()).isEqualTo(1);
    }

    @Test
    void oneRealAndThreeFakePlayersAreOneWingmanMatchOfFour() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 3, "\"de_lake\"");
        assertRoster(search(REAL, WINGMAN, "\"de_lake\"", "r").get("assignment"), 4, 1, 3);
        assertThat(matches().size()).isEqualTo(1);
    }

    @Test
    void oneRealAndFifteenFakePlayersAreOneDangerZoneMatchOfSixteen() throws Exception {
        addServer("192.168.1.150", 27019, "dangerzone", "dz_blacksite");
        addFake("dangerzone", 15, "\"dz_blacksite\"");
        assertRoster(search(REAL, DANGERZONE, "\"dz_blacksite\"", "r").get("assignment"), 16, 1, 15);
        assertThat(matches().size()).isEqualTo(1);
    }

    /** required_players is the size of the match, it does not count fake players "on top" and is not lowered for them */
    private void assertRoster(JsonNode assignment, int required, int real, int fake) {
        assertThat(assignment.get("required_players").asInt()).isEqualTo(required);
        assertThat(assignment.get("real_players").asInt()).isEqualTo(real);
        assertThat(assignment.get("fake_players").asInt()).isEqualTo(fake);
        assertThat(real + fake).isEqualTo(required);
        JsonNode players = assignment.get("players");
        assertThat(players.size()).isEqualTo(required);
        assertThat(players.get(0).get("account_id").asLong()).isEqualTo(REAL);
        assertThat(players.get(0).get("fake").asBoolean()).isFalse();
        for (int i = 1; i <= fake; i++) {
            assertThat(players.get(i).get("account_id").asLong()).isEqualTo(0xFA4E0000L + i);
            assertThat(players.get(i).get("fake").asBoolean()).isTrue();
        }
    }

    // ------------------------------------------------------------------------------------------ what does not match

    @Test
    void aFakeSearchOfAnIncompatibleModeDoesNotMatch() throws Exception {
        addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        long wingmanFake = addFake("wingman", 3, "\"de_dust2\"");
        long dangerFake = addFake("dangerzone", 15, null);
        JsonNode real = search(REAL, COMPETITIVE, "\"de_dust2\"", "r");
        assertThat(real.get("status").asText()).isEqualTo("MATCHED");
        assertThat(real.get("match").get("players").asInt()).isEqualTo(1);
        assertThat(fake(wingmanFake).get("matches").size()).isZero();
        assertThat(fake(dangerFake).get("matches").size()).isZero();
    }

    @Test
    void aFakeSearchWithAnIncompatibleMapDoesNotMatch() throws Exception {
        addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        long fakeId = addFake("competitive", 9, "\"de_mirage\",\"de_inferno\"");
        JsonNode real = search(REAL, COMPETITIVE, "\"de_dust2\"", "r");
        assertThat(real.get("status").asText()).isEqualTo("MATCHED");
        assertThat(real.get("match").get("players").asInt()).isEqualTo(1);
        assertThat(fake(fakeId).get("matches").size()).isZero();
    }

    /** the profiles are not added up to the capacity: the one with the highest priority (the oldest of them on a tie) is the match's */
    @Test
    void severalProfilesOfTheSamePriorityTheOldestWins() throws Exception {
        addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        addFake("competitive", 4, "\"de_dust2\"");
        addFake("competitive", 3, null);
        addFake("competitive", 2, "\"de_dust2\",\"de_mirage\"");
        JsonNode real = search(REAL, COMPETITIVE, "\"de_dust2\"", "r");
        assertRoster(real.get("assignment"), 5, 1, 4);          // 1 real + the 4 of the oldest profile, not topped up to 10
        assertThat(real.get("match").get("required_players").asInt()).isEqualTo(10);
        assertThat(matches().size()).isEqualTo(1);
    }

    @Test
    void aDisabledFakeSearchDoesNotParticipate() throws Exception {
        addServer("10.0.0.5", 27018, "wingman", "de_lake");
        long off = addFake("wingman", 3, "\"de_lake\"", false);
        JsonNode real = search(REAL, WINGMAN, "\"de_lake\"", "r");
        assertThat(real.get("status").asText()).isEqualTo("MATCHED");
        assertThat(real.get("match").get("players").asInt()).isEqualTo(1);
        assertThat(fake(off).get("status").asText()).isEqualTo("OFF");

        fakeEnabled(off, true);                                            // switched on: it is applied and the match is complete
        assertRoster(poll("r").get("assignment"), 4, 1, 3);
    }

    @Test
    void fakePlayersNeverCreateAMatchWithoutASuitableRealSearch() throws Exception {
        long server = addServer("10.0.0.5", 27017, "competitive", "de_dust2");
        long fakeId = addFake("competitive", 5, "\"de_dust2\"");          // a profile alone starts nothing
        assertThat(matches().size()).isZero();
        assertThat(server(server).get("state").asText()).isEqualTo("AVAILABLE");

        // a real search of another mode, or of Competitive on another map, is no anchor for them either
        search(1, WINGMAN, null, "w");
        search(2, COMPETITIVE, "\"de_mirage\"", "m");
        assertThat(matches().size()).isZero();
        assertThat(fake(fakeId).get("matches").size()).isZero();

        // and when the only real player leaves, the match falls apart and nothing is left running
        search(REAL, COMPETITIVE, "\"de_dust2\"", "r");
        assertThat(fake(fakeId).get("matches").size()).isEqualTo(1);
        cancel(REAL, "r");
        assertThat(fake(fakeId).get("matches").size()).isZero();
        assertThat(fake(fakeId).get("status").asText()).isEqualTo("ON");
        assertThat(server(server).get("state").asText()).isEqualTo("AVAILABLE");
        for (JsonNode m : matches()) {
            assertThat(m.get("status").asText()).isEqualTo("CANCELLED");
        }
    }

    // ------------------------------------------------------------------------------------------ the srcds handshake

    private static final String ROSTER = "/api/v1/servers/roster?address=192.168.1.150&port=27017";

    private ResultActions pollRoster() throws Exception {
        return mvc.perform(get(ROSTER).header(KEY, "test-api-key"));
    }

    private ResultActions ready(String matchId) throws Exception {
        return mvc.perform(post("/api/v1/servers/roster/ready").header(KEY, "test-api-key")
                .contentType(MediaType.APPLICATION_JSON)
                .content("{\"address\":\"192.168.1.150\",\"port\":27017,\"match_id\":\"" + matchId + "\"}"));
    }

    @Test
    void aServerThatReadsItsRosterHoldsTheAssignmentUntilItConfirmsTheRoster() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, "\"de_dust2\"");
        pollRoster().andExpect(status().isNotFound());                       // srcds polls: no match yet

        JsonNode real = search(REAL, COMPETITIVE, "\"de_dust2\"", "r");
        // the match is complete (10/10) but the player does not get the server before srcds armed the roster
        assertThat(real.get("status").asText()).isEqualTo("MATCHED");
        assertThat(real.hasNonNull("assignment")).isFalse();
        assertThat(real.get("match").get("players").asInt()).isEqualTo(10);
        assertThat(real.get("match").get("status").asText()).isEqualTo("READY");
        assertThat(real.get("match").get("awaiting_server").asBoolean()).isTrue();
        assertThat(matches().get(0).get("awaiting_server").asBoolean()).isTrue();

        // what srcds sees: the complete roster of that match
        JsonNode roster = body(pollRoster().andExpect(status().isOk()));
        assertThat(roster.get("status").asText()).isEqualTo("READY");
        assertThat(roster.get("required_players").asInt()).isEqualTo(10);
        assertThat(roster.get("real_players").asInt()).isEqualTo(1);
        assertThat(roster.get("fake_players").asInt()).isEqualTo(9);
        assertThat(roster.get("players").get(0).get("account_id").asLong()).isEqualTo(REAL);
        assertThat(roster.get("players").get(9).get("account_id").asLong()).isEqualTo(0xFA4E0009L);
        assertThat(poll("r").get("status").asText()).isEqualTo("MATCHED");   // still held

        // srcds armed it: now the player gets the server, with the same participants
        JsonNode answer = body(ready(roster.get("match_id").asText()).andExpect(status().isOk()));
        assertThat(answer.get("promoted").asInt()).isEqualTo(1);
        JsonNode after = poll("r");
        assertThat(after.get("status").asText()).isEqualTo("WAITING_ACCEPT");
        assertRoster(after.get("assignment"), 10, 1, 9);
        assertThat(after.get("assignment").get("match_id").asText()).isEqualTo(roster.get("match_id").asText());
        assertThat(after.get("match").get("awaiting_server").asBoolean()).isFalse();
        ready(roster.get("match_id").asText()).andExpect(status().isOk());   // a repeat changes nothing
    }

    @Test
    void aServerThatDoesNotReadItsRosterGetsTheOldImmediateAssignment() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");     // srcds with the legacy fake driver never asks
        addFake("competitive", 9, null);
        JsonNode real = search(REAL, COMPETITIVE, "\"de_dust2\"", "r");
        assertThat(real.get("status").asText()).isEqualTo("WAITING_ACCEPT");
        assertRoster(real.get("assignment"), 10, 1, 9);
    }

    @Test
    void theHoldEndsAfterTheAckTimeoutIfTheServerNeverConfirms() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, null);
        pollRoster();
        search(REAL, COMPETITIVE, "\"de_dust2\"", "r");
        clock.advance(Duration.ofSeconds(8));
        pollRoster();                                                         // still polling, still silent
        assertThat(poll("r").get("status").asText()).isEqualTo("MATCHED");
        clock.advance(Duration.ofSeconds(9));
        pollRoster();
        assertThat(poll("r").get("status").asText()).isEqualTo("MATCHED");   // 17 s < 25 s
        clock.advance(Duration.ofSeconds(9));
        pollRoster();
        assertThat(poll("r").get("status").asText()).isEqualTo("WAITING_ACCEPT");   // 26 s >= 25 s
    }

    @Test
    void theHoldEndsWhenTheServerStopsAskingForRosters() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, null);
        pollRoster();
        search(REAL, COMPETITIVE, "\"de_dust2\"", "r");
        assertThat(poll("r").get("status").asText()).isEqualTo("MATCHED");
        clock.advance(Duration.ofSeconds(11));                                // srcds went away: nobody will confirm
        tick();
        assertThat(poll("r").get("status").asText()).isEqualTo("WAITING_ACCEPT");
    }

    @Test
    void classicModesAreNeverHeldForAServer() throws Exception {
        addServer("192.168.1.150", 27016, "casual", "de_dust2");
        mvc.perform(get("/api/v1/servers/roster?address=192.168.1.150&port=27016").header(KEY, "test-api-key"));
        JsonNode casual = search(1, CASUAL, "\"de_dust2\"", "c");
        assertThat(casual.get("status").asText()).isEqualTo("READY_TO_CONNECT");
        assertThat(casual.get("assignment").get("players").size()).isEqualTo(1);
    }

    @Test
    void aConfirmationHasToNameTheMatchOfThatServer() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("competitive", 9, null);
        pollRoster();
        search(REAL, COMPETITIVE, "\"de_dust2\"", "r");
        String matchId = matches().get(0).get("id").asText();

        ready("m-00000000").andExpect(status().isNotFound());                 // unknown match
        mvc.perform(post("/api/v1/servers/roster/ready").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"address\":\"192.168.1.150\",\"port\":27018,\"match_id\":\"" + matchId + "\"}"))
                .andExpect(status().isNotFound());                            // the match of another server
        mvc.perform(post("/api/v1/servers/roster/ready").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"address\":\"10.9.9.9\",\"port\":1,\"match_id\":\"" + matchId + "\"}"))
                .andExpect(status().isNotFound());                            // a server that is not registered
        mvc.perform(post("/api/v1/servers/roster/ready").contentType(MediaType.APPLICATION_JSON)
                .content("{\"address\":\"192.168.1.150\",\"port\":27017,\"match_id\":\"" + matchId + "\"}"))
                .andExpect(status().isUnauthorized());                        // API key required
        mvc.perform(post("/api/v1/servers/roster/ready").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"address\":\"192.168.1.150\",\"port\":27017}")).andExpect(status().isBadRequest());
        assertThat(poll("r").get("status").asText()).isEqualTo("MATCHED");   // nothing of that promoted the player

        ready(matchId).andExpect(status().isOk());
        assertThat(poll("r").get("status").asText()).isEqualTo("WAITING_ACCEPT");
    }

    @Test
    void theRosterOfASecondMatchOnTheSameServerIsTheNewOne() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        long fakeId = addFake("competitive", 9, null);
        pollRoster();
        search(REAL, COMPETITIVE, "\"de_dust2\"", "a");
        String first = body(pollRoster()).get("match_id").asText();
        ready(first);

        // the player starts over (Accept failed): the old match is cancelled and the fake players are searching again on their
        // own. Its server is not handed out for the cooldown: srcds sees no match on it (and drops its old reservation)
        search(REAL, COMPETITIVE, "\"de_dust2\"", "b");
        assertThat(fake(fakeId).get("matches").size()).isEqualTo(1);       // the profile serves the new match, nobody presses Start
        pollRoster().andExpect(status().isNotFound());
        clock.advance(Duration.ofSeconds(16));                                // server-release-cooldown is PT15S
        pollRoster().andExpect(status().isNotFound());                        // srcds keeps asking: it reads its roster from here
        tick();
        JsonNode second = body(pollRoster().andExpect(status().isOk()));
        assertThat(second.get("match_id").asText()).isNotEqualTo(first);
        assertThat(second.get("players").size()).isEqualTo(10);
        assertThat(poll("b").get("status").asText()).isEqualTo("MATCHED");   // held again for the new match
        ready(first).andExpect(status().isNotFound());                        // the old match is over
        ready(second.get("match_id").asText()).andExpect(status().isOk());
        assertThat(poll("b").get("status").asText()).isEqualTo("WAITING_ACCEPT");
    }
}
