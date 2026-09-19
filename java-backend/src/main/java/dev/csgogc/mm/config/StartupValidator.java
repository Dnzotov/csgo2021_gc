package dev.csgogc.mm.config;

import jakarta.annotation.PostConstruct;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;

/** Refuses to start without admin credentials (none are hardcoded) and warns about risky settings. */
@Component
public class StartupValidator {

    private static final Logger log = LoggerFactory.getLogger(StartupValidator.class);

    private final BackendProperties props;
    private final String bindAddress;

    public StartupValidator(BackendProperties props, @Value("${server.address:}") String bindAddress) {
        this.props = props;
        this.bindAddress = bindAddress;
    }

    @PostConstruct
    void validate() {
        if (props.admin().username().isBlank() || props.admin().password().isBlank()) {
            throw new IllegalStateException(
                    "Admin credentials are not configured. Set backend.admin.username and backend.admin.password "
                            + "(config/application.properties) or the environment variables BACKEND_ADMIN_USERNAME "
                            + "and BACKEND_ADMIN_PASSWORD. There are no default credentials.");
        }
        if (props.admin().password().length() < 8) {
            log.warn("backend.admin.password is shorter than 8 characters");
        }
        if (props.staleSearchTimeout().isNegative() || props.staleSearchTimeout().isZero()) {
            throw new IllegalStateException("backend.stale-search-timeout must be positive");
        }
        props.requiredPlayers().forEach((key, players) -> {
            boolean accept = dev.csgogc.mm.mode.ModeCategory.fromKey(key).map(dev.csgogc.mm.mode.ModeCategory::acceptRequired).orElse(false);
            if (!accept || players == null || players < 1 || players > 64) {
                throw new IllegalStateException("backend.required-players." + key + "=" + players
                        + " is invalid: only competitive / wingman / dangerzone can be set, to 1..64");
            }
            log.warn("backend.required-players.{}={} (retail: {}): matches of this category start with {} real player(s)",
                    key, players, dev.csgogc.mm.mode.ModeCategory.fromKey(key).orElseThrow().requiredPlayers(), players);
        });
        if (props.apiKey().isBlank()) {
            log.warn("backend.api-key is empty: POST /api/v1/matchmaking/search|cancel are open to anyone who can "
                    + "reach the HTTP port. Set BACKEND_API_KEY and send it as X-Api-Key from the GC.");
        }
        boolean exposed = !bindAddress.isBlank() && !bindAddress.equals("127.0.0.1") && !bindAddress.equals("localhost")
                && !bindAddress.equals("::1");
        if (exposed) {
            log.warn("HTTP is bound to {} (not loopback) and served without TLS: admin credentials travel in clear text",
                    bindAddress);
        }
    }
}
