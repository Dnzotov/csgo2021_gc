package dev.csgogc.mm.fake;

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
public class FakeSearchRepository {

    private static final String COLUMNS = "id, category, players, maps, enabled, status, match_id, created_at, updated_at, matched_at";

    private final JdbcClient jdbc;

    public FakeSearchRepository(JdbcClient jdbc) {
        this.jdbc = jdbc;
    }

    public long insert(String category, int players, List<String> maps, boolean enabled, Instant now) {
        KeyHolder keys = new GeneratedKeyHolder();
        jdbc.sql("INSERT INTO fake_search (category, players, maps, enabled, status, created_at, updated_at) "
                        + "VALUES (?, ?, ?, ?, ?, ?, ?)")
                .params(category, players, String.join(",", maps), enabled ? 1 : 0,
                        (enabled ? FakeStatus.SEARCHING : FakeStatus.STOPPED).name(), now.toEpochMilli(), now.toEpochMilli())
                .update(keys);
        return keys.getKey().longValue();
    }

    public Optional<FakeSearch> findById(long id) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM fake_search WHERE id = ?").param(id)
                .query(FakeSearchRepository::map).optional();
    }

    public List<FakeSearch> list() {
        return jdbc.sql("SELECT " + COLUMNS + " FROM fake_search ORDER BY id").query(FakeSearchRepository::map).list();
    }

    /** enabled fake searches waiting for a match, oldest first */
    public List<FakeSearch> listSearching() {
        return jdbc.sql("SELECT " + COLUMNS + " FROM fake_search WHERE enabled = 1 AND status = 'SEARCHING' AND match_id IS NULL "
                        + "ORDER BY created_at, id")
                .query(FakeSearchRepository::map).list();
    }

    /** every fake search placed in the match (joined order), whatever became of it */
    public List<FakeSearch> listByMatch(String matchId) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM fake_search WHERE match_id = ? ORDER BY matched_at, id")
                .param(matchId).query(FakeSearchRepository::map).list();
    }

    public int countPlayersInMatch(String matchId) {
        Integer sum = jdbc.sql("SELECT COALESCE(SUM(players), 0) FROM fake_search WHERE match_id = ?")
                .param(matchId).query(Integer.class).single();
        return sum == null ? 0 : sum;
    }

    public void update(long id, String category, int players, List<String> maps, Instant now) {
        jdbc.sql("UPDATE fake_search SET category = ?, players = ?, maps = ?, updated_at = ? WHERE id = ?")
                .params(category, players, String.join(",", maps), now.toEpochMilli(), id).update();
    }

    /** it joined a match */
    public void place(long id, String matchId, Instant now) {
        jdbc.sql("UPDATE fake_search SET status = 'MATCHED', match_id = ?, matched_at = ?, updated_at = ? WHERE id = ?")
                .params(matchId, now.toEpochMilli(), now.toEpochMilli(), id).update();
    }

    /** the forming match fell apart: the joined fake searches queue again */
    public void unplaceByMatch(String matchId, Instant now) {
        jdbc.sql("UPDATE fake_search SET status = 'SEARCHING', match_id = NULL, matched_at = NULL, updated_at = ? "
                        + "WHERE match_id = ? AND status = 'MATCHED'")
                .params(now.toEpochMilli(), matchId).update();
    }

    /** the match ended */
    public void completeByMatch(String matchId, Instant now) {
        jdbc.sql("UPDATE fake_search SET status = 'COMPLETED', updated_at = ? WHERE match_id = ? AND status = 'MATCHED'")
                .params(now.toEpochMilli(), matchId).update();
    }

    /** switched off; leaveMatch drops the link to a match that is still forming */
    public void stop(long id, boolean leaveMatch, Instant now) {
        jdbc.sql("UPDATE fake_search SET enabled = 0, status = 'STOPPED', updated_at = ?"
                        + (leaveMatch ? ", match_id = NULL, matched_at = NULL" : "") + " WHERE id = ?")
                .params(now.toEpochMilli(), id).update();
    }

    /** switched (back) on: a fresh search */
    public void start(long id, Instant now) {
        jdbc.sql("UPDATE fake_search SET enabled = 1, status = 'SEARCHING', match_id = NULL, matched_at = NULL, updated_at = ? WHERE id = ?")
                .params(now.toEpochMilli(), id).update();
    }

    public boolean delete(long id) {
        return jdbc.sql("DELETE FROM fake_search WHERE id = ?").param(id).update() > 0;
    }

    private static FakeSearch map(ResultSet rs, int row) throws SQLException {
        String maps = rs.getString("maps");
        long matched = rs.getLong("matched_at");
        boolean matchedNull = rs.wasNull();
        return new FakeSearch(
                rs.getLong("id"),
                rs.getString("category"),
                rs.getInt("players"),
                maps == null || maps.isEmpty() ? List.of() : Arrays.asList(maps.split(",")),
                rs.getInt("enabled") != 0,
                FakeStatus.valueOf(rs.getString("status")),
                rs.getString("match_id"),
                Instant.ofEpochMilli(rs.getLong("created_at")),
                Instant.ofEpochMilli(rs.getLong("updated_at")),
                matchedNull ? null : Instant.ofEpochMilli(matched));
    }
}
