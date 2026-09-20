package dev.csgogc.mm.server;

import dev.csgogc.mm.mode.ModeCategory;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.time.Instant;
import java.util.List;
import java.util.Optional;
import org.springframework.jdbc.core.simple.JdbcClient;
import org.springframework.jdbc.support.GeneratedKeyHolder;
import org.springframework.jdbc.support.KeyHolder;
import org.springframework.stereotype.Repository;

@Repository
public class GameServerRepository {

    private static final String COLUMNS = "id, host, port, category, map, enabled, state, reserved_match_id, reserved_at, "
            + "last_assigned_at, last_heartbeat_at, max_players, created_at, updated_at, available_after";

    private final JdbcClient jdbc;

    public GameServerRepository(JdbcClient jdbc) {
        this.jdbc = jdbc;
    }

    public List<GameServer> list() {
        return jdbc.sql("SELECT " + COLUMNS + " FROM game_server ORDER BY category, host, port")
                .query(GameServerRepository::map)
                .list();
    }

    public Optional<GameServer> findById(long id) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM game_server WHERE id = ?")
                .param(id)
                .query(GameServerRepository::map)
                .optional();
    }

    public Optional<GameServer> findByHostPort(String host, int port) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM game_server WHERE host = ? AND port = ?")
                .params(host, port)
                .query(GameServerRepository::map)
                .optional();
    }

    /**
     * enabled and AVAILABLE servers of a category that may be handed out at {@code now}, the least recently assigned first
     * (spreads the load)
     */
    public List<GameServer> findFree(String category, Instant now) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM game_server WHERE enabled = 1 AND state = 'AVAILABLE' AND category = ? "
                        + "AND (available_after IS NULL OR available_after <= ?) ORDER BY COALESCE(last_assigned_at, 0), id")
                .params(category, now.toEpochMilli())
                .query(GameServerRepository::map)
                .list();
    }

    /** servers that are RESERVED since before the cutoff (the matchmaker checks that a live match still holds each of them) */
    public List<GameServer> findReservedBefore(Instant cutoff) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM game_server WHERE state = 'RESERVED' AND reserved_at < ?")
                .param(cutoff.toEpochMilli())
                .query(GameServerRepository::map)
                .list();
    }

    public long insert(String host, int port, String category, String map, boolean enabled, String state,
                       Integer maxPlayers, Instant now) {
        KeyHolder keys = new GeneratedKeyHolder();
        jdbc.sql("INSERT INTO game_server (host, port, category, map, enabled, state, max_players, created_at, updated_at) "
                        + "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)")
                .params(host, port, category, map, enabled ? 1 : 0, state, maxPlayers, now.toEpochMilli(), now.toEpochMilli())
                .update(keys);
        return keys.getKey().longValue();
    }

    /** an edit never touches state / reservation */
    public boolean update(long id, String host, int port, String category, String map, boolean enabled,
                          Integer maxPlayers, Instant now) {
        return jdbc.sql("UPDATE game_server SET host = ?, port = ?, category = ?, map = ?, enabled = ?, max_players = ?, "
                        + "updated_at = ? WHERE id = ?")
                .params(host, port, category, map, enabled ? 1 : 0, maxPlayers, now.toEpochMilli(), id)
                .update() > 0;
    }

    public boolean setEnabled(long id, boolean enabled, Instant now) {
        return jdbc.sql("UPDATE game_server SET enabled = ?, updated_at = ? WHERE id = ?")
                .params(enabled ? 1 : 0, now.toEpochMilli(), id)
                .update() > 0;
    }

    /** AVAILABLE -> RESERVED for a match (only if it is still AVAILABLE: two searches never get the same server) */
    public boolean reserve(long id, String matchId, Instant now) {
        return jdbc.sql("UPDATE game_server SET state = 'RESERVED', reserved_match_id = ?, reserved_at = ?, "
                        + "last_assigned_at = ?, updated_at = ?, available_after = NULL WHERE id = ? AND state = 'AVAILABLE'")
                .params(matchId, now.toEpochMilli(), now.toEpochMilli(), now.toEpochMilli(), id)
                .update() > 0;
    }

    /**
     * a classic mode (Casual, Deathmatch, Arms Race, Demolition, Skirmish) was pointed at the server: only the load
     * balancing clock moves, the state stays as it is (no reservation, other players can be sent to the same server)
     */
    public void markAssigned(long id, Instant now) {
        jdbc.sql("UPDATE game_server SET last_assigned_at = ?, updated_at = ? WHERE id = ?")
                .params(now.toEpochMilli(), now.toEpochMilli(), id)
                .update();
    }

    /** sets AVAILABLE / BUSY and drops any reservation (and any cooldown) */
    public boolean setState(long id, String state, Instant now) {
        return jdbc.sql("UPDATE game_server SET state = ?, reserved_match_id = NULL, reserved_at = NULL, available_after = NULL, "
                        + "updated_at = ? WHERE id = ?")
                .params(state, now.toEpochMilli(), id)
                .update() > 0;
    }

    /**
     * a reserved server goes back to AVAILABLE but is not handed out before {@code availableAfter} (a cancelled Accept
     * match: its game server still holds the old reservation for a moment). Only if the given match still holds it.
     */
    public boolean release(long id, String matchId, Instant availableAfter, Instant now) {
        return jdbc.sql("UPDATE game_server SET state = 'AVAILABLE', reserved_match_id = NULL, reserved_at = NULL, "
                        + "available_after = ?, updated_at = ? WHERE id = ? AND reserved_match_id = ?")
                .params(availableAfter == null ? null : availableAfter.toEpochMilli(), now.toEpochMilli(), id, matchId)
                .update() > 0;
    }

    /** what a server reports about itself (POST /api/v1/servers/state) */
    public boolean heartbeat(long id, String map, Instant now) {
        return jdbc.sql("UPDATE game_server SET last_heartbeat_at = ?, map = COALESCE(?, map), updated_at = ? WHERE id = ?")
                .params(now.toEpochMilli(), map, now.toEpochMilli(), id)
                .update() > 0;
    }

    public boolean delete(long id) {
        return jdbc.sql("DELETE FROM game_server WHERE id = ?").param(id).update() > 0;
    }

    private static Instant instantOrNull(ResultSet rs, String column) throws SQLException {
        long value = rs.getLong(column);
        return rs.wasNull() ? null : Instant.ofEpochMilli(value);
    }

    private static GameServer map(ResultSet rs, int row) throws SQLException {
        ModeCategory category = ModeCategory.fromKey(rs.getString("category")).orElse(null);
        Instant reservedAt = instantOrNull(rs, "reserved_at");
        Instant lastAssignedAt = instantOrNull(rs, "last_assigned_at");
        Instant lastHeartbeatAt = instantOrNull(rs, "last_heartbeat_at");
        int maxPlayers = rs.getInt("max_players");
        Integer maxPlayersOrNull = rs.wasNull() ? null : maxPlayers;
        return new GameServer(
                rs.getLong("id"),
                rs.getString("host"),
                rs.getInt("port"),
                rs.getString("category"),
                category != null ? category.label() : rs.getString("category"),
                category != null && category.acceptRequired(),
                category != null ? category.requiredPlayers() : 0,
                rs.getString("map"),
                rs.getInt("enabled") != 0,
                rs.getString("state"),
                rs.getString("reserved_match_id"),
                reservedAt,
                lastAssignedAt,
                lastHeartbeatAt,
                maxPlayersOrNull,
                Instant.ofEpochMilli(rs.getLong("created_at")),
                Instant.ofEpochMilli(rs.getLong("updated_at")),
                instantOrNull(rs, "available_after"));
    }
}
