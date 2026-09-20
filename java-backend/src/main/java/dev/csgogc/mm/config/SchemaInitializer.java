package dev.csgogc.mm.config;

import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.HashSet;
import java.util.Set;
import javax.sql.DataSource;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.core.io.ClassPathResource;
import org.springframework.jdbc.datasource.init.ResourceDatabasePopulator;

/** Creates the tables (schema.sql) and adds the columns introduced after stage 1 to an existing database file. */
final class SchemaInitializer {

    private static final Logger log = LoggerFactory.getLogger(SchemaInitializer.class);

    private SchemaInitializer() {
    }

    static void apply(DataSource dataSource) {
        new ResourceDatabasePopulator(new ClassPathResource("schema.sql")).execute(dataSource);

        try (Connection connection = dataSource.getConnection()) {
            // game_server: state machine + heartbeat (stage 2)
            ensureColumn(connection, "game_server", "state", "TEXT NOT NULL DEFAULT 'AVAILABLE'");
            ensureColumn(connection, "game_server", "reserved_match_id", "TEXT");
            ensureColumn(connection, "game_server", "reserved_at", "INTEGER");
            ensureColumn(connection, "game_server", "last_assigned_at", "INTEGER");
            ensureColumn(connection, "game_server", "last_heartbeat_at", "INTEGER");
            ensureColumn(connection, "game_server", "max_players", "INTEGER");
            // matchmaking_search: the match a search was placed in
            ensureColumn(connection, "matchmaking_search", "match_id", "TEXT");
            ensureColumn(connection, "matchmaking_search", "matched_at", "INTEGER");
            // Skirmish: the modes the player selected (RESEARCH_FINDINGS.md #54)
            ensureColumn(connection, "matchmaking_search", "variants", "TEXT");
            // Accept lifecycle (RESEARCH_FINDINGS.md #63): when the player accepted, the deadline of the Accept phase of a
            // match, when it was accepted by everybody, and the moment a server that was released from a cancelled match
            // may be handed out again (the srcds side needs that long to drop the old reservation)
            ensureColumn(connection, "matchmaking_search", "accepted_at", "INTEGER");
            ensureColumn(connection, "matchmaking_match", "accept_deadline_at", "INTEGER");
            ensureColumn(connection, "matchmaking_match", "accepted_at", "INTEGER");
            ensureColumn(connection, "game_server", "available_after", "INTEGER");
        } catch (SQLException e) {
            throw new IllegalStateException("database migration failed", e);
        }
    }

    private static void ensureColumn(Connection connection, String table, String column, String definition)
            throws SQLException {
        Set<String> existing = new HashSet<>();
        try (Statement statement = connection.createStatement();
             ResultSet rs = statement.executeQuery("PRAGMA table_info(" + table + ")")) {
            while (rs.next()) {
                existing.add(rs.getString("name"));
            }
        }
        if (!existing.contains(column)) {
            try (Statement statement = connection.createStatement()) {
                statement.execute("ALTER TABLE " + table + " ADD COLUMN " + column + " " + definition);
            }
            log.info("database migrated: {}.{} added", table, column);
        }
    }
}
