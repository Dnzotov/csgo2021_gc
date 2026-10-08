package dev.csgogc.mm;

import static org.assertj.core.api.Assertions.assertThat;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasterxml.jackson.databind.JsonNode;
import dev.csgogc.mm.equipment.EquipmentSnapshotStore;
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
 * Equipment Sync: the GC of a player posts its EquipmentSnapshot (the base weapons it picked, SO type 43 data), the backend
 * keeps it as temporary matchmaking state next to (and independent of) the skin snapshot, attaches the snapshots of the real
 * players of a match to the roster the game server polls (GET /servers/roster -> equipment_snapshots / equipment_missing)
 * and drops them when the search / match is over. Same lifecycle rules as SkinSnapshotTest.
 */
class EquipmentSnapshotTest extends BackendTestBase {

    private static final Path DB_DIR = tempDir("mm-backend-equipment-test");

    @DynamicPropertySource
    static void database(DynamicPropertyRegistry registry) {
        registry.add("backend.database-path", () -> DB_DIR.resolve("mm.db").toString());
    }

    @Autowired SearchService searchService;
    @Autowired EquipmentSnapshotStore equipmentStore;
    @Autowired SkinSnapshotStore skinStore;

    @BeforeEach
    void emptyStores() {
        equipmentStore.all().forEach(stored -> equipmentStore.remove(stored.accountId()));
        skinStore.all().forEach(stored -> skinStore.remove(stored.accountId()));
    }

    private static final long REAL = 1050166997L;
    private static final String DUST2 = "\"de_dust2\"";
    private static final String ROSTER = "/api/v1/servers/roster?address=192.168.1.150&port=27017";

    private static String entry(int def, int classId, int slot) {
        return "{\"item_definition\":" + def + ",\"class_id\":" + classId + ",\"slot_id\":" + slot + "}";
    }

    private ResultActions postEquipment(long account, String requestId, String entries) throws Exception {
        return mvc.perform(post("/api/v1/matchmaking/equipment-snapshot").header(KEY, "test-api-key")
                .contentType(MediaType.APPLICATION_JSON)
                .content("{\"account_id\":" + account + ",\"request_id\":\"" + requestId + "\",\"entries\":[" + entries + "]}"));
    }

    private JsonNode pollRoster() throws Exception {
        return body(mvc.perform(get(ROSTER).header(KEY, "test-api-key")).andExpect(status().isOk()));
    }

    private JsonNode snapshotOf(JsonNode roster, long account) {
        for (JsonNode s : roster.get("equipment_snapshots")) {
            if (s.get("account_id").asLong() == account) {
                return s;
            }
        }
        return null;
    }

    // ------------------------------------------------------------------------------------------ roster aggregation

    @Test
    void theEquipmentOfARealPlayerReachesTheRosterOfItsMatch() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);
        mvc.perform(get(ROSTER).header(KEY, "test-api-key"));

        search(REAL, COMPETITIVE, DUST2, "r1");
        JsonNode stored = body(postEquipment(REAL, "r1", entry(61, 3, 2) + "," + entry(60, 3, 15) + "," + entry(4, 2, 2))
                .andExpect(status().isOk()));
        assertThat(stored.get("stored").asBoolean()).isTrue();
        assertThat(stored.get("entries").asInt()).isEqualTo(3);

        JsonNode roster = pollRoster();
        assertThat(roster.get("players").size()).isEqualTo(10);                 // matchmaking unchanged
        assertThat(roster.get("equipment_snapshots").size()).isEqualTo(1);       // virtual players have no loadout
        JsonNode mine = snapshotOf(roster, REAL);
        assertThat(mine).isNotNull();
        assertThat(mine.get("steam_id64").asText()).isEqualTo("76561199010432725");
        assertThat(mine.get("entries").size()).isEqualTo(3);
        assertThat(mine.get("entries").get(0).get("item_definition").asInt()).isEqualTo(61);
        assertThat(mine.get("entries").get(0).get("class_id").asInt()).isEqualTo(3);
        assertThat(mine.get("entries").get(0).get("slot_id").asInt()).isEqualTo(2);
        assertThat(roster.get("equipment_missing").size()).isEqualTo(0);
        assertThat(roster.get("skin_snapshots").size()).isEqualTo(0);            // the skin side is independent
    }

    /** two players with different picks: every account gets exactly its own entries, the rest is named as missing */
    @Test
    void everyPlayerKeepsItsOwnEquipmentAndTheMissingOnesAreNamed() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        for (int i = 1; i <= 10; i++) {
            search(i, COMPETITIVE, DUST2, "r" + i);
        }
        postEquipment(2, "r2", entry(61, 3, 2)).andExpect(status().isOk());               // A: USP-S
        postEquipment(3, "r3", entry(32, 3, 2) + "," + entry(16, 3, 15)).andExpect(status().isOk()); // B: P2000 + M4A4

        JsonNode roster = pollRoster();
        assertThat(roster.get("equipment_snapshots").size()).isEqualTo(2);
        assertThat(snapshotOf(roster, 2).get("entries").size()).isEqualTo(1);
        assertThat(snapshotOf(roster, 2).get("entries").get(0).get("item_definition").asInt()).isEqualTo(61);
        assertThat(snapshotOf(roster, 3).get("entries").size()).isEqualTo(2);
        assertThat(snapshotOf(roster, 3).get("entries").get(0).get("item_definition").asInt()).isEqualTo(32);
        assertThat(snapshotOf(roster, 3).get("entries").get(1).get("item_definition").asInt()).isEqualTo(16);
        assertThat(snapshotOf(roster, 1)).isNull();
        assertThat(roster.get("equipment_missing").size()).isEqualTo(8);
        assertThat(roster.get("equipment_missing").toString()).contains("1").contains("10").doesNotContain("[2,").doesNotContain(",3,");
    }

    @Test
    void aNewerSnapshotReplacesTheOlderOne() throws Exception {
        addServer("192.168.1.150", 27017, "competitive", "de_dust2");
        addFake("competitive", 9, DUST2);
        mvc.perform(get(ROSTER).header(KEY, "test-api-key"));
        search(REAL, COMPETITIVE, DUST2, "r1");
        postEquipment(REAL, "r1", entry(61, 3, 2)).andExpect(status().isOk());
        postEquipment(REAL, "r1", entry(32, 3, 2)).andExpect(status().isOk());

        JsonNode roster = pollRoster();
        assertThat(roster.get("equipment_snapshots").size()).isEqualTo(1);
        assertThat(snapshotOf(roster, REAL).get("entries").get(0).get("item_definition").asInt()).isEqualTo(32);
        assertThat(searchService.equipmentSnapshotCount()).isEqualTo(1);
    }

    // ------------------------------------------------------------------------------------------ authentication / validation

    @Test
    void aSnapshotNeedsTheLiveSearchOfItsOwnerAndItsRequestId() throws Exception {
        search(1, COMPETITIVE, DUST2, "r-a");
        search(2, COMPETITIVE, DUST2, "r-b");

        postEquipment(3, "r-c", entry(61, 3, 2)).andExpect(status().isNotFound());
        postEquipment(2, "r-a", entry(61, 3, 2)).andExpect(status().isConflict());     // A's request id used for B
        postEquipment(1, "r-b", entry(61, 3, 2)).andExpect(status().isConflict());     // B's request id used for A
        postEquipment(1, "r-old", entry(61, 3, 2)).andExpect(status().isConflict());   // stale
        assertThat(searchService.equipmentSnapshotCount()).isEqualTo(0);

        postEquipment(1, "r-a", entry(61, 3, 2)).andExpect(status().isOk());
        assertThat(searchService.equipmentSnapshotCount()).isEqualTo(1);
    }

    @Test
    void theGcApiKeyIsRequired() throws Exception {
        search(1, COMPETITIVE, DUST2, "r-a");
        mvc.perform(post("/api/v1/matchmaking/equipment-snapshot").contentType(MediaType.APPLICATION_JSON)
                .content("{\"account_id\":1,\"request_id\":\"r-a\",\"entries\":[" + entry(61, 3, 2) + "]}"))
                .andExpect(status().isUnauthorized());
        assertThat(searchService.equipmentSnapshotCount()).isEqualTo(0);
    }

    @Test
    void aMalformedSnapshotIsRejectedAndNotStored() throws Exception {
        search(1, COMPETITIVE, DUST2, "r-a");
        postEquipment(1, "r-a", entry(61, 0, 2)).andExpect(status().isBadRequest());     // class 0
        postEquipment(1, "r-a", entry(61, 4, 2)).andExpect(status().isBadRequest());     // class 4
        postEquipment(1, "r-a", entry(61, 3, 0)).andExpect(status().isBadRequest());     // melee slot
        postEquipment(1, "r-a", entry(61, 3, 8)).andExpect(status().isBadRequest());     // between the ranges
        postEquipment(1, "r-a", entry(61, 3, 20)).andExpect(status().isBadRequest());    // beyond the rifles
        postEquipment(1, "r-a", entry(0, 3, 2)).andExpect(status().isBadRequest());      // no definition
        postEquipment(1, "r-a", entry(-1, 3, 2)).andExpect(status().isBadRequest());
        postEquipment(1, "r-a", entry(61, 3, 2) + "," + entry(32, 3, 2)).andExpect(status().isBadRequest()); // duplicate (class, slot)
        postEquipment(1, "r-a", "{\"item_definition\":61}").andExpect(status().isBadRequest());
        StringBuilder many = new StringBuilder();
        for (int i = 0; i < 25; i++) {
            many.append(i > 0 ? "," : "").append(entry(61, 3, 2 + (i % 6)));
        }
        postEquipment(1, "r-a", many.toString()).andExpect(status().isBadRequest());     // more than 24 entries
        mvc.perform(post("/api/v1/matchmaking/equipment-snapshot").header(KEY, "test-api-key")
                .contentType(MediaType.APPLICATION_JSON).content("{\"account_id\":1}")).andExpect(status().isBadRequest());
        assertThat(searchService.equipmentSnapshotCount()).isEqualTo(0);

        // the same weapon in the same slot for the other team is fine; an empty snapshot is valid (nothing chosen)
        postEquipment(1, "r-a", entry(61, 3, 2) + "," + entry(4, 2, 2)).andExpect(status().isOk());
        postEquipment(1, "r-a", "").andExpect(status().isOk());
        assertThat(searchService.equipmentSnapshotCount()).isEqualTo(1);
    }

    // ------------------------------------------------------------------------------------------ cleanup

    @Test
    void theSnapshotIsRemovedWhenTheSearchIsCancelled() throws Exception {
        search(REAL, COMPETITIVE, DUST2, "r1");
        postEquipment(REAL, "r1", entry(61, 3, 2)).andExpect(status().isOk());
        assertThat(searchService.equipmentSnapshotCount()).isEqualTo(1);
        cancel(REAL, "r1");
        assertThat(searchService.equipmentSnapshotCount()).isEqualTo(0);
    }

    @Test
    void theSnapshotIsRemovedWhenTheSearchExpires() throws Exception {
        search(REAL, COMPETITIVE, DUST2, "r1");
        postEquipment(REAL, "r1", entry(61, 3, 2)).andExpect(status().isOk());
        pass(60);
        assertThat(searchService.equipmentSnapshotCount()).isEqualTo(1);
        pass(300);
        assertThat(searchService.equipmentSnapshotCount()).isEqualTo(0);
    }

    @Test
    void theSnapshotStaysWhileItsMatchHoldsTheServerAndGoesWhenTheMatchEnds() throws Exception {
        addServer("192.168.1.150", 27016, "casual", "de_dust2");
        JsonNode casual = search(REAL, CASUAL, DUST2, "r1");
        assertThat(casual.get("status").asText()).isEqualTo("READY_TO_CONNECT");
        postEquipment(REAL, "r1", entry(61, 3, 2)).andExpect(status().isOk());

        pass(125);
        assertThat(poll("r1").get("status").asText()).isEqualTo("COMPLETED");
        assertThat(searchService.equipmentSnapshotCount()).isEqualTo(1);

        pass(600);
        assertThat(searchService.equipmentSnapshotCount()).isEqualTo(0);
    }
}
