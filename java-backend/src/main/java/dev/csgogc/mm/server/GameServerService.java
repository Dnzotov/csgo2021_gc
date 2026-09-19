package dev.csgogc.mm.server;

import dev.csgogc.mm.mode.ModeCategory;
import dev.csgogc.mm.search.SearchService;
import dev.csgogc.mm.web.ApiException;
import java.time.Clock;
import java.util.List;
import java.util.Locale;
import java.util.Optional;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Service;

/**
 * The game server registry and the availability of each server. The registry is the source of truth for the
 * matchmaker: which servers exist, what they run and whether they are free. Changes go through here so a reserved
 * server never loses its match silently and a new / freed server is offered to waiting searches at once.
 */
@Service
public class GameServerService {

    private static final Logger log = LoggerFactory.getLogger(GameServerService.class);

    private final GameServerRepository repository;
    private final SearchService matchmaker;
    private final Clock clock;

    public GameServerService(GameServerRepository repository, SearchService matchmaker, Clock clock) {
        this.repository = repository;
        this.matchmaker = matchmaker;
        this.clock = clock;
    }

    public List<GameServer> list() {
        return repository.list();
    }

    public GameServer get(long id) {
        return repository.findById(id).orElseThrow(() -> ApiException.notFound("game server " + id));
    }

    public GameServer create(GameServerRequest request) {
        synchronized (matchmaker) {
            return doCreate(request);
        }
    }

    private GameServer doCreate(GameServerRequest request) {
        Normalized n = normalize(request);
        ensureUnique(n.host, n.port, null);
        String state = request.state() == null ? ServerState.AVAILABLE.name() : request.state();
        long id = repository.insert(n.host, n.port, n.category.key(), n.map, n.enabled, state, request.maxPlayers(), clock.instant());
        log.info("game server added: {}:{} {} map='{}' enabled={} state={}", n.host, n.port, n.category.key(), n.map, n.enabled, state);
        matchmaker.serversChanged();
        return get(id);
    }

    /** host / port / category / map / enabled / max players; the availability state is not touched */
    public GameServer update(long id, GameServerRequest request) {
        synchronized (matchmaker) {
            return doUpdate(id, request);
        }
    }

    private GameServer doUpdate(long id, GameServerRequest request) {
        get(id);
        Normalized n = normalize(request);
        ensureUnique(n.host, n.port, id);
        repository.update(id, n.host, n.port, n.category.key(), n.map, n.enabled, request.maxPlayers(), clock.instant());
        log.info("game server {} updated: {}:{} {} map='{}' enabled={}", id, n.host, n.port, n.category.key(), n.map, n.enabled);
        matchmaker.serversChanged();
        return get(id);
    }

    public GameServer setEnabled(long id, boolean enabled) {
        synchronized (matchmaker) {
            return doSetEnabled(id, enabled);
        }
    }

    private GameServer doSetEnabled(long id, boolean enabled) {
        get(id);
        repository.setEnabled(id, enabled, clock.instant());
        log.info("game server {} {}", id, enabled ? "enabled" : "disabled");
        matchmaker.serversChanged();
        return get(id);
    }

    /**
     * admin: AVAILABLE or BUSY. Taking a RESERVED server (AVAILABLE = "release") ends its match: a forming match
     * falls apart and its players go back to searching.
     */
    public GameServer setState(long id, ServerState state) {
        synchronized (matchmaker) {
            return doSetState(id, state);
        }
    }

    private GameServer doSetState(long id, ServerState state) {
        if (state == ServerState.RESERVED) {
            throw ApiException.badRequest("invalid_state", "RESERVED is set by the matchmaker only");
        }
        GameServer server = get(id);
        matchmaker.freeServer(server);
        repository.setState(id, state.name(), clock.instant());
        log.info("game server {}:{} set to {} (was {})", server.host(), server.port(), state, server.state());
        matchmaker.serversChanged();
        return get(id);
    }

    public void delete(long id) {
        synchronized (matchmaker) {
            doDelete(id);
        }
    }

    private void doDelete(long id) {
        GameServer server = get(id);
        matchmaker.freeServer(server);
        repository.delete(id);
        log.info("game server {} deleted", id);
    }

    /** what a server reports about itself (POST /api/v1/servers/state): "I am alive, this map, this state" */
    public record Report(String address, Integer port, String state, String map) {
    }

    /**
     * Heartbeat of a registered game server. Updates the last-seen time and the map; state BUSY is always accepted,
     * AVAILABLE only for a server that is not RESERVED (a fresh assignment must not be undone by a server that does
     * not know about it yet — releasing a reserved server is an admin action or the reservation TTL).
     */
    public GameServer report(Report report) {
        synchronized (matchmaker) {
            return doReport(report);
        }
    }

    private GameServer doReport(Report report) {
        String host = report.address().trim().toLowerCase(Locale.ROOT);
        GameServer server = repository.findByHostPort(host, report.port())
                .orElseThrow(() -> ApiException.notFound("game server " + host + ":" + report.port() + " is not registered"));
        String map = report.map() == null || report.map().isBlank() ? null : report.map().trim();
        repository.heartbeat(server.id(), map, clock.instant());

        if (report.state() != null) {
            ServerState wanted = ServerState.valueOf(report.state());
            if (wanted == ServerState.BUSY) {
                matchmaker.freeServer(server);
                repository.setState(server.id(), ServerState.BUSY.name(), clock.instant());
            } else if (wanted == ServerState.AVAILABLE && !ServerState.RESERVED.name().equals(server.state())) {
                repository.setState(server.id(), ServerState.AVAILABLE.name(), clock.instant());
            }
        }
        matchmaker.serversChanged();
        return get(server.id());
    }

    private void ensureUnique(String host, int port, Long selfId) {
        Optional<GameServer> other = repository.findByHostPort(host, port);
        if (other.isPresent() && (selfId == null || other.get().id() != selfId)) {
            throw ApiException.conflict("duplicate_server", "a server with " + host + ":" + port + " already exists");
        }
    }

    private record Normalized(String host, int port, ModeCategory category, String map, boolean enabled) {
    }

    private static Normalized normalize(GameServerRequest request) {
        ModeCategory category = ModeCategory.fromKey(request.category()).orElseThrow(() -> ApiException.badRequest(
                "unsupported_category", "unknown category '" + request.category() + "'"));
        String host = request.host().trim().toLowerCase(Locale.ROOT);
        if (host.isEmpty()) {
            throw ApiException.badRequest("invalid_request", "host must not be blank");
        }
        return new Normalized(host, request.port(), category, request.map() == null ? "" : request.map().trim(),
                request.enabled() == null || request.enabled());
    }
}
