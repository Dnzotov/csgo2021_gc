package dev.csgogc.mm.search;

import java.sql.ResultSet;
import java.sql.SQLException;
import java.time.Instant;
import java.util.List;
import java.util.Optional;
import org.springframework.jdbc.core.simple.JdbcClient;
import org.springframework.stereotype.Repository;

@Repository
public class MatchRepository {

    private static final String COLUMNS = "id, category, server_id, server_host, server_port, map, required_players, "
            + "accept_required, status, created_at, ready_at, ended_at";

    private final JdbcClient jdbc;

    public MatchRepository(JdbcClient jdbc) {
        this.jdbc = jdbc;
    }

    public void insert(MatchRecord m) {
        jdbc.sql("INSERT INTO matchmaking_match (id, category, server_id, server_host, server_port, map, required_players, "
                        + "accept_required, status, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
                .params(m.id(), m.category(), m.serverId(), m.serverHost(), m.serverPort(), m.map(), m.requiredPlayers(),
                        m.acceptRequired() ? 1 : 0, m.status().name(), m.createdAt().toEpochMilli())
                .update();
    }

    public Optional<MatchRecord> findById(String id) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_match WHERE id = ?")
                .param(id)
                .query(MatchRepository::map)
                .optional();
    }

    /** matches still gathering players, oldest first */
    public List<MatchRecord> findForming(String category) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_match WHERE status = 'FORMING' AND category = ? ORDER BY created_at, id")
                .param(category)
                .query(MatchRepository::map)
                .list();
    }

    /** the newest match still forming or ready on a server (for the srcds side roster call) */
    public Optional<MatchRecord> findActiveByServer(long serverId) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_match WHERE server_id = ? AND status IN ('FORMING', 'READY') "
                        + "ORDER BY created_at DESC, id DESC LIMIT 1")
                .param(serverId)
                .query(MatchRepository::map)
                .optional();
    }

    /** every complete match that is still running (a handful at most) */
    public List<MatchRecord> findReady() {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_match WHERE status = 'READY' ORDER BY ready_at, id")
                .query(MatchRepository::map)
                .list();
    }

    public List<MatchRecord> findReadyBefore(Instant cutoff) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_match WHERE status = 'READY' AND ready_at < ?")
                .param(cutoff.toEpochMilli())
                .query(MatchRepository::map)
                .list();
    }

    public List<MatchRecord> recent(int limit) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_match ORDER BY created_at DESC, id DESC LIMIT ?")
                .param(limit)
                .query(MatchRepository::map)
                .list();
    }

    public void markReady(String id, Instant now) {
        jdbc.sql("UPDATE matchmaking_match SET status = 'READY', ready_at = ? WHERE id = ?")
                .params(now.toEpochMilli(), id)
                .update();
    }

    public void end(String id, MatchStatus status, Instant now) {
        jdbc.sql("UPDATE matchmaking_match SET status = ?, ended_at = ? WHERE id = ? AND status IN ('FORMING', 'READY')")
                .params(status.name(), now.toEpochMilli(), id)
                .update();
    }

    public int deleteEndedBefore(Instant cutoff) {
        return jdbc.sql("DELETE FROM matchmaking_match WHERE status IN ('ENDED', 'CANCELLED') AND ended_at < ?")
                .param(cutoff.toEpochMilli())
                .update();
    }

    private static MatchRecord map(ResultSet rs, int row) throws SQLException {
        long ready = rs.getLong("ready_at");
        boolean readyNull = rs.wasNull();
        long ended = rs.getLong("ended_at");
        boolean endedNull = rs.wasNull();
        return new MatchRecord(
                rs.getString("id"),
                rs.getString("category"),
                rs.getLong("server_id"),
                rs.getString("server_host"),
                rs.getInt("server_port"),
                rs.getString("map"),
                rs.getInt("required_players"),
                rs.getInt("accept_required") != 0,
                MatchStatus.valueOf(rs.getString("status")),
                Instant.ofEpochMilli(rs.getLong("created_at")),
                readyNull ? null : Instant.ofEpochMilli(ready),
                endedNull ? null : Instant.ofEpochMilli(ended));
    }
}
