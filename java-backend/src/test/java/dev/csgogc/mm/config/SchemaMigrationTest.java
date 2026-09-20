package dev.csgogc.mm.config;

import static org.assertj.core.api.Assertions.assertThat;

import com.zaxxer.hikari.HikariConfig;
import com.zaxxer.hikari.HikariDataSource;
import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.HashSet;
import java.util.Set;
import org.junit.jupiter.api.Test;

/** A database file created by stage 1 (the user's data\matchmaking.db) must keep its rows and work with stage 2. */
class SchemaMigrationTest {

    @Test
    void aStageOneDatabaseIsMigratedInPlace() throws Exception {
        Path dir = Files.createTempDirectory("mm-migration-test");
        String url = "jdbc:sqlite:" + dir.resolve("old.db").toString().replace('\\', '/');
        HikariConfig config = new HikariConfig();
        config.setJdbcUrl(url);
        config.setMaximumPoolSize(1);

        // the old file is written through its own connection, the pool (max 1) is only used by the migration
        try (Connection setup = DriverManager.getConnection(url); Statement statement = setup.createStatement()) {

            // the stage 1 schema, byte for byte what schema.sql created back then
            statement.execute("CREATE TABLE game_server (id INTEGER PRIMARY KEY AUTOINCREMENT, host TEXT NOT NULL, port INTEGER NOT NULL, "
                    + "category TEXT NOT NULL, map TEXT NOT NULL DEFAULT '', enabled INTEGER NOT NULL DEFAULT 1, "
                    + "created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, UNIQUE (host, port))");
            statement.execute("CREATE TABLE matchmaking_search (id INTEGER PRIMARY KEY AUTOINCREMENT, account_id INTEGER NOT NULL, "
                    + "game_type INTEGER NOT NULL, category TEXT NOT NULL, game_mode TEXT NOT NULL, maps TEXT NOT NULL DEFAULT '', "
                    + "request_id TEXT, status TEXT NOT NULL, source TEXT NOT NULL DEFAULT 'gc', started_at INTEGER NOT NULL, "
                    + "last_seen_at INTEGER NOT NULL, ended_at INTEGER)");
            statement.execute("CREATE UNIQUE INDEX ux_search_active_account ON matchmaking_search (account_id) WHERE status = 'SEARCHING'");
            statement.execute("INSERT INTO game_server (host, port, category, map, enabled, created_at, updated_at) "
                    + "VALUES ('192.168.1.150', 27016, 'casual', 'de_dust2', 1, 1, 1)");
            statement.execute("INSERT INTO matchmaking_search (account_id, game_type, category, game_mode, status, started_at, last_seen_at) "
                    + "VALUES (7, 519, 'casual', 'casual', 'SEARCHING', 1, 1)");

        }

        try (HikariDataSource dataSource = new HikariDataSource(config)) {
            SchemaInitializer.apply(dataSource);
            SchemaInitializer.apply(dataSource);   // and it is idempotent
        }

        try (Connection check = DriverManager.getConnection(url); Statement statement = check.createStatement()) {

            assertThat(columns(statement, "game_server")).contains("state", "reserved_match_id", "reserved_at",
                    "last_assigned_at", "last_heartbeat_at", "max_players", "available_after");
            assertThat(columns(statement, "matchmaking_search")).contains("match_id", "matched_at", "variants", "accepted_at");
            assertThat(columns(statement, "fake_search")).contains("category", "players", "maps", "enabled", "status", "match_id");
            assertThat(columns(statement, "matchmaking_match")).contains("server_id", "required_players", "status",
                    "accept_deadline_at", "accepted_at");   // the Accept lifecycle of #63

            try (ResultSet rs = statement.executeQuery("SELECT host, port, state FROM game_server")) {
                assertThat(rs.next()).isTrue();
                assertThat(rs.getString("host")).isEqualTo("192.168.1.150");
                assertThat(rs.getString("state")).isEqualTo("AVAILABLE");   // the default for old rows
            }
            try (ResultSet rs = statement.executeQuery("SELECT status FROM matchmaking_search WHERE account_id = 7")) {
                assertThat(rs.next()).isTrue();
                assertThat(rs.getString("status")).isEqualTo("SEARCHING");
            }

            // the old unique index is gone, the new one covers every live status
            Set<String> indexes = new HashSet<>();
            try (ResultSet rs = statement.executeQuery("PRAGMA index_list(matchmaking_search)")) {
                while (rs.next()) {
                    indexes.add(rs.getString("name"));
                }
            }
            assertThat(indexes).contains("ux_search_live_account").doesNotContain("ux_search_active_account");
        }
    }

    private static Set<String> columns(Statement statement, String table) throws Exception {
        Set<String> names = new HashSet<>();
        try (ResultSet rs = statement.executeQuery("PRAGMA table_info(" + table + ")")) {
            while (rs.next()) {
                names.add(rs.getString("name"));
            }
        }
        return names;
    }
}
