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

/**
 * The Fake Players profiles (table fake_search). The columns status / match_id / matched_at / taken of the earlier "pool that a
 * match uses up" design are still in the table of an existing database and are ignored: the match records its virtual players.
 */
@Repository
public class FakeSearchRepository {

    private static final String COLUMNS = "id, category, players, maps, enabled, priority, server_id, created_at, updated_at";

    private final JdbcClient jdbc;

    public FakeSearchRepository(JdbcClient jdbc) {
        this.jdbc = jdbc;
    }

    public long insert(String category, int players, List<String> maps, boolean enabled, int priority, Long serverId, Instant now) {
        KeyHolder keys = new GeneratedKeyHolder();
        jdbc.sql("INSERT INTO fake_search (category, players, maps, enabled, status, priority, server_id, created_at, updated_at) "
                        + "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)")
                .params(category, players, String.join(",", maps), enabled ? 1 : 0, enabled ? "SEARCHING" : "STOPPED", priority,
                        serverId, now.toEpochMilli(), now.toEpochMilli())
                .update(keys);
        return keys.getKey().longValue();
    }

    public Optional<FakeSearch> findById(long id) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM fake_search WHERE id = ?").param(id)
                .query(FakeSearchRepository::map).optional();
    }

    public List<FakeSearch> list() {
        return jdbc.sql("SELECT " + COLUMNS + " FROM fake_search ORDER BY category, priority DESC, id")
                .query(FakeSearchRepository::map).list();
    }

    /** the enabled profiles of a mode in the order they are tried: highest priority first, the older one on a tie */
    public List<FakeSearch> listEnabled(String category) {
        return jdbc.sql("SELECT " + COLUMNS + " FROM fake_search WHERE enabled = 1 AND category = ? ORDER BY priority DESC, id")
                .param(category).query(FakeSearchRepository::map).list();
    }

    public void update(long id, String category, int players, List<String> maps, int priority, Long serverId, Instant now) {
        jdbc.sql("UPDATE fake_search SET category = ?, players = ?, maps = ?, priority = ?, server_id = ?, updated_at = ? WHERE id = ?")
                .params(category, players, String.join(",", maps), priority, serverId, now.toEpochMilli(), id).update();
    }

    public void setEnabled(long id, boolean enabled, Instant now) {
        jdbc.sql("UPDATE fake_search SET enabled = ?, status = ?, updated_at = ? WHERE id = ?")
                .params(enabled ? 1 : 0, enabled ? "SEARCHING" : "STOPPED", now.toEpochMilli(), id).update();
    }

    public boolean delete(long id) {
        return jdbc.sql("DELETE FROM fake_search WHERE id = ?").param(id).update() > 0;
    }

    private static FakeSearch map(ResultSet rs, int row) throws SQLException {
        String maps = rs.getString("maps");
        long server = rs.getLong("server_id");
        boolean serverNull = rs.wasNull();
        return new FakeSearch(
                rs.getLong("id"),
                rs.getString("category"),
                rs.getInt("players"),
                maps == null || maps.isEmpty() ? List.of() : Arrays.asList(maps.split(",")),
                rs.getInt("enabled") != 0,
                rs.getInt("priority"),
                serverNull ? null : server,
                Instant.ofEpochMilli(rs.getLong("created_at")),
                Instant.ofEpochMilli(rs.getLong("updated_at")));
    }
}
