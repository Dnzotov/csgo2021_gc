package dev.csgogc.mm.fake;

import java.util.Optional;
import org.springframework.jdbc.core.simple.JdbcClient;
import org.springframework.stereotype.Repository;

/** Runtime settings of the Fake Players tool that the admin panel edits (table backend_setting, key/value). */
@Repository
public class FakeSettingsRepository {

    public static final String MASTER = "fake.master";
    public static final String GATHER_WINDOW_SECONDS = "fake.gather_window_seconds";

    private final JdbcClient jdbc;

    public FakeSettingsRepository(JdbcClient jdbc) {
        this.jdbc = jdbc;
    }

    public Optional<String> get(String key) {
        return jdbc.sql("SELECT value FROM backend_setting WHERE key = ?").param(key).query(String.class).optional();
    }

    public void put(String key, String value) {
        jdbc.sql("INSERT INTO backend_setting (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value")
                .params(key, value).update();
    }
}
