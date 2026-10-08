package dev.csgogc.mm;

import static org.assertj.core.api.Assertions.assertThat;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasterxml.jackson.databind.JsonNode;
import dev.csgogc.mm.search.SearchService;
import dev.csgogc.mm.skin.SkinSnapshotStore;
import java.nio.file.Path;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.http.MediaType;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;
import org.springframework.test.web.servlet.ResultActions;

/**
 * Skin sync, Phase A-C (research/backend_skin_sync_design.md): the GC of a player posts its EquippedSkinSnapshot, the
 * backend keeps it as temporary matchmaking state, attaches the snapshots of the real players of a match to the roster the
 * game server polls (GET /servers/roster -> skin_snapshots / skin_missing) and drops them when the search / match is over.
 * The matchmaking itself (search, Accept, reservation) is untouched; the existing test classes cover it.
 */
class SkinSnapshotTest extends BackendTestBase {

    private static final Path DB_DIR = tempDir("mm-backend-skin-test");

    @DynamicPropertySource
    static void database(DynamicPropertyRegistry registry) {
        registry.add("backend.database-path", () -> DB_DIR.resolve("mm.db").toString());
    }

    @Autowired SearchService searchService;
    @Autowired SkinSnapshotStore snapshotStore;

    /** the store lives in the shared Spring context (the database is cleaned by the base class): start every test empty */
    @BeforeEach
    void emptyStore() {
        snapshotStore.all().forEach(stored -> snapshotStore.remove(stored.accountId()));
    }

    private static final long REAL = 1050166997L;
    private static final String DUST2 = "\"de_dust2\"";
    private static final String ROSTER = "/api/v1/servers/roster?address=192.168.1.150&port=27017";

    // ------------------------------------------------------------------------------------------ helpers

    /** the project's test item: item 2, Karambit (507), paint 38, seed 41, wear 0.000001, CT knife slot */
    private static String knife(long itemId) {
        return "{\"item_id\":" + itemId + ",\"def_index\":507,\"quality\":99,\"rarity\":6,\"paint_kit\":38,\"paint_seed\":41,"
                + "\"paint_wear\":0.000001,\"equipped\":[{\"class_id\":3,\"slot_id\":0}]}";
    }

    private ResultActions postSkin(long account, String requestId, String items) throws Exception {
        return mvc.perform(post("/api/v1/matchmaking/skin-snapshot").header(KEY, "test-api-key")
                .contentType(MediaType.APPLICATION_JSON)
                .content("{\"account_id\":" + account + ",\"request_id\":\"" + requestId + "\",\"items\":[" + items + "]}"));
    }

    private JsonNode pollRoster() throws Exception {
        return body(mvc.perform(get(ROSTER).header(KEY, "test-api-key")).andExpect(status().isOk()));
    }

    private JsonNode snapshotOf(JsonNode roster, long account) {
        for (JsonNode s : roster.get("skin_snapshots")) {
            if (s.get("account_id").asLong() == account) {
                return s;
            }
        }
        return null;
    }

    // ------------------------------------------------------------------------------------------ Phase A + B + C

    /** client -> backend -> match -> the roster the game server polls: "player A -> item 2, def 507, paint 38" */
    @Test
    void theSnapshotOfARealPlayerReachesTheRosterOfItsMatch() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);
        mvc.perform(get(ROSTER).header(KEY, "test-api-key"));                 // srcds polls: no match yet

        search(REAL, COMPETITIVE, DUST2, "r1");
        JsonNode stored = body(postSkin(REAL, "r1", knife(2)).andExpect(status().isOk()));
        assertThat(stored.get("stored").asBoolean()).isTrue();
        assertThat(stored.get("items").asInt()).isEqualTo(1);

        JsonNode roster = pollRoster();
        assertThat(roster.get("players").size()).isEqualTo(10);               // matchmaking unchanged: 1 real + 9 fake
        assertThat(roster.get("skin_snapshots").size()).isEqualTo(1);          // the virtual players have no inventory
        JsonNode mine = snapshotOf(roster, REAL);
        assertThat(mine).isNotNull();
        assertThat(mine.get("steam_id64").asText()).isEqualTo("76561199010432725");
        JsonNode item = mine.get("items").get(0);
        assertThat(item.get("item_id").asLong()).isEqualTo(2);
        assertThat(item.get("def_index").asInt()).isEqualTo(507);
        assertThat(item.get("paint_kit").asInt()).isEqualTo(38);
        assertThat(item.get("paint_seed").asInt()).isEqualTo(41);
        assertThat(item.get("paint_wear").asDouble()).isEqualTo(0.000001);
        assertThat(item.get("equipped").get(0).get("class_id").asInt()).isEqualTo(3);
        assertThat(item.get("equipped").get(0).get("slot_id").asInt()).isEqualTo(0);
        assertThat(roster.get("skin_missing").size()).isEqualTo(0);
    }

    /** B + C + A: B and C sent theirs, the rest did not -> exactly one snapshot per player, the others are named as missing */
    @Test
    void aMatchCollectsTheSnapshotsOfItsPlayersAndNamesTheMissingOnes() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        for (int i = 1; i <= 10; i++) {
            search(i, COMPETITIVE, DUST2, "r" + i);
        }
        postSkin(2, "r2", knife(200)).andExpect(status().isOk());              // player B
        postSkin(3, "r3", knife(300) + "," + knife(301).replace("\"def_index\":507", "\"def_index\":7")).andExpect(status().isOk());

        JsonNode roster = pollRoster();
        assertThat(roster.get("players").size()).isEqualTo(10);
        assertThat(roster.get("skin_snapshots").size()).isEqualTo(2);
        assertThat(snapshotOf(roster, 2).get("items").size()).isEqualTo(1);
        assertThat(snapshotOf(roster, 2).get("items").get(0).get("item_id").asLong()).isEqualTo(200);
        assertThat(snapshotOf(roster, 3).get("items").size()).isEqualTo(2);
        assertThat(snapshotOf(roster, 3).get("items").get(1).get("def_index").asInt()).isEqualTo(7);
        assertThat(snapshotOf(roster, 1)).isNull();                            // a player that sent nothing has no entry...
        assertThat(roster.get("skin_missing").size()).isEqualTo(8);            // ...and is listed as missing, the match is fine
        assertThat(roster.get("skin_missing").toString()).contains("1").contains("10").doesNotContain("[2,").doesNotContain(",3,");
    }

    /** the last snapshot of a search wins (the player changed the loadout and searched again within the same request) */
    @Test
    void aNewerSnapshotReplacesTheOlderOne() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);
        mvc.perform(get(ROSTER).header(KEY, "test-api-key"));
        search(REAL, COMPETITIVE, DUST2, "r1");
        postSkin(REAL, "r1", knife(2)).andExpect(status().isOk());
        postSkin(REAL, "r1", knife(2).replace("\"paint_kit\":38", "\"paint_kit\":44")).andExpect(status().isOk());

        JsonNode roster = pollRoster();
        assertThat(roster.get("skin_snapshots").size()).isEqualTo(1);
        assertThat(snapshotOf(roster, REAL).get("items").get(0).get("paint_kit").asInt()).isEqualTo(44);
        assertThat(searchService.skinSnapshotCount()).isEqualTo(1);
    }

    // ------------------------------------------------------------------------------------------ authentication / validation

    /** player A posting a snapshot for player B is rejected */
    @Test
    void aSnapshotNeedsTheLiveSearchOfItsOwnerAndItsRequestId() throws Exception {
        search(1, COMPETITIVE, DUST2, "r-a");
        search(2, COMPETITIVE, DUST2, "r-b");

        postSkin(3, "r-c", knife(2)).andExpect(status().isNotFound());        // account 3 has no search at all
        postSkin(2, "r-a", knife(2)).andExpect(status().isConflict());         // A's request id used for B
        postSkin(1, "r-b", knife(2)).andExpect(status().isConflict());         // B's request id used for A
        postSkin(1, "r-old", knife(2)).andExpect(status().isConflict());       // stale: another search of the account
        assertThat(searchService.skinSnapshotCount()).isEqualTo(0);            // nothing of that was stored

        postSkin(1, "r-a", knife(2)).andExpect(status().isOk());
        assertThat(searchService.skinSnapshotCount()).isEqualTo(1);
    }

    @Test
    void theGcApiKeyIsRequired() throws Exception {
        search(1, COMPETITIVE, DUST2, "r-a");
        mvc.perform(post("/api/v1/matchmaking/skin-snapshot").contentType(MediaType.APPLICATION_JSON)
                .content("{\"account_id\":1,\"request_id\":\"r-a\",\"items\":[" + knife(2) + "]}"))
                .andExpect(status().isUnauthorized());
        assertThat(searchService.skinSnapshotCount()).isEqualTo(0);
    }

    @Test
    void aMalformedSnapshotIsRejectedAndNotStored() throws Exception {
        search(1, COMPETITIVE, DUST2, "r-a");
        postSkin(1, "r-a", knife(2).replace("\"paint_wear\":0.000001", "\"paint_wear\":2.0")).andExpect(status().isBadRequest());
        postSkin(1, "r-a", knife(2).replace("\"equipped\":[{\"class_id\":3,\"slot_id\":0}]", "\"equipped\":[]"))
                .andExpect(status().isBadRequest());                           // only equipped items are part of a snapshot
        postSkin(1, "r-a", knife(2).replace("\"def_index\":507", "\"def_index\":-1")).andExpect(status().isBadRequest());
        postSkin(1, "r-a", knife(2).replace("}]}", "}],\"custom_name\":\"a\\u0007b\"}")).andExpect(status().isBadRequest());
        postSkin(1, "r-a", "{\"item_id\":1}").andExpect(status().isBadRequest());
        StringBuilder many = new StringBuilder();
        for (int i = 1; i <= 65; i++) {
            many.append(i > 1 ? "," : "").append(knife(i));
        }
        postSkin(1, "r-a", many.toString()).andExpect(status().isBadRequest()); // more than 64 items
        mvc.perform(post("/api/v1/matchmaking/skin-snapshot").header(KEY, "test-api-key")
                .contentType(MediaType.APPLICATION_JSON).content("{\"account_id\":1}")).andExpect(status().isBadRequest());
        assertThat(searchService.skinSnapshotCount()).isEqualTo(0);

        // an empty snapshot is valid: nothing custom equipped (different from "did not send one")
        postSkin(1, "r-a", "").andExpect(status().isOk());
        assertThat(searchService.skinSnapshotCount()).isEqualTo(1);
    }

    // ------------------------------------------------------------------------------------------ cleanup

    @Test
    void theSnapshotIsRemovedWhenTheSearchIsCancelled() throws Exception {
        search(REAL, COMPETITIVE, DUST2, "r1");
        postSkin(REAL, "r1", knife(2)).andExpect(status().isOk());
        assertThat(searchService.skinSnapshotCount()).isEqualTo(1);
        cancel(REAL, "r1");
        assertThat(searchService.skinSnapshotCount()).isEqualTo(0);
    }

    @Test
    void theSnapshotIsRemovedWhenTheSearchExpires() throws Exception {
        search(REAL, COMPETITIVE, DUST2, "r1");
        postSkin(REAL, "r1", knife(2)).andExpect(status().isOk());
        pass(60);
        assertThat(searchService.skinSnapshotCount()).isEqualTo(1);            // a live search keeps it
        pass(300);                                                             // stale-search-timeout PT5M, nobody polled
        assertThat(searchService.skinSnapshotCount()).isEqualTo(0);
    }

    /** the match of the snapshot is on its server until it ends: the search is COMPLETED long before, the snapshot stays */
    @Test
    void theSnapshotStaysWhileItsMatchHoldsTheServerAndGoesWhenTheMatchEnds() throws Exception {
        addServer("192.168.1.150", 27016, "casual", "de_dust2");
        JsonNode casual = search(REAL, CASUAL, DUST2, "r1");
        assertThat(casual.get("status").asText()).isEqualTo("READY_TO_CONNECT");
        postSkin(REAL, "r1", knife(2)).andExpect(status().isOk());
        assertThat(searchService.skinSnapshotCount()).isEqualTo(1);

        pass(125);                                                             // assigned-search-timeout PT2M: the search is COMPLETED
        assertThat(poll("r1").get("status").asText()).isEqualTo("COMPLETED");
        assertThat(searchService.skinSnapshotCount()).isEqualTo(1);            // ...the match still holds the server

        pass(600);                                                             // server-reservation-ttl PT10M: the match ENDED
        assertThat(searchService.skinSnapshotCount()).isEqualTo(0);
    }

    /** reservation / Accept failure: the match is cancelled, the player searches again and the snapshot goes on with the search */
    @Test
    void anAcceptTimeoutKeepsTheSnapshotOfAPlayerThatSearchesAgainAndACancelRemovesIt() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);
        mvc.perform(get(ROSTER).header(KEY, "test-api-key"));
        search(REAL, COMPETITIVE, DUST2, "r1");
        postSkin(REAL, "r1", knife(2)).andExpect(status().isOk());
        String matchId = matches().get(0).get("id").asText();
        mvc.perform(post("/api/v1/servers/roster/ready").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                .content("{\"address\":\"192.168.1.150\",\"port\":27017,\"match_id\":\"" + matchId + "\"}")).andExpect(status().isOk());

        pass(30);                                                              // accept-timeout PT25S: nobody accepted
        assertThat(poll("r1").get("status").asText()).isIn("SEARCHING", "MATCHED"); // the same search, back in the queue
        assertThat(searchService.skinSnapshotCount()).isEqualTo(1);

        cancel(REAL, "r1");
        assertThat(searchService.skinSnapshotCount()).isEqualTo(0);
    }
}
