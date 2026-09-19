package dev.csgogc.mm;

import static org.assertj.core.api.Assertions.assertThat;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasterxml.jackson.databind.JsonNode;
import java.nio.file.Path;
import java.util.List;
import java.util.stream.Collectors;
import org.junit.jupiter.api.Test;
import org.springframework.http.MediaType;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;

/**
 * Map groups end to end on the backend side (RESEARCH_FINDINGS.md #54): the GC decodes the client's game_type into
 * maps[] (per map group of gamemodes.txt) or, for Skirmish, into one variant per selected skirmish mode. A search may
 * only get a server of ITS category whose map belongs to what the player selected - never one of another group/mode.
 * The map lists below are the groups of csgo/gamemodes.txt (test data, not used by the backend).
 */
class MapGroupMatchingTest extends BackendTestBase {

    private static final Path DB_DIR = tempDir("mm-backend-mapgroup-test");

    @DynamicPropertySource
    static void database(DynamicPropertyRegistry registry) {
        registry.add("backend.database-path", () -> DB_DIR.resolve("mm.db").toString());
    }

    // mapgroupsMP of casual / deathmatch
    private static final List<String> SIGMA = List.of("de_basalt", "de_ancient", "de_vertigo", "de_cbble", "de_canals");
    private static final List<String> DELTA = List.of("de_mirage", "de_inferno", "de_overpass", "de_nuke", "de_train", "de_cache");
    private static final List<String> HOSTAGE = List.of("cs_insertion2", "cs_agency", "cs_militia", "cs_office", "cs_italy", "cs_assault");
    private static final List<String> DUST247 = List.of("de_dust2");
    // mg_skirmish_armsrace / mg_skirmish_demolition / mg_skirmish_retakes
    private static final List<String> SKIRMISH_ARMSRACE =
            List.of("de_lake", "ar_baggage", "de_safehouse", "de_stmarc", "ar_shoots", "ar_lunacy", "ar_monastery");
    private static final List<String> SKIRMISH_DEMOLITION =
            List.of("de_lake", "de_safehouse", "de_sugarcane", "de_bank", "de_stmarc", "de_shortdust");
    private static final List<String> SKIRMISH_RETAKES =
            List.of("de_inferno", "de_mirage", "de_dust2", "de_nuke", "de_overpass", "de_train", "de_vertigo", "de_ancient");

    /** game_type of the live log: Deathmatch, the Sigma group (mask 0x640048) */
    private static final long DEATHMATCH_SIGMA = 1677740038L;
    /** Skirmish with Arms Race (id 10 -> 0x200) and Demolition (id 11 -> 0x400) ticked */
    private static final long SKIRMISH_AR_DEMO = 12L | (0x600L << 8);

    private static String maps(List<String> maps) {
        return maps.stream().map(m -> "\"" + m + "\"").collect(Collectors.joining(","));
    }

    private static String variant(String name, String gameMode, List<String> maps) {
        return "{\"name\":\"" + name + "\",\"game_mode\":\"" + gameMode + "\",\"maps\":[" + maps(maps) + "]}";
    }

    private static String variants(String... items) {
        return "[" + String.join(",", items) + "]";
    }

    // ------------------------------------------------------------------------------------------ casual / deathmatch groups

    @Test
    void everyCasualMapGroupGetsOnlyAServerOnOneOfItsOwnMaps() throws Exception {
        addServer("10.0.0.5", 27001, "casual", "de_dust2");
        addServer("10.0.0.5", 27002, "casual", "de_inferno");    // delta
        addServer("10.0.0.5", 27003, "casual", "cs_office");     // hostage
        addServer("10.0.0.5", 27004, "casual", "de_ancient");    // sigma

        assertThat(search(1, CASUAL, maps(SIGMA), "sigma").get("assignment").get("map").asText()).isEqualTo("de_ancient");
        assertThat(search(2, CASUAL, maps(DELTA), "delta").get("assignment").get("map").asText()).isEqualTo("de_inferno");
        assertThat(search(3, CASUAL, maps(HOSTAGE), "hostage").get("assignment").get("map").asText()).isEqualTo("cs_office");
        assertThat(search(4, CASUAL, maps(DUST247), "dust").get("assignment").get("map").asText()).isEqualTo("de_dust2");
    }

    @Test
    void aGroupWithoutAServerOnAnyOfItsMapsKeepsSearchingInsteadOfGettingAnotherGroupsServer() throws Exception {
        addServer("10.0.0.5", 27001, "casual", "de_dust2");
        addServer("10.0.0.5", 27002, "casual", "de_inferno");    // delta
        addServer("10.0.0.5", 27003, "casual", "cs_office");     // hostage

        JsonNode sigma = search(1, CASUAL, maps(SIGMA), "sigma");
        assertThat(sigma.get("status").asText()).isEqualTo("SEARCHING");
        assertThat(sigma.hasNonNull("assignment")).isFalse();

        addServer("10.0.0.5", 27004, "casual", "de_vertigo");    // a Sigma map: the waiting search takes it
        JsonNode matched = poll("sigma");
        assertThat(matched.get("status").asText()).isEqualTo("READY_TO_CONNECT");
        assertThat(matched.get("assignment").get("map").asText()).isEqualTo("de_vertigo");
    }

    @Test
    void theModeCategoryIsPartOfTheMatchAsWellAsTheMapGroup() throws Exception {
        long casualSigma = addServer("10.0.0.5", 27001, "casual", "de_ancient");
        // the real game_type of a Deathmatch / Sigma search: eGame 6 with map mask 0x640048
        JsonNode dm = search(1, DEATHMATCH_SIGMA, maps(SIGMA), "dm");
        assertThat(dm.get("e_game").asInt()).isEqualTo(6);
        assertThat(dm.get("mode").asText()).isEqualTo("deathmatch");
        assertThat(dm.get("status").asText()).isEqualTo("SEARCHING");    // a Casual server on a Sigma map is not a DM server

        long dmSigma = addServer("10.0.0.5", 27002, "deathmatch", "de_canals");
        assertThat(poll("dm").get("assignment").get("server_id").asLong()).isEqualTo(dmSigma);
        assertThat(casualSigma).isNotEqualTo(dmSigma);
        assertThat(server(casualSigma).get("state").asText()).isEqualTo("AVAILABLE");
    }

    @Test
    void demolitionAndSigmaAreDifferentGroupsOfDifferentModes() throws Exception {
        addServer("10.0.0.5", 27001, "demolition", "de_lake");   // Demolition server: never a Casual / Sigma target
        JsonNode sigma = search(1, CASUAL, maps(SIGMA), "sigma");
        assertThat(sigma.get("status").asText()).isEqualTo("SEARCHING");

        addServer("10.0.0.5", 27002, "casual", "de_basalt");
        assertThat(poll("sigma").get("assignment").get("server_port").asInt()).isEqualTo(27002);
    }

    @Test
    void aSearchWithSeveralGroupsAcceptsAServerOnAnyOfTheirMaps() throws Exception {
        addServer("10.0.0.5", 27001, "casual", "de_nuke");       // delta
        List<String> sigmaAndDelta = new java.util.ArrayList<>(SIGMA);
        sigmaAndDelta.addAll(DELTA);
        JsonNode both = search(1, CASUAL, maps(sigmaAndDelta), "both");
        assertThat(both.get("assignment").get("map").asText()).isEqualTo("de_nuke");
        assertThat(search(2, CASUAL, maps(SIGMA), "sigma").get("status").asText()).isEqualTo("SEARCHING");
    }

    // ------------------------------------------------------------------------------------------ skirmish ("War Games")

    @Test
    void aSkirmishSearchCarriesTheSelectedModesAndEachIsServedByItsOwnCategoryAndMapGroup() throws Exception {
        long armsRace = addServer("10.0.0.5", 27001, "armsrace", "ar_shoots");
        long demolition = addServer("10.0.0.5", 27002, "demolition", "de_bank");
        long skirmish = addServer("10.0.0.5", 27003, "skirmish", "de_dust2");

        String both = variants(variant("armsrace", "gungameprogressive", SKIRMISH_ARMSRACE),
                variant("demolition", "gungametrbomb", SKIRMISH_DEMOLITION));
        JsonNode first = searchRaw(1, SKIRMISH_AR_DEMO, null, both, "s1");
        assertThat(first.get("e_game").asInt()).isEqualTo(12);
        assertThat(first.get("mode").asText()).isEqualTo("skirmish");
        assertThat(first.get("variants").size()).isEqualTo(2);
        assertThat(first.get("variants").get(0).get("category").asText()).isEqualTo("armsrace");
        assertThat(first.get("variants").get(1).get("category").asText()).isEqualTo("demolition");
        assertThat(first.get("status").asText()).isEqualTo("READY_TO_CONNECT");
        long firstServer = first.get("assignment").get("server_id").asLong();
        assertThat(List.of(armsRace, demolition)).contains(firstServer);
        assertThat(firstServer).isNotEqualTo(skirmish);          // the generic Skirmish category is not a target here

        // the second player goes to the other mode's server (least recently assigned first)
        long secondServer = searchRaw(2, SKIRMISH_AR_DEMO, null, both, "s2").get("assignment").get("server_id").asLong();
        assertThat(secondServer).isNotEqualTo(firstServer);
        assertThat(List.of(armsRace, demolition)).contains(secondServer);
    }

    @Test
    void aSkirmishModeNeedsAServerOnAMapOfItsOwnGroup() throws Exception {
        addServer("10.0.0.5", 27001, "armsrace", "de_bank");     // a Demolition map, not an Arms Race one
        String armsRaceOnly = variants(variant("armsrace", "gungameprogressive", SKIRMISH_ARMSRACE));
        JsonNode search = searchRaw(1, 12L | (0x200L << 8), null, armsRaceOnly, "s1");
        assertThat(search.get("status").asText()).isEqualTo("SEARCHING");

        addServer("10.0.0.5", 27002, "armsrace", "ar_baggage");
        assertThat(poll("s1").get("assignment").get("server_port").asInt()).isEqualTo(27002);
    }

    @Test
    void demolitionAloneNeverGetsAnArmsRaceServer() throws Exception {
        addServer("10.0.0.5", 27001, "armsrace", "de_lake");     // de_lake is in both groups, the category decides
        String demolition = variants(variant("demolition", "gungametrbomb", SKIRMISH_DEMOLITION));
        JsonNode search = searchRaw(1, 12L | (0x400L << 8), null, demolition, "s1");
        assertThat(search.get("status").asText()).isEqualTo("SEARCHING");

        addServer("10.0.0.5", 27002, "demolition", "de_lake");
        assertThat(poll("s1").get("assignment").get("server_port").asInt()).isEqualTo(27002);
    }

    @Test
    void theOtherSkirmishModesAreServedByTheSkirmishCategory() throws Exception {
        addServer("10.0.0.5", 27001, "armsrace", "de_dust2");
        addServer("10.0.0.5", 27002, "skirmish", "de_lake");     // not a Retakes map
        String retakes = variants(variant("retakes", "casual", SKIRMISH_RETAKES));
        JsonNode search = searchRaw(1, 12L | (0x800L << 8), null, retakes, "s1");
        assertThat(search.get("status").asText()).isEqualTo("SEARCHING");
        assertThat(search.get("variants").get(0).get("category").asText()).isEqualTo("skirmish");

        addServer("10.0.0.5", 27003, "skirmish", "de_dust2");
        assertThat(poll("s1").get("assignment").get("server_port").asInt()).isEqualTo(27003);
    }

    @Test
    void aSkirmishSearchWithoutVariantsStillFitsAnySkirmishServer() throws Exception {
        addServer("10.0.0.5", 27001, "armsrace", "ar_shoots");
        addServer("10.0.0.5", 27002, "skirmish", "de_dust2");
        JsonNode search = search(1, SKIRMISH, null, "s1");        // an older GC that does not decode the mask
        assertThat(search.get("assignment").get("server_port").asInt()).isEqualTo(27002);
    }

    @Test
    void variantsExistForSkirmishOnly() throws Exception {
        String body = "{\"account_id\":1,\"game_type\":8,\"request_id\":\"x\",\"variants\":"
                + variants(variant("armsrace", "gungameprogressive", SKIRMISH_ARMSRACE)) + "}";
        mvc.perform(post("/api/v1/matchmaking/search").header(KEY, "test-api-key").contentType(MediaType.APPLICATION_JSON)
                        .content(body))
                .andExpect(status().isBadRequest())
                .andExpect(jsonPath("$.error").value("variants_not_allowed"));
    }

    @Test
    void changingTheSelectedSkirmishModesReplacesTheSearch() throws Exception {
        String ar = variants(variant("armsrace", "gungameprogressive", SKIRMISH_ARMSRACE));
        String demo = variants(variant("demolition", "gungametrbomb", SKIRMISH_DEMOLITION));
        JsonNode first = searchRaw(1, 12L | (0x200L << 8), null, ar, "a");
        assertThat(first.get("variants").get(0).get("name").asText()).isEqualTo("armsrace");
        // same account, a different selection (new request id): the live search is rewritten, not duplicated
        JsonNode second = searchRaw(1, 12L | (0x400L << 8), null, demo, "b");
        assertThat(second.get("id").asLong()).isEqualTo(first.get("id").asLong());
        assertThat(second.get("variants").get(0).get("name").asText()).isEqualTo("demolition");
        assertThat(second.get("variants").size()).isEqualTo(1);
    }
}
