package dev.csgogc.mm;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.context.properties.ConfigurationPropertiesScan;
import org.springframework.scheduling.annotation.EnableScheduling;

@SpringBootApplication
@ConfigurationPropertiesScan
@EnableScheduling
public class MatchmakingBackendApplication {

    public static void main(String[] args) throws IOException {
        useLocalUnixDomainTempDir();
        SpringApplication.run(MatchmakingBackendApplication.class, args);
    }

    /**
     * On Windows the JDK builds every NIO Selector (Tomcat needs one) on a loopback AF_UNIX socket created in the
     * temp directory. On some machines that fails for the user's %TEMP% ("Unable to establish loopback connection",
     * "Invalid argument: connect") and the HTTP server never starts. A directory next to the working directory
     * always works, so it is used unless the operator chose one (-Djdk.net.unixdomain.tmpdir=...). Must run before
     * the first Selector is opened.
     */
    private static void useLocalUnixDomainTempDir() throws IOException {
        if (System.getProperty("jdk.net.unixdomain.tmpdir") != null
                || !System.getProperty("os.name", "").toLowerCase().contains("win")) {
            return;
        }
        Path dir = Path.of("data", "tmp").toAbsolutePath();
        Files.createDirectories(dir);
        System.setProperty("jdk.net.unixdomain.tmpdir", dir.toString());
    }
}
