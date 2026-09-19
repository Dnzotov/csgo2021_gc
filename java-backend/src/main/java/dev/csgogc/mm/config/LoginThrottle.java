package dev.csgogc.mm.config;

import java.time.Clock;
import java.time.Instant;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import org.springframework.stereotype.Component;

/** Locks a client address out of the login form after too many failed attempts (in memory, resets on restart). */
@Component
public class LoginThrottle {

    private record State(int failures, Instant windowStart, Instant lockedUntil) {
    }

    private final Map<String, State> states = new ConcurrentHashMap<>();
    private final BackendProperties.Login settings;
    private final Clock clock;

    public LoginThrottle(BackendProperties props, Clock clock) {
        this.settings = props.login();
        this.clock = clock;
    }

    public boolean isLocked(String client) {
        State s = states.get(client);
        return s != null && s.lockedUntil() != null && clock.instant().isBefore(s.lockedUntil());
    }

    public void failure(String client) {
        Instant now = clock.instant();
        states.compute(client, (k, s) -> {
            if (s == null || now.isAfter(s.windowStart().plus(settings.failureWindow()))
                    || (s.lockedUntil() != null && now.isAfter(s.lockedUntil()))) {
                s = new State(0, now, null);
            }
            int failures = s.failures() + 1;
            Instant lockedUntil = failures >= settings.maxFailures() ? now.plus(settings.lockDuration()) : null;
            return new State(failures, s.windowStart(), lockedUntil);
        });
    }

    public void success(String client) {
        states.remove(client);
    }
}
