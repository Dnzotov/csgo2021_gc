package dev.csgogc.mm.search;

import java.sql.ResultSet;
import java.sql.SQLException;
import java.time.Instant;
import java.util.Arrays;
import java.util.List;
import java.util.Optional;
import org.springframework.jdbc.core.simple.JdbcClient;
import org.springframework.jdbc.support.GeneratedKeyHolder;
import org.springframework.jdbc.support.KeyHolder;
import org.springframework.stereotype.Repository;

@Repository
public class SearchRepository {

    private static final String COLUMNS = "id, account_id, game_type, category, game_mode, maps, variants, request_id, status, source, "
            + "started_at, last_seen_at, ended_at, match_id, matched_at, accepted_at";

    private final JdbcClient jdbc;

    public SearchRepository(JdbcClient jdbc) {
        this.jdbc = jdbc;
    }

    public Optional<SearchRecord> findLiveByAccount(long accountId) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_search WHERE account_id = ? AND status IN " + SearchStatus.LIVE_SQL)
                .param(accountId)
                .query(SearchRepository::map)
                .optional();
    }

    public Optional<SearchRecord> findById(long id) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_search WHERE id = ?")
                .param(id)
                .query(SearchRepository::map)
                .optional();
    }

    /** the newest search that used this request id (a request id belongs to one search, live or ended) */
    public Optional<SearchRecord> findByRequestId(String requestId) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_search WHERE request_id = ? ORDER BY id DESC LIMIT 1")
                .param(requestId)
                .query(SearchRepository::map)
                .optional();
    }

    public long insert(long accountId, long gameType, String category, String gameMode, List<String> maps,
                       List<SearchVariant> variants, String requestId, String source, Instant now) {
        KeyHolder keys = new GeneratedKeyHolder();
        jdbc.sql("INSERT INTO matchmaking_search (account_id, game_type, category, game_mode, maps, variants, request_id, "
                        + "status, source, started_at, last_seen_at) VALUES (?, ?, ?, ?, ?, ?, ?, 'SEARCHING', ?, ?, ?)")
                .params(accountId, gameType, category, gameMode, String.join(",", maps), SearchVariant.encode(variants),
                        requestId, source, now.toEpochMilli(), now.toEpochMilli())
                .update(keys);
        return keys.getKey().longValue();
    }

    /** the search changed (new mode/maps/request): rewrite it in place, back to plain SEARCHING, and restart the clock */
    public void replaceLive(long id, long gameType, String category, String gameMode, List<String> maps,
                            List<SearchVariant> variants, String requestId, String source, Instant now) {
        jdbc.sql("UPDATE matchmaking_search SET game_type = ?, category = ?, game_mode = ?, maps = ?, variants = ?, "
                        + "request_id = ?, source = ?, status = 'SEARCHING', match_id = NULL, matched_at = NULL, accepted_at = NULL, "
                        + "started_at = ?, last_seen_at = ? WHERE id = ?")
                .params(gameType, category, gameMode, String.join(",", maps), SearchVariant.encode(variants), requestId,
                        source, now.toEpochMilli(), now.toEpochMilli(), id)
                .update();
    }

    public void touch(long id, Instant now) {
        jdbc.sql("UPDATE matchmaking_search SET last_seen_at = ? WHERE id = ?").params(now.toEpochMilli(), id).update();
    }

    /** a live search -> an ended status; returns whether the row was still live */
    public boolean finish(long id, SearchStatus status, Instant now) {
        return jdbc.sql("UPDATE matchmaking_search SET status = ?, ended_at = ? WHERE id = ? AND status IN " + SearchStatus.LIVE_SQL)
                .params(status.name(), now.toEpochMilli(), id)
                .update() > 0;
    }

    /** places a search in a match (MATCHED while forming, WAITING_ACCEPT / READY_TO_CONNECT when complete) */
    public void place(long id, String matchId, SearchStatus status, Instant now) {
        jdbc.sql("UPDATE matchmaking_search SET match_id = ?, status = ?, matched_at = ?, accepted_at = NULL WHERE id = ? AND status IN " + SearchStatus.LIVE_SQL)
                .params(matchId, status.name(), now.toEpochMilli(), id)
                .update();
    }

    /** back into the queue (the match it was in fell apart) */
    public void unplace(long id) {
        jdbc.sql("UPDATE matchmaking_search SET match_id = NULL, matched_at = NULL, accepted_at = NULL, status = 'SEARCHING' WHERE id = ? AND status IN " + SearchStatus.LIVE_SQL)
                .param(id)
                .update();
    }

    /** the game server said everybody accepted and this player's GC reported it (only while the assignment is out) */
    public boolean markAccepted(long id, Instant now) {
        return jdbc.sql("UPDATE matchmaking_search SET accepted_at = ? WHERE id = ? AND status = 'WAITING_ACCEPT' AND accepted_at IS NULL")
                .params(now.toEpochMilli(), id)
                .update() > 0;
    }

    /** every player of the match accepted: they connect now */
    public void promoteAccepted(String matchId, Instant now) {
        jdbc.sql("UPDATE matchmaking_search SET status = 'READY_TO_CONNECT', matched_at = ? WHERE match_id = ? AND status = 'WAITING_ACCEPT'")
                .params(now.toEpochMilli(), matchId)
                .update();
    }

    /** waiting for a server, oldest first */
    public List<SearchRecord> listUnplaced() {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_search WHERE status = 'SEARCHING' AND match_id IS NULL "
                        + "ORDER BY started_at, id")
                .query(SearchRepository::map)
                .list();
    }

    public List<SearchRecord> listLiveByMatch(String matchId) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_search WHERE match_id = ? AND status IN " + SearchStatus.LIVE_SQL
                        + " ORDER BY started_at, id")
                .param(matchId)
                .query(SearchRepository::map)
                .list();
    }

    /** every search that was placed in the match, whatever became of it (for the match list of the panel) */
    public List<SearchRecord> listByMatch(String matchId) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_search WHERE match_id = ? ORDER BY started_at, id")
                .param(matchId)
                .query(SearchRepository::map)
                .list();
    }

    /** ids of every match this account was ever placed in */
    public List<String> findMatchIdsByAccount(long accountId) {
        return jdbc.sql("SELECT DISTINCT match_id FROM matchmaking_search WHERE account_id = ? AND match_id IS NOT NULL")
                .param(accountId)
                .query(String.class)
                .list();
    }

    /** SEARCHING / MATCHED searches nobody refreshed (the GC polls, so a live client keeps them fresh) */
    public List<SearchRecord> listStale(Instant cutoff) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_search WHERE status IN ('SEARCHING', 'MATCHED') AND last_seen_at < ?")
                .param(cutoff.toEpochMilli())
                .query(SearchRepository::map)
                .list();
    }

    /** assigned searches whose server data has been out for long enough */
    public List<SearchRecord> listAssignedBefore(Instant cutoff) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_search WHERE status IN ('WAITING_ACCEPT', 'READY_TO_CONNECT') AND matched_at < ?")
                .param(cutoff.toEpochMilli())
                .query(SearchRepository::map)
                .list();
    }

    public int deleteFinishedBefore(Instant cutoff) {
        return jdbc.sql("DELETE FROM matchmaking_search WHERE status NOT IN " + SearchStatus.LIVE_SQL + " AND ended_at < ?")
                .param(cutoff.toEpochMilli())
                .update();
    }

    /** live searches oldest first, then (optionally) the recently ended ones newest first */
    public List<SearchRecord> list(boolean includeFinished) {
        String where = includeFinished ? "" : " WHERE status IN " + SearchStatus.LIVE_SQL;
        return jdbc.sql("SELECT " + COLUMNS + " FROM matchmaking_search" + where
                        + " ORDER BY (status IN " + SearchStatus.LIVE_SQL + ") DESC, "
                        + "CASE WHEN status IN " + SearchStatus.LIVE_SQL + " THEN started_at END ASC, ended_at DESC")
                .query(SearchRepository::map)
                .list();
    }

    private static SearchRecord map(ResultSet rs, int row) throws SQLException {
        String maps = rs.getString("maps");
        long ended = rs.getLong("ended_at");
        boolean endedIsNull = rs.wasNull();
        String matchId = rs.getString("match_id");
        long matched = rs.getLong("matched_at");
        boolean matchedIsNull = rs.wasNull();
        long accepted = rs.getLong("accepted_at");
        boolean acceptedIsNull = rs.wasNull();
        return new SearchRecord(
                rs.getLong("id"),
                rs.getLong("account_id"),
                rs.getLong("game_type"),
                rs.getString("category"),
                rs.getString("game_mode"),
                maps == null || maps.isEmpty() ? List.of() : Arrays.asList(maps.split(",")),
                SearchVariant.decode(rs.getString("variants")),
                rs.getString("request_id"),
                SearchStatus.valueOf(rs.getString("status")),
                rs.getString("source"),
                Instant.ofEpochMilli(rs.getLong("started_at")),
                Instant.ofEpochMilli(rs.getLong("last_seen_at")),
                endedIsNull ? null : Instant.ofEpochMilli(ended),
                matchId,
                matchedIsNull ? null : Instant.ofEpochMilli(matched),
                acceptedIsNull ? null : Instant.ofEpochMilli(accepted));
    }
}
