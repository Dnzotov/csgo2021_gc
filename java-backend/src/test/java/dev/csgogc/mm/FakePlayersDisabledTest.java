package dev.csgogc.mm;

import static org.assertj.core.api.Assertions.assertThat;
import static org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.csrf;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasterxml.jackson.databind.JsonNode;
import java.nio.file.Path;
import org.junit.jupiter.api.Test;
import org.springframework.http.MediaType;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;
import org.springframework.test.context.TestPropertySource;

/** backend.fake-players.enabled=false (production): the tool is inert, the matcher never looks at fake searches. */
@TestPropertySource(properties = "backend.fake-players.enabled=false")
class FakePlayersDisabledTest extends BackendTestBase {

    private static final Path DB_DIR = tempDir("mm-backend-fake-off-test");

    @DynamicPropertySource
    static void database(DynamicPropertyRegistry registry) {
        registry.add("backend.database-path", () -> DB_DIR.resolve("mm.db").toString());
    }

    @Test
    void theSwitchRefusesNewFakeSearchesAndTheMatcherIgnoresExistingOnes() throws Exception {
        addServer("10.0.0.5", 27018, "wingman", "de_lake");
        // a fake search left in the database from a run with the tool switched on
        jdbc.sql("INSERT INTO fake_search (category, players, maps, enabled, status, created_at, updated_at) "
                + "VALUES ('wingman', 3, '', 1, 'SEARCHING', 0, 0)").update();

        JsonNode list = body(mvc.perform(get("/admin/api/fake-searches").with(ADMIN)).andExpect(status().isOk()));
        assertThat(list.get("enabled").asBoolean()).isFalse();

        mvc.perform(post("/admin/api/fake-searches").with(ADMIN).with(csrf()).contentType(MediaType.APPLICATION_JSON)
                .content("{\"mode\":\"wingman\",\"players\":3}")).andExpect(status().isConflict());

        // the real player is not filled up: 1/4 and waiting, exactly like production without fake players
        JsonNode real = search(1, WINGMAN, "\"de_lake\"", "w");
        assertThat(real.get("status").asText()).isEqualTo("MATCHED");
        assertThat(real.get("match").get("players").asInt()).isEqualTo(1);
        assertThat(real.get("match").get("fake_players").asInt()).isZero();
    }
}
