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
            + "accept_required, status, created_at, ready_at, ended_at, accept_deadline_at, accepted_at, "
            + "fake_search_id, fake_count, fake_configured";

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

    /** every match that has no server yet (gathering or full), oldest first */
    public List<MatchRecord> findGathering() {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_match WHERE status IN ('FORMING', 'FULL') ORDER BY created_at, id")
                .query(MatchRepository::map)
                .list();
    }

    /** full matches waiting for a free game server, the one that waited longest first */
    public List<MatchRecord> findFull() {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_match WHERE status = 'FULL' ORDER BY created_at, id")
                .query(MatchRepository::map)
                .list();
    }

    /** matches of an older version that were FORMING with a server already reserved (the server is only reserved for FULL now) */
    public List<MatchRecord> findFormingWithServer() {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_match WHERE status = 'FORMING' AND server_id <> 0")
                .query(MatchRepository::map)
                .list();
    }

    /** the newest match that holds the server (for the srcds side roster call) */
    public Optional<MatchRecord> findActiveByServer(long serverId) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_match WHERE server_id = ? AND status IN " + MatchStatus.SERVER_SQL
                        + " ORDER BY created_at DESC, id DESC LIMIT 1")
                .param(serverId)
                .query(MatchRepository::map)
                .optional();
    }

    /** every match whose server is reserved / assigned but the players are not past the roster hand-over (a handful at most) */
    public List<MatchRecord> findReady() {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_match WHERE status = 'READY' ORDER BY ready_at, id")
                .query(MatchRepository::map)
                .list();
    }

    /** matches that hold a server since before the cutoff (the reservation TTL) */
    public List<MatchRecord> findHoldingServerBefore(Instant cutoff) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_match WHERE status IN " + MatchStatus.SERVER_SQL
                        + " AND ready_at < ?")
                .param(cutoff.toEpochMilli())
                .query(MatchRepository::map)
                .list();
    }

    /** Accept matches whose players ran out of time */
    public List<MatchRecord> findAcceptingDue(Instant now) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_match WHERE status = 'ACCEPTING' AND accept_deadline_at IS NOT NULL "
                        + "AND accept_deadline_at <= ? ORDER BY accept_deadline_at, id")
                .param(now.toEpochMilli())
                .query(MatchRepository::map)
                .list();
    }

    public List<MatchRecord> recent(int limit) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_match ORDER BY created_at DESC, id DESC LIMIT ?")
                .param(limit)
                .query(MatchRepository::map)
                .list();
    }

    /** a classic match: the server is assigned, the players connect (there is no gathering / Accept) */
    public void markReady(String id, Instant now) {
        jdbc.sql("UPDATE matchmaking_match SET status = 'READY', ready_at = ? WHERE id = ? AND status IN ('FORMING', 'FULL')")
                .params(now.toEpochMilli(), id)
                .update();
    }

    /** gathering -> every required player is there */
    public void markFull(String id) {
        jdbc.sql("UPDATE matchmaking_match SET status = 'FULL' WHERE id = ? AND status = 'FORMING'").param(id).update();
    }

    /** a player left a full match: it gathers again, and its virtual players are decided afresh once the window is over */
    public void revertToForming(String id) {
        jdbc.sql("UPDATE matchmaking_match SET status = 'FORMING', fake_search_id = NULL, fake_count = 0, fake_configured = NULL "
                        + "WHERE id = ? AND status = 'FULL'").param(id).update();
    }

    /** the Fake Players profile chosen for the match and the number of virtual players it gets (only while it is gathering) */
    public boolean setFake(String id, long profileId, int configured, int count) {
        return jdbc.sql("UPDATE matchmaking_match SET fake_search_id = ?, fake_configured = ?, fake_count = ? "
                        + "WHERE id = ? AND status = 'FORMING'")
                .params(profileId, configured, count, id)
                .update() > 0;
    }

    /** the live matches that used a profile (for the panel) */
    public List<MatchRecord> findLiveByFakeProfile(long profileId) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_match WHERE fake_search_id = ? AND status IN " + MatchStatus.LIVE_SQL
                        + " ORDER BY created_at DESC, id DESC")
                .param(profileId)
                .query(MatchRepository::map)
                .list();
    }

    /** FULL -> READY: the server that was reserved for the match (same transaction as GameServerRepository.reserve) */
    public boolean assignServer(String id, long serverId, String host, int port, String map, Instant now) {
        return jdbc.sql("UPDATE matchmaking_match SET server_id = ?, server_host = ?, server_port = ?, map = ?, status = 'READY', "
                        + "ready_at = ? WHERE id = ? AND status = 'FULL'")
                .params(serverId, host, port, map, now.toEpochMilli(), id)
                .update() > 0;
    }

    /** READY -> ACCEPTING: the assignment was issued */
    public boolean markAccepting(String id, Instant deadline) {
        return jdbc.sql("UPDATE matchmaking_match SET status = 'ACCEPTING', accept_deadline_at = ? WHERE id = ? AND status = 'READY'")
                .params(deadline.toEpochMilli(), id)
                .update() > 0;
    }

    /** ACCEPTING (or READY, nobody has to accept) -> ACCEPTED */
    public boolean markAccepted(String id, Instant now) {
        return jdbc.sql("UPDATE matchmaking_match SET status = 'ACCEPTED', accepted_at = ? WHERE id = ? AND status IN ('READY', 'ACCEPTING')")
                .params(now.toEpochMilli(), id)
                .update() > 0;
    }

    public void end(String id, MatchStatus status, Instant now) {
        jdbc.sql("UPDATE matchmaking_match SET status = ?, ended_at = ? WHERE id = ? AND status IN " + MatchStatus.LIVE_SQL)
                .params(status.name(), now.toEpochMilli(), id)
                .update();
    }

    public int deleteEndedBefore(Instant cutoff) {
        return jdbc.sql("DELETE FROM matchmaking_match WHERE status IN ('ENDED', 'CANCELLED') AND ended_at < ?")
                .param(cutoff.toEpochMilli())
                .update();
    }

    private static Instant instantOrNull(ResultSet rs, String column) throws SQLException {
        long value = rs.getLong(column);
        return rs.wasNull() ? null : Instant.ofEpochMilli(value);
    }

    private static MatchRecord map(ResultSet rs, int row) throws SQLException {
        long profile = rs.getLong("fake_search_id");
        Long fakeSearchId = rs.wasNull() ? null : profile;
        int fakeConfigured = rs.getInt("fake_configured");
        boolean fakeConfiguredNull = rs.wasNull();
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
                instantOrNull(rs, "ready_at"),
                instantOrNull(rs, "ended_at"),
                instantOrNull(rs, "accept_deadline_at"),
                instantOrNull(rs, "accepted_at"),
                fakeSearchId,
                rs.getInt("fake_count"),
                fakeConfiguredNull ? null : fakeConfigured);
    }
}
