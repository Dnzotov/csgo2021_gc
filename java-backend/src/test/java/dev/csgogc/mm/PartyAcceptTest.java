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

/**
 * Several real players in one Accept match (RESEARCH_FINDINGS.md #65). The lobby leader's MatchmakingStart carries every
 * member of the party, the members' clients send nothing: the backend keeps a search for each member, puts the whole party
 * into ONE match, gives every member the assignment and lets nobody connect before EVERY real player accepted.
 */
class PartyAcceptTest extends BackendTestBase {

    private static final Path DB_DIR = tempDir("mm-backend-party-test");

    @DynamicPropertySource
    static void database(DynamicPropertyRegistry registry) {
        registry.add("backend.database-path", () -> DB_DIR.resolve("mm.db").toString());
    }

    private static final String DUST2 = "\"de_dust2\"";
    private static final String LAKE = "\"de_lake\"";

    /** POST /search of a party leader; partyIds = the other accounts of the lobby (the leader may be listed too) */
    private JsonNode partySearch(long leader, long gameType, String maps, String requestId, long... partyIds) throws Exception {
        StringBuilder ids = new StringBuilder();
        for (long id : partyIds) {
            ids.append(ids.length() == 0 ? "" : ",").append(id);
        }
        String request = "{\"account_id\":" + leader + ",\"game_type\":" + gameType + (maps == null ? "" : ",\"maps\":[" + maps + "]")
                + ",\"party_account_ids\":[" + ids + "],\"request_id\":\"" + requestId + "\"}";
        return body(mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key")
                .contentType(MediaType.APPLICATION_JSON).content(request)).andExpect(status().isOk())).get("search");
    }

    /** what the GC of a party member asks: the live search of its account */
    private JsonNode accountSearch(long account) throws Exception {
        return body(mvc.perform(get("/api/v1/matchmaking/account/" + account).header(KEY, "test-api-key"))
                .andExpect(status().isOk())).get("search");
    }

    private String memberRequestId(long account) throws Exception {
        return accountSearch(account).get("request_id").asText();
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

    private String matchState(String matchId) throws Exception {
        for (JsonNode m : matches()) {
            if (m.get("id").asText().equals(matchId)) {
                return m.get("status").asText();
            }
        }
        throw new AssertionError("match " + matchId);
    }

    // ------------------------------------------------------------------------------------------ the party is one match

    /** 4. a party of several real players: ONE match, everybody gets Match Found, everybody is in the roster */
    @Test
    void aPartyIsOneMatchAndEveryMemberGetsTheAssignment() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 2, LAKE);
        JsonNode leader = partySearch(1, WINGMAN, LAKE, "lead-1", 2);          // the lobby of 2: leader 1 + member 2

        assertThat(statusOf(leader)).isEqualTo("WAITING_ACCEPT");                // 2 real + 2 fake = 4: Match Found
        String matchId = matchIdOf(leader);
        assertThat(matchStatus(leader)).isEqualTo("ACCEPTING");

        JsonNode member = accountSearch(2);                                      // the member's GC finds its search
        assertThat(member.get("party_leader_id").asLong()).isEqualTo(leader.get("id").asLong());
        assertThat(member.get("mode").asText()).isEqualTo("wingman");
        assertThat(member.get("game_type").asLong()).isEqualTo(WINGMAN);
        assertThat(member.get("request_id").asText()).startsWith("pt");
        assertThat(statusOf(member)).isEqualTo("WAITING_ACCEPT");
        assertThat(matchIdOf(member)).isEqualTo(matchId);                       // the same match
        assertThat(member.get("assignment").get("match_id").asText()).isEqualTo(leader.get("assignment").get("match_id").asText());
        assertThat(member.get("assignment").get("server_port").asInt()).isEqualTo(27018);

        // both real accounts are in the roster that srcds arms: awaiting counts BOTH of them
        JsonNode players = leader.get("assignment").get("players");
        assertThat(players.size()).isEqualTo(4);
        assertThat(leader.get("assignment").get("real_players").asInt()).isEqualTo(2);
        assertThat(leader.get("assignment").get("fake_players").asInt()).isEqualTo(2);
        assertThat(players.get(0).get("account_id").asLong()).isEqualTo(1);
        assertThat(players.get(1).get("account_id").asLong()).isEqualTo(2);
    }

    /** 6. one player accepts earlier than the rest: nobody connects, the match is not ACCEPTED */
    @Test
    void oneMemberAcceptingDoesNotStartTheConnect() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 2, LAKE);
        JsonNode leader = partySearch(1, WINGMAN, LAKE, "lead-2", 2);
        String matchId = matchIdOf(leader);
        String memberRequest = memberRequestId(2);

        JsonNode first = accepted(1, "lead-2");                                  // the leader is quicker
        assertThat(first.get("accepted").asBoolean()).isTrue();
        assertThat(first.get("match_accepted").asBoolean()).isFalse();           // ... and connects nowhere
        assertThat(matchState(matchId)).isEqualTo("ACCEPTING");
        assertThat(statusOf(poll("lead-2"))).isEqualTo("WAITING_ACCEPT");       // still not READY_TO_CONNECT
        assertThat(statusOf(accountSearch(2))).isEqualTo("WAITING_ACCEPT");

        JsonNode last = accepted(2, memberRequest);                              // 7. everybody accepted
        assertThat(last.get("match_accepted").asBoolean()).isTrue();
        assertThat(matchState(matchId)).isEqualTo("ACCEPTED");
        assertThat(statusOf(poll("lead-2"))).isEqualTo("READY_TO_CONNECT");
        assertThat(statusOf(accountSearch(2))).isEqualTo("READY_TO_CONNECT");
    }

    /** 5. several real players + fake fill in a big mode (a party of 3 in Competitive) */
    @Test
    void aPartyOfThreeAndSevenFakePlayersMakeACompetitiveMatch() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 7, DUST2);
        JsonNode leader = partySearch(10, COMPETITIVE, DUST2, "lead-3", 11, 12);
        assertThat(statusOf(leader)).isEqualTo("WAITING_ACCEPT");
        assertThat(leader.get("assignment").get("real_players").asInt()).isEqualTo(3);
        assertThat(leader.get("assignment").get("fake_players").asInt()).isEqualTo(7);
        for (long member : new long[] { 11, 12 }) {
            assertThat(statusOf(accountSearch(member))).isEqualTo("WAITING_ACCEPT");
            assertThat(matchIdOf(accountSearch(member))).isEqualTo(matchIdOf(leader));
        }
        assertThat(accepted(10, "lead-3").get("match_accepted").asBoolean()).isFalse();
        assertThat(accepted(11, memberRequestId(11)).get("match_accepted").asBoolean()).isFalse();
        assertThat(accepted(12, memberRequestId(12)).get("match_accepted").asBoolean()).isTrue();
    }

    /** Danger Zone: a party of 2 + 14 fake fill 16 (the same mechanism, not a mode special case) */
    @Test
    void aDangerZonePartyIsTreatedLikeAnyOtherAcceptMatch() throws Exception {
        addServer("192.168.1.150", 27019, "dangerzone", "dz_blacksite");
        addFake("dangerzone", 14, "\"dz_blacksite\"");
        JsonNode leader = partySearch(20, DANGERZONE, "\"dz_blacksite\"", "lead-4", 21);
        assertThat(statusOf(leader)).isEqualTo("WAITING_ACCEPT");
        assertThat(leader.get("assignment").get("required_players").asInt()).isEqualTo(16);
        assertThat(leader.get("assignment").get("real_players").asInt()).isEqualTo(2);
        assertThat(accepted(20, "lead-4").get("match_accepted").asBoolean()).isFalse();
        assertThat(accepted(21, memberRequestId(21)).get("match_accepted").asBoolean()).isTrue();
    }

    /** the same for one real player and fake fill in every Accept mode (1. 2. 3.) */
    @Test
    void oneRealPlayerAndFakeFillWorksInEveryAcceptMode() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addServer("192.168.1.150", 27019, "dangerzone", "dz_blacksite");
        addFake("competitive", 9, DUST2);
        addFake("wingman", 3, LAKE);
        addFake("dangerzone", 15, "\"dz_blacksite\"");
        addServer("192.168.1.150", 27020, "scrimcomp5v5", "de_dust2");
        addFake("scrimcomp5v5", 9, DUST2);
        Object[][] modes = { { COMPETITIVE, DUST2, 10L }, { WINGMAN, LAKE, 4L }, { DANGERZONE, "\"dz_blacksite\"", 16L },
                { 11L, DUST2, 10L } };                                                  // 11 = ScrimComp5v5
        long account = 300;
        for (Object[] mode : modes) {
            account++;
            JsonNode real = search(account, (Long) mode[0], (String) mode[1], "solo-" + account);
            assertThat(statusOf(real)).isEqualTo("WAITING_ACCEPT");
            assertThat(real.get("assignment").get("required_players").asLong()).isEqualTo((Long) mode[2]);
            assertThat(real.get("assignment").get("real_players").asInt()).isEqualTo(1);
            assertThat(accepted(account, "solo-" + account).get("match_accepted").asBoolean()).isTrue();   // fakes count as accepted
        }
    }

    /** two players who search on their own (no party) + a fake search sized for the rest are one match too */
    @Test
    void twoIndependentPlayersAndFakeFillShareAMatch() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 2, LAKE);
        gatherWindow(10);
        search(1, WINGMAN, LAKE, "a");
        search(2, WINGMAN, LAKE, "b");
        pass(11);
        JsonNode second = poll("b");
        assertThat(statusOf(second)).isEqualTo("WAITING_ACCEPT");
        assertThat(matchIdOf(poll("a"))).isEqualTo(matchIdOf(second));
        assertThat(accepted(1, "a").get("match_accepted").asBoolean()).isFalse();
        assertThat(accepted(2, "b").get("match_accepted").asBoolean()).isTrue();
    }

    // ------------------------------------------------------------------------------------------ room, cancel, timeout

    @Test
    void aPartyThatDoesNotFitTheGatheringMatchStartsItsOwn() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addServer("192.168.1.151", 27018, "wingman", "de_lake");
        addFake("wingman", 0, LAKE);
        gatherWindow(10);
        search(1, WINGMAN, LAKE, "solo");                                        // 1 of 4 gathering, room for 3
        JsonNode roomy = partySearch(2, WINGMAN, LAKE, "p-ok", 3);               // a party of 2 fits: 3 of 4
        assertThat(matchIdOf(roomy)).isEqualTo(matchIdOf(poll("solo")));
        assertThat(roomy.get("match").get("players").asInt()).isEqualTo(3);

        JsonNode party3 = partySearch(5, WINGMAN, LAKE, "p-big", 6, 7);          // a party of 3 does not fit next to 3
        assertThat(matchIdOf(party3)).isNotEqualTo(matchIdOf(poll("solo")));
        assertThat(party3.get("match").get("players").asInt()).isEqualTo(3);
    }

    /** 8. the leader cancels before the Accept: the whole party is out, the match falls apart */
    @Test
    void theLeaderCancellingTakesThePartyAlong() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 2, LAKE);
        JsonNode leader = partySearch(1, WINGMAN, LAKE, "lead-5", 2);
        String matchId = matchIdOf(leader);
        String memberRequest = memberRequestId(2);

        cancel(1, "lead-5");
        assertThat(matchState(matchId)).isEqualTo("CANCELLED");
        assertThat(statusOf(poll(memberRequest))).isEqualTo("CANCELLED");        // the member's search ended with the leader's
        mvc.perform(get("/api/v1/matchmaking/account/2").header(KEY, "test-api-key")).andExpect(status().isNotFound());
        assertThat(server(matches().get(0).get("server_id").asLong()).get("state").asText()).isEqualTo("AVAILABLE");
    }

    /** the stop the client sends when it CONNECTS (after the accept) must not pull the others out */
    @Test
    void theLeaderStopAfterAcceptingDoesNotEndTheMembersSearch() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        addFake("wingman", 2, LAKE);
        partySearch(1, WINGMAN, LAKE, "lead-6", 2);
        String memberRequest = memberRequestId(2);
        accepted(1, "lead-6");
        accepted(2, memberRequest);
        cancel(1, "lead-6");                                                     // the leader connects first and sends its stop
        assertThat(statusOf(poll(memberRequest))).isEqualTo("READY_TO_CONNECT"); // the member can still connect
        cancel(2, memberRequest);
    }

    /** 9. Accept timeout with a party: everybody is searching again, together, in a NEW match */
    @Test
    void theTimeoutSendsThePartyBackAndTheNextMatchHasThemAllAgain() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        long fakeId = addFake("wingman", 2, LAKE);
        JsonNode leader = partySearch(1, WINGMAN, LAKE, "lead-7", 2);
        String first = matchIdOf(leader);
        String memberRequest = memberRequestId(2);
        accepted(1, "lead-7");                                                   // one accepted, the other did not

        clock.advance(Duration.ofSeconds(26));
        tick();
        assertThat(matchState(first)).isEqualTo("CANCELLED");
        JsonNode again = poll("lead-7");
        JsonNode memberAgain = poll(memberRequest);
        assertThat(statusOf(again)).isEqualTo("MATCHED");
        assertThat(statusOf(memberAgain)).isEqualTo("MATCHED");
        assertThat(matchIdOf(again)).isNotEqualTo(first);
        assertThat(matchIdOf(memberAgain)).isEqualTo(matchIdOf(again));           // together
        assertThat(fake(fakeId).get("matches").size()).isEqualTo(1);            // the profile serves the new match on its own
        assertThat(accepted(1, "lead-7").get("reason").asText()).isEqualTo("match_not_accepting");   // no carried-over accept

        clock.advance(Duration.ofSeconds(15));                                    // server-release-cooldown
        tick();
        assertThat(statusOf(poll("lead-7"))).isEqualTo("WAITING_ACCEPT");
        assertThat(statusOf(poll(memberRequest))).isEqualTo("WAITING_ACCEPT");
        assertThat(accepted(1, "lead-7").get("match_accepted").asBoolean()).isFalse();
        assertThat(accepted(2, memberRequest).get("match_accepted").asBoolean()).isTrue();
    }

    /** a member that is searching on its own is taken into the party (a player is in one search only) */
    @Test
    void aMemberWithASearchOfItsOwnJoinsThePartyInstead() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        search(2, WINGMAN, LAKE, "own-2");
        partySearch(1, WINGMAN, LAKE, "lead-8", 2);
        assertThat(statusOf(poll("own-2"))).isEqualTo("CANCELLED");
        assertThat(accountSearch(2).get("party_leader_id").asLong()).isPositive();
    }

    /** a repeated request with the same request id keeps the party, another one rewrites it */
    @Test
    void aRepeatedSearchRequestKeepsThePartyAndAChangedOneRewritesIt() throws Exception {
        addServer("192.168.1.150", 27018, "wingman", "de_lake");
        partySearch(1, WINGMAN, LAKE, "lead-9", 2);
        String member = memberRequestId(2);
        partySearch(1, WINGMAN, LAKE, "lead-9", 2);                              // the same search again
        assertThat(memberRequestId(2)).isEqualTo(member);
        partySearch(1, WINGMAN, LAKE, "lead-9b", 2, 3);                           // another search: a party of 3 now
        assertThat(statusOf(poll(member))).isEqualTo("CANCELLED");                // the old member search ended, a new one exists
        assertThat(memberRequestId(2)).isNotEqualTo(member);
        assertThat(accountSearch(3).get("party_leader_id").asLong()).isPositive();
    }

    // ------------------------------------------------------------------------------------------ modes without Accept

    /** 11. no Accept, no reservation, no deadline in any classic mode - with a party too */
    @Test
    void classicModesNeverWaitForAccept() throws Exception {
        long[] modes = { CASUAL, DEATHMATCH, 4L, 5L, SKIRMISH };                  // Casual, Deathmatch, Arms Race, Demolition, Skirmish
        String[] categories = { "casual", "deathmatch", "armsrace", "demolition", "skirmish" };
        for (int i = 0; i < modes.length; i++) {
            long serverId = addServer("192.168.1.150", 27100 + i, categories[i], "de_dust2");
            JsonNode solo = search(500 + i, modes[i], DUST2, "c" + i);
            assertThat(statusOf(solo)).as(categories[i]).isEqualTo("READY_TO_CONNECT");
            assertThat(solo.get("assignment").get("accept_required").asBoolean()).isFalse();
            assertThat(solo.get("match").has("accept_deadline_at")).isFalse();
            assertThat(server(serverId).get("state").asText()).isEqualTo("AVAILABLE");   // never reserved
        }
        // a party in Casual: the members get the same server at once, nothing to accept
        JsonNode leader = partySearch(600, CASUAL, DUST2, "cp", 601);
        assertThat(statusOf(leader)).isEqualTo("READY_TO_CONNECT");
        assertThat(statusOf(accountSearch(601))).isEqualTo("READY_TO_CONNECT");
        assertThat(accountSearch(601).get("assignment").get("server_port").asInt())
                .isEqualTo(leader.get("assignment").get("server_port").asInt());
    }

    /** Cooperative (eGame 9) is an Accept mode the GC does not serve: the backend says so instead of gathering nobody */
    @Test
    void cooperativeIsRefusedNotSilentlyQueued() throws Exception {
        mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"account_id\":5,\"game_type\":9,\"request_id\":\"coop\"}")).andExpect(status().isBadRequest());
    }

    @Test
    void theAccountEndpointNeedsTheApiKeyAndKnowsNoStrangers() throws Exception {
        mvc.perform(get("/api/v1/matchmaking/account/5")).andExpect(status().isUnauthorized());
        mvc.perform(get("/api/v1/matchmaking/account/5").header(KEY, "test-api-key")).andExpect(status().isNotFound());
    }
}
