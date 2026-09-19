package dev.csgogc.mm.config;

import com.zaxxer.hikari.HikariConfig;
import com.zaxxer.hikari.HikariDataSource;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Clock;
import javax.sql.DataSource;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

@Configuration
public class DatabaseConfig {

    private static final Logger log = LoggerFactory.getLogger(DatabaseConfig.class);

    /** SQLite allows a single writer: one pooled connection keeps everything serialized and avoids SQLITE_BUSY. */
    @Bean(destroyMethod = "close")
    DataSource dataSource(BackendProperties props) throws IOException {
        Path path = Path.of(props.databasePath()).toAbsolutePath().normalize();
        if (path.getParent() != null) {
            Files.createDirectories(path.getParent());
        }
        log.info("SQLite database: {}", path);

        HikariConfig config = new HikariConfig();
        config.setJdbcUrl("jdbc:sqlite:" + path.toString().replace('\\', '/') + "?journal_mode=WAL&busy_timeout=5000");
        config.setMaximumPoolSize(1);
        config.setPoolName("sqlite");
        HikariDataSource dataSource = new HikariDataSource(config);
        SchemaInitializer.apply(dataSource); // tables + migration of an existing file, before anything uses the database
        return dataSource;
    }

    @Bean
    Clock clock() {
        return Clock.systemUTC();
    }
}
