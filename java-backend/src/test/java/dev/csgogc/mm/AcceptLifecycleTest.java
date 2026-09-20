package dev.csgogc.mm;

import static org.assertj.core.api.Assertions.assertThat;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasterxml.jackson.databind.JsonNode;
import dev.csgogc.mm.server.GameServerRepository;
import java.nio.file.Path;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.http.MediaType;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;

/**
 * The lifecycle of an Accept match (RESEARCH_FINDINGS.md #63):
 * SEARCHING -> gathering (no server) -> FULL -> RESERVED -> ACCEPTING (deadline owned by the backend) -> ACCEPTED, or
 * CANCELLED: server free again, real and fake players searching again, the next search makes a NEW match.
 */
class AcceptLifecycleTest extends BackendTestBase {

    private static final Path DB_DIR = tempDir("mm-backend-lifecycle-test");

    @DynamicPropertySource
    static void database(DynamicPropertyRegistry registry) {
        registry.add("backend.database-path", () -> DB_DIR.resolve("mm.db").toString());
    }

    @Autowired GameServerRepository serverRepository;

    private static final long REAL = 1050166997L;
    private static final String DUST2 = "\"de_dust2\"";

    private JsonNode match(String id) throws Exception {
        for (JsonNode m : matches()) {
            if (m.get("id").asText().equals(id)) {
                return m;
            }
        }
        throw new AssertionError("match " + id + " not listed");
    }

    private String statusOf(JsonNode search) {
        return search.get("status").asText();
    }

    private String matchStatus(JsonNode search) {
        return search.get("match").get("status").asText();
    }

    private String matchIdOf(JsonNode search) {
        return search.get("match").get("match_id").asText();
    }

    private String state(long serverId) throws Exception {
        return server(serverId).get("state").asText();
    }

    // ------------------------------------------------------------------------------------------ when the server is reserved

    /** 1. 1/10: the server is not reserved */
    @Test
    void oneOfTenDoesNotReserveAServer() throws Exception {
        long serverId = addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        JsonNode real = search(REAL, COMPETITIVE, DUST2, "r");
        assertThat(statusOf(real)).isEqualTo("MATCHED");
        assertThat(matchStatus(real)).isEqualTo("FORMING");
        assertThat(real.get("match").get("players").asInt()).isEqualTo(1);
        assertThat(real.get("match").get("required_players").asInt()).isEqualTo(10);
        assertThat(real.get("match").get("server_id").asLong()).isZero();          // no server
        assertThat(real.has("assignment")).isFalse();
        assertThat(state(serverId)).isEqualTo("AVAILABLE");
        assertThat(server(serverId).has("reserved_match_id")).isFalse();
        assertThat(matches().get(0).get("server_address").asText()).isEmpty();
    }

    /** 2. the match is still gathering (the window of the Fake Players is running): no server is reserved */
    @Test
    void aGatheringMatchDoesNotReserveAServer() throws Exception {
        long serverId = addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 8, DUST2);
        gatherWindow(10);
        JsonNode real = search(REAL, COMPETITIVE, DUST2, "r");
        assertThat(real.get("match").get("players").asInt()).isEqualTo(1);
        assertThat(matchStatus(real)).isEqualTo("FORMING");
        assertThat(statusOf(real)).isEqualTo("MATCHED");
        pass(9);
        assertThat(matchStatus(poll("r"))).isEqualTo("FORMING");
        assertThat(state(serverId)).isEqualTo("AVAILABLE");
    }

    /** 3. the match is complete (real + configured virtual players): the server is RESERVED, by that very match */
    @Test
    void aCompleteMatchReservesTheServer() throws Exception {
        long serverId = addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 8, DUST2);
        gatherWindow(10);
        JsonNode real = search(REAL, COMPETITIVE, DUST2, "r");
        assertThat(state(serverId)).isEqualTo("AVAILABLE");

        pass(11);                                                          // the window is over: 1 real + 8 fake
        JsonNode full = poll("r");
        assertThat(full.get("match").get("players").asInt()).isEqualTo(9);
        assertThat(state(serverId)).isEqualTo("RESERVED");
        assertThat(server(serverId).get("reserved_match_id").asText()).isEqualTo(matchIdOf(full));
        assertThat(matchIdOf(full)).isEqualTo(matchIdOf(real));
        assertThat(match(matchIdOf(full)).get("server_port").asInt()).isEqualTo(27017);
    }

    /** 4. 10/10: ACCEPTING with a deadline (accept-timeout PT25S), the assignment is out */
    @Test
    void tenOfTenStartsAcceptingWithADeadline() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);
        Instant before = clock.instant();
        JsonNode real = search(REAL, COMPETITIVE, DUST2, "r");
        assertThat(statusOf(real)).isEqualTo("WAITING_ACCEPT");
        assertThat(matchStatus(real)).isEqualTo("ACCEPTING");
        assertThat(real.get("assignment").get("accept_required").asBoolean()).isTrue();
        assertThat(Instant.parse(real.get("match").get("accept_deadline_at").asText())).isEqualTo(before.plusSeconds(25));
        assertThat(match(matchIdOf(real)).get("status").asText()).isEqualTo("ACCEPTING");
    }

    // ------------------------------------------------------------------------------------------ the players accept

    /** 5. all accepted -> ACCEPTED, and it stays (the deadline no longer matters) */
    @Test
    void everybodyAcceptedMakesTheMatchAccepted() throws Exception {
        long serverId = addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);
        String matchId = matchIdOf(search(REAL, COMPETITIVE, DUST2, "r"));

        JsonNode answer = accepted(REAL, "r");
        assertThat(answer.get("accepted").asBoolean()).isTrue();
        assertThat(answer.get("match_accepted").asBoolean()).isTrue();
        assertThat(match(matchId).get("status").asText()).isEqualTo("ACCEPTED");
        assertThat(statusOf(poll("r"))).isEqualTo("READY_TO_CONNECT");
        assertThat(poll("r").has("assignment")).isTrue();
        assertThat(state(serverId)).isEqualTo("RESERVED");                  // the reservation stays until the players connect

        assertThat(accepted(REAL, "r").get("accepted").asBoolean()).isTrue();   // a repeated report changes nothing
        clock.advance(Duration.ofSeconds(60));
        tick();
        assertThat(match(matchId).get("status").asText()).isEqualTo("ACCEPTED");
        assertThat(state(serverId)).isEqualTo("RESERVED");
    }

    /** the match is ACCEPTED only when every real player reported; the backend checks who reports */
    @Test
    void theMatchNeedsTheReportOfEveryRealPlayerAndOnlyMembersCanReport() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 2, null);
        gatherWindow(10);
        search(1, WINGMAN, "\"de_lake\"", "a");
        search(2, WINGMAN, "\"de_lake\"", "b");
        pass(11);
        JsonNode second = poll("b");                                        // 2 real + 2 fake = 4: the Accept starts
        String matchId = matchIdOf(second);
        assertThat(matchStatus(second)).isEqualTo("ACCEPTING");

        JsonNode first = accepted(1, "a");
        assertThat(first.get("accepted").asBoolean()).isTrue();
        assertThat(first.get("match_accepted").asBoolean()).isFalse();
        assertThat(match(matchId).get("status").asText()).isEqualTo("ACCEPTING");
        assertThat(statusOf(poll("a"))).isEqualTo("WAITING_ACCEPT");

        // not a member / no such search / another request id / a match that is not accepting: nothing changes
        assertThat(accepted(77, "x").get("reason").asText()).isEqualTo("no_active_search");
        assertThat(accepted(2, "wrong-id").get("reason").asText()).isEqualTo("request_id_mismatch");
        assertThat(match(matchId).get("status").asText()).isEqualTo("ACCEPTING");

        JsonNode last = accepted(2, "b");
        assertThat(last.get("match_accepted").asBoolean()).isTrue();
        assertThat(match(matchId).get("status").asText()).isEqualTo("ACCEPTED");
        assertThat(statusOf(poll("a"))).isEqualTo("READY_TO_CONNECT");
        assertThat(statusOf(poll("b"))).isEqualTo("READY_TO_CONNECT");
    }

    @Test
    void aPlayerInAGatheringMatchCannotAccept() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        search(REAL, COMPETITIVE, DUST2, "r");                              // 1/10, gathering
        JsonNode answer = accepted(REAL, "r");
        assertThat(answer.get("accepted").asBoolean()).isFalse();
        assertThat(answer.get("reason").asText()).isEqualTo("match_not_accepting");
        assertThat(statusOf(poll("r"))).isEqualTo("MATCHED");
    }

    @Test
    void theAcceptedEndpointNeedsTheApiKeyAndAValidBody() throws Exception {
        mvc.perform(post("/api/v1/matchmaking/accepted").contentType(MediaType.APPLICATION_JSON)
                .content("{\"account_id\":1,\"request_id\":\"r\"}")).andExpect(status().isUnauthorized());
        mvc.perform(post("/api/v1/matchmaking/accepted").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"request_id\":\"r\"}")).andExpect(status().isBadRequest());
        mvc.perform(post("/api/v1/matchmaking/accepted").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"account_id\":1,\"request_id\":\"not valid!\"}")).andExpect(status().isBadRequest());
    }

    // ------------------------------------------------------------------------------------------ the timeout

    /** 6. the deadline passed -> CANCELLED (owned by the backend, nobody has to report anything) */
    @Test
    void theAcceptTimeoutCancelsTheMatch() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);
        String matchId = matchIdOf(search(REAL, COMPETITIVE, DUST2, "r"));

        clock.advance(Duration.ofSeconds(24));
        tick();
        assertThat(match(matchId).get("status").asText()).isEqualTo("ACCEPTING");   // 24 s < 25 s

        clock.advance(Duration.ofSeconds(2));
        tick();
        assertThat(match(matchId).get("status").asText()).isEqualTo("CANCELLED");
        assertThat(match(matchId).has("ended_at")).isTrue();
    }

    /** 7. ... and the server is AVAILABLE again (after the cooldown the game server needs to drop the reservation) */
    @Test
    void theAcceptTimeoutFreesTheServer() throws Exception {
        long serverId = addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);
        search(REAL, COMPETITIVE, DUST2, "r");
        assertThat(state(serverId)).isEqualTo("RESERVED");

        clock.advance(Duration.ofSeconds(26));
        tick();
        JsonNode freed = server(serverId);
        assertThat(freed.get("state").asText()).isEqualTo("AVAILABLE");
        assertThat(freed.has("reserved_match_id")).isFalse();
        assertThat(freed.has("reserved_at")).isFalse();
        assertThat(freed.has("available_after")).isTrue();                  // not handed out before the cooldown is over
    }

    /** 8. ... the profile stays: the next match of the same player gets its virtual players from it again, nobody presses Start */
    @Test
    void theAcceptTimeoutKeepsTheProfileForTheNextMatch() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        long fakeId = addFake("competitive", 9, DUST2);
        String first = matchIdOf(search(REAL, COMPETITIVE, DUST2, "r"));
        assertThat(fake(fakeId).get("matches").get(0).get("match_id").asText()).isEqualTo(first);

        clock.advance(Duration.ofSeconds(26));
        tick();
        JsonNode again = fake(fakeId);
        assertThat(again.get("status").asText()).isEqualTo("ON");
        assertThat(again.get("matches").size()).isEqualTo(1);                          // only the live match: the new one
        assertThat(again.get("matches").get(0).get("match_id").asText()).isNotEqualTo(first);
        assertThat(again.get("matches").get(0).get("match_status").asText()).isEqualTo("FULL");
        assertThat(again.get("matches").get(0).get("fake_players").asInt()).isEqualTo(9);
    }

    /** 8b. the real player leaves during the Accept: the match is gone, the profile is untouched and used by nobody */
    @Test
    void theProfileIsUntouchedWhenTheRealPlayerLeavesDuringTheAccept() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        long fakeId = addFake("competitive", 9, DUST2);
        String matchId = matchIdOf(search(REAL, COMPETITIVE, DUST2, "r"));

        cancel(REAL, "r");                                                  // the player pressed Cancel instead of Accept
        assertThat(match(matchId).get("status").asText()).isEqualTo("CANCELLED");
        JsonNode f = fake(fakeId);
        assertThat(f.get("status").asText()).isEqualTo("ON");
        assertThat(f.get("matches").size()).isZero();
    }

    /** 9. a search after the timeout is gathered normally: a NEW match, the same server once it is free again */
    @Test
    void aSearchAfterTheTimeoutMakesANewMatchOnTheFreedServer() throws Exception {
        long serverId = addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);
        JsonNode first = search(REAL, COMPETITIVE, DUST2, "r");
        String firstMatch = matchIdOf(first);
        String firstAssignment = first.get("assignment").get("match_id").asText();

        clock.advance(Duration.ofSeconds(26));
        tick();
        // the same search (same request id) carries on: back in the queue, in a new full match that waits for the server
        JsonNode waiting = poll("r");
        assertThat(statusOf(waiting)).isEqualTo("MATCHED");
        assertThat(waiting.has("assignment")).isFalse();                    // the old assignment is not active any more
        assertThat(matchIdOf(waiting)).isNotEqualTo(firstMatch);
        assertThat(matchStatus(waiting)).isEqualTo("FULL");
        assertThat(state(serverId)).isEqualTo("AVAILABLE");

        clock.advance(Duration.ofSeconds(15));                              // server-release-cooldown
        tick();
        JsonNode again = poll("r");
        assertThat(statusOf(again)).isEqualTo("WAITING_ACCEPT");
        assertThat(matchStatus(again)).isEqualTo("ACCEPTING");
        assertThat(again.get("assignment").get("match_id").asText()).isNotEqualTo(firstAssignment);
        assertThat(again.get("assignment").get("players").size()).isEqualTo(10);
        assertThat(state(serverId)).isEqualTo("RESERVED");

        assertThat(accepted(REAL, "r").get("match_accepted").asBoolean()).isTrue();   // and it can be accepted
    }

    /** a player who accepted is not spared: the match is all or nothing, everybody searches again */
    @Test
    void theTimeoutSendsEveryPlayerBackToTheQueueEvenTheOneWhoAccepted() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 2, null);
        gatherWindow(10);
        search(1, WINGMAN, "\"de_lake\"", "a");
        search(2, WINGMAN, "\"de_lake\"", "b");
        pass(11);
        String matchId = matchIdOf(poll("b"));
        assertThat(accepted(1, "a").get("match_accepted").asBoolean()).isFalse();   // b never accepts

        clock.advance(Duration.ofSeconds(26));
        tick();
        assertThat(match(matchId).get("status").asText()).isEqualTo("CANCELLED");
        for (String request : new String[] { "a", "b" }) {
            JsonNode again = poll(request);
            assertThat(statusOf(again)).isEqualTo("MATCHED");                 // gathering again (in the next match)
            assertThat(matchIdOf(again)).isNotEqualTo(matchId);
        }
        // the accept of the first player does not carry over to the next match
        assertThat(accepted(1, "a").get("reason").asText()).isEqualTo("match_not_accepting");
    }

    /** the retail deadline is measured from the moment the players hold the server, not from the reservation */
    @Test
    void theDeadlineStartsWhenThePlayersGetTheServerNotWhenItIsReserved() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);
        String road = "/api/v1/servers/roster?address=192.168.1.150&port=27017";
        mvc.perform(get(road).header(KEY, "test-api-key"));                 // srcds reads its roster from the backend

        JsonNode held = search(REAL, COMPETITIVE, DUST2, "r");              // the server is reserved, the players are held
        assertThat(statusOf(held)).isEqualTo("MATCHED");
        assertThat(matchStatus(held)).isEqualTo("READY");
        assertThat(held.get("match").has("accept_deadline_at")).isFalse();

        clock.advance(Duration.ofSeconds(20));
        mvc.perform(get(road).header(KEY, "test-api-key"));
        tick();
        assertThat(statusOf(poll("r"))).isEqualTo("MATCHED");                 // no Accept clock is running yet

        String matchId = matchIdOf(held);
        mvc.perform(post("/api/v1/servers/roster/ready").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"address\":\"192.168.1.150\",\"port\":27017,\"match_id\":\"" + matchId + "\"}")).andExpect(status().isOk());
        JsonNode accepting = poll("r");
        assertThat(statusOf(accepting)).isEqualTo("WAITING_ACCEPT");
        assertThat(Instant.parse(accepting.get("match").get("accept_deadline_at").asText())).isEqualTo(clock.instant().plusSeconds(25));

        clock.advance(Duration.ofSeconds(24));
        tick();
        assertThat(match(matchId).get("status").asText()).isEqualTo("ACCEPTING");
        clock.advance(Duration.ofSeconds(2));
        tick();
        assertThat(match(matchId).get("status").asText()).isEqualTo("CANCELLED");
    }

    /** what the srcds side sees: READY while the players accept, nothing once the match is gone (then it drops its reservation) */
    @Test
    void theGameServerSeesTheRosterUntilTheMatchIsCancelled() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);
        String road = "/api/v1/servers/roster?address=192.168.1.150&port=27017";
        mvc.perform(get(road).header(KEY, "test-api-key"));
        String matchId = matchIdOf(search(REAL, COMPETITIVE, DUST2, "r"));
        mvc.perform(post("/api/v1/servers/roster/ready").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"address\":\"192.168.1.150\",\"port\":27017,\"match_id\":\"" + matchId + "\"}")).andExpect(status().isOk());

        JsonNode roster = body(mvc.perform(get(road).header(KEY, "test-api-key")).andExpect(status().isOk()));
        assertThat(roster.get("match_id").asText()).isEqualTo(matchId);
        assertThat(roster.get("status").asText()).isEqualTo("READY");       // what srcds' Snapshot::Complete() looks for
        assertThat(roster.get("phase").asText()).isEqualTo("ACCEPTING");

        clock.advance(Duration.ofSeconds(26));
        mvc.perform(get(road).header(KEY, "test-api-key"));
        tick();
        mvc.perform(get(road).header(KEY, "test-api-key")).andExpect(status().isNotFound());   // gone: unreserve
    }

    // ------------------------------------------------------------------------------------------ leaving

    @Test
    void aPlayerWhoLeavesAfterAcceptingDoesNotCancelTheMatch() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 2, null);
        gatherWindow(10);
        search(1, WINGMAN, "\"de_lake\"", "a");
        search(2, WINGMAN, "\"de_lake\"", "b");
        pass(11);
        String matchId = matchIdOf(poll("b"));
        accepted(1, "a");
        cancel(1, "a");                                                     // connecting: the client sends its stop
        assertThat(match(matchId).get("status").asText()).isEqualTo("ACCEPTING");
        assertThat(accepted(2, "b").get("match_accepted").asBoolean()).isTrue();   // the one who is left completes it
        assertThat(match(matchId).get("status").asText()).isEqualTo("ACCEPTED");
    }

    @Test
    void aPlayerWhoLeavesAFullMatchThatWaitsForAServerMakesItGatherAgain() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 3, null);
        assertThat(matchStatus(search(1, WINGMAN, "\"de_lake\"", "a"))).isEqualTo("ACCEPTING");   // holds the only server

        gatherWindow(10);
        search(2, WINGMAN, "\"de_lake\"", "c");
        search(3, WINGMAN, "\"de_lake\"", "d");
        pass(11);                                                           // c + d + 2 fake = 4: full, but no free server
        assertThat(matchStatus(poll("d"))).isEqualTo("FULL");

        cancel(3, "d");                                                     // the match gathers again; its window is over: c + 3 fake
        JsonNode c = poll("c");
        assertThat(matchStatus(c)).isEqualTo("FULL");
        assertThat(c.get("match").get("players").asInt()).isEqualTo(4);
        assertThat(c.get("match").get("fake_players").asInt()).isEqualTo(3);
    }

    // ------------------------------------------------------------------------------------------ two full matches, one server

    /** 10. two full matches never get the same server */
    @Test
    void twoFullMatchesNeverGetTheSameServer() throws Exception {
        long only = addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 3, null);
        addFake("wingman", 3, null);
        JsonNode a = search(1, WINGMAN, "\"de_lake\"", "a");                // a + 3 fake = 4: full, gets the server
        JsonNode b = search(2, WINGMAN, "\"de_lake\"", "b");                // b + 3 fake = 4: full, no server left
        assertThat(matchStatus(a)).isEqualTo("ACCEPTING");
        assertThat(statusOf(poll("b"))).isEqualTo("MATCHED");
        assertThat(matchStatus(poll("b"))).isEqualTo("FULL");
        assertThat(server(only).get("reserved_match_id").asText()).isEqualTo(matchIdOf(a));

        // a second server: the waiting match takes it, and it is another server
        long second = addServer("192.168.1.150", 27019, "wingman", "de_lake");
        JsonNode b2 = poll("b");
        assertThat(statusOf(b2)).isEqualTo("WAITING_ACCEPT");
        assertThat(server(second).get("reserved_match_id").asText()).isEqualTo(matchIdOf(b2));
        assertThat(server(only).get("reserved_match_id").asText()).isEqualTo(matchIdOf(a));
        assertThat(b2.get("assignment").get("server_port").asInt()).isEqualTo(27019);
        assertThat(a.get("assignment").get("server_port").asInt()).isEqualTo(27018);
    }

    /** the reservation itself is atomic: the conditional UPDATE lets exactly one match take an AVAILABLE server */
    @Test
    void aServerCanBeReservedByOnlyOneMatch() throws Exception {
        long id = addServer("192.168.1.150", 27018, "wingman", "de_lake");
        Instant now = clock.instant();
        assertThat(serverRepository.reserve(id, "m-00000001", now)).isTrue();
        assertThat(serverRepository.reserve(id, "m-00000002", now)).isFalse();
        assertThat(server(id).get("reserved_match_id").asText()).isEqualTo("m-00000001");
    }

    /** full matches racing for few servers from several threads: every server ends up with exactly one match */
    @Test
    void concurrentFullMatchesNeverShareAServer() throws Exception {
        int servers = 3;
        int matchesWanted = 8;
        for (int i = 0; i < servers; i++) {
            addServer("192.168.1.150", 27100 + i, "wingman", "de_lake");
        }
        for (int i = 0; i < matchesWanted; i++) {
            addFake("wingman", 3, null);
        }
        List<Thread> threads = new ArrayList<>();
        List<Throwable> failures = java.util.Collections.synchronizedList(new ArrayList<>());
        for (int i = 0; i < matchesWanted; i++) {
            final int account = 100 + i;
            Thread t = new Thread(() -> {
                try {
                    search(account, WINGMAN, "\"de_lake\"", "race-" + account);
                } catch (Throwable e) {
                    failures.add(e);
                }
            });
            threads.add(t);
        }
        threads.forEach(Thread::start);
        for (Thread t : threads) {
            t.join();
        }
        assertThat(failures).isEmpty();

        Set<String> holders = new HashSet<>();
        int reserved = 0;
        for (JsonNode s : body(mvc.perform(get("/admin/api/servers").with(ADMIN)))) {
            if (s.get("state").asText().equals("RESERVED")) {
                reserved++;
                assertThat(holders.add(s.get("reserved_match_id").asText())).as("a match holds one server only").isTrue();
            }
        }
        assertThat(reserved).isEqualTo(servers);
        long accepting = 0;
        for (JsonNode m : matches()) {
            accepting += m.get("status").asText().equals("ACCEPTING") ? 1 : 0;
            assertThat(m.get("server_address").asText().isEmpty() && m.get("status").asText().equals("ACCEPTING")).isFalse();
        }
        assertThat(accepting).isEqualTo(servers);
    }

    // ------------------------------------------------------------------------------------------ housekeeping

    /** findReservedBefore has a job now: a server that is RESERVED for a match that does not hold it is given back */
    @Test
    void aReservationNoLiveMatchHoldsIsGivenBack() throws Exception {
        long id = addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        Instant now = clock.instant();
        jdbc.sql("UPDATE game_server SET state = 'RESERVED', reserved_match_id = 'm-deadbeef', reserved_at = ? WHERE id = ?")
                .params(now.toEpochMilli(), id).update();
        clock.advance(Duration.ofSeconds(30));
        tick();
        assertThat(state(id)).isEqualTo("RESERVED");                        // within the grace period: a hand-over may be running
        clock.advance(Duration.ofMinutes(2));
        tick();
        assertThat(state(id)).isEqualTo("AVAILABLE");
        assertThat(server(id).has("reserved_match_id")).isFalse();
    }

    /** a match of the previous version (FORMING with a reserved server) is taken apart, its players queue again */
    @Test
    void aFormingMatchThatHoldsAServerFromAnOlderVersionIsDissolved() throws Exception {
        long id = addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        long now = clock.instant().toEpochMilli();
        jdbc.sql("UPDATE game_server SET state = 'RESERVED', reserved_match_id = 'm-0ldf0rm1', reserved_at = ? WHERE id = ?")
                .params(now, id).update();
        jdbc.sql("INSERT INTO matchmaking_match (id, category, server_id, server_host, server_port, map, required_players, "
                        + "accept_required, status, created_at) VALUES ('m-0ldf0rm1', 'competitive', ?, '192.168.1.150', 27017, "
                        + "'de_dust2', 10, 1, 'FORMING', ?)").params(id, now).update();
        jdbc.sql("INSERT INTO matchmaking_search (account_id, game_type, category, game_mode, maps, request_id, status, source, "
                        + "started_at, last_seen_at, match_id, matched_at) VALUES (5, 8, 'competitive', 'competitive', 'de_dust2', "
                        + "'old-1', 'MATCHED', 'gc', ?, ?, 'm-0ldf0rm1', ?)").params(now, now, now).update();
        tick();
        assertThat(state(id)).isEqualTo("AVAILABLE");
        JsonNode search = poll("old-1");
        assertThat(statusOf(search)).isEqualTo("MATCHED");                    // gathering in a match of the new kind
        assertThat(matchIdOf(search)).isNotEqualTo("m-0ldf0rm1");
        assertThat(matchStatus(search)).isEqualTo("FORMING");
    }

    /** a gathering match nobody is in any more disappears */
    @Test
    void anEmptyGatheringMatchIsDissolved() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 3, DUST2);
        gatherWindow(10);
        search(REAL, COMPETITIVE, DUST2, "r");
        assertThat(matches().get(0).get("status").asText()).isEqualTo("FORMING");
        jdbc.sql("UPDATE matchmaking_search SET status = 'CANCELLED', ended_at = 1 WHERE request_id = 'r'").update();   // vanished
        tick();
        assertThat(matches().get(0).get("status").asText()).isEqualTo("CANCELLED");
    }

    // ------------------------------------------------------------------------------------------ what did not change

    /** classic modes: no gathering, no reservation, no Accept, no deadline */
    @Test
    void classicModesKnowNothingOfAccept() throws Exception {
        long serverId = addServer("192.168.1.150", 27016, "casual", "de_dust2");
        JsonNode casual = search(1, CASUAL, DUST2, "c");
        assertThat(statusOf(casual)).isEqualTo("READY_TO_CONNECT");
        assertThat(matchStatus(casual)).isEqualTo("READY");
        assertThat(casual.get("match").has("accept_deadline_at")).isFalse();
        assertThat(state(serverId)).isEqualTo("AVAILABLE");
        clock.advance(Duration.ofSeconds(60));
        tick();
        assertThat(statusOf(poll("c"))).isEqualTo("READY_TO_CONNECT");         // no deadline cancels it
        assertThat(matches().get(0).get("status").asText()).isEqualTo("READY");
    }

    @Test
    void aSearchWithoutAnyServerThatCouldRunItsMapsKeepsSearching() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        JsonNode nowhere = search(REAL, COMPETITIVE, "\"de_nuke\"", "n");   // no server runs de_nuke: nothing gathers
        assertThat(statusOf(nowhere)).isEqualTo("SEARCHING");
        assertThat(matches().size()).isZero();
        addServer("192.168.1.150", 27018, "competitive", "de_nuke");
        assertThat(statusOf(poll("n"))).isEqualTo("MATCHED");
    }
}
