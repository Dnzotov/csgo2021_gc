package dev.csgogc.mm.search;

import dev.csgogc.mm.server.GameServer;
import dev.csgogc.mm.server.GameServerService;
import dev.csgogc.mm.web.ApiException;
import jakarta.validation.Valid;
import jakarta.validation.constraints.Max;
import jakarta.validation.constraints.Min;
import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.NotNull;
import jakarta.validation.constraints.Pattern;
import jakarta.validation.constraints.Size;
import java.time.Clock;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.boot.info.BuildProperties;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

import dev.csgogc.mm.config.BackendProperties;

/** The HTTP API the GC (csgo_gc.dll) talks to, plus the search list the admin panel polls. */
@RestController
@RequestMapping("/api/v1")
public class SearchApiController {

    /** what a game server reports about itself: "I am alive, this is my address, map and state" */
    public record ServerReport(
            @NotBlank @Size(max = 253) @Pattern(regexp = "[A-Za-z0-9._:-]*", message = "must be an IP address or hostname") String address,
            @NotNull @Min(1) @Max(65535) Integer port,
            @Pattern(regexp = "AVAILABLE|BUSY", message = "must be AVAILABLE or BUSY") String state,
            @Pattern(regexp = "[A-Za-z0-9_]{0,64}", message = "must be a map name such as de_dust2") String map) {
    }

    private final SearchService searches;
    private final GameServerService servers;
    private final BackendProperties props;
    private final Clock clock;
    private final String version;

    public SearchApiController(SearchService searches, GameServerService servers, BackendProperties props, Clock clock,
                               ObjectProvider<BuildProperties> build) {
        this.searches = searches;
        this.servers = servers;
        this.props = props;
        this.clock = clock;
        BuildProperties b = build.getIfAvailable();
        this.version = b != null ? b.getVersion() : "dev";
    }

    @GetMapping("/health")
    public Map<String, Object> health() {
        Map<String, Object> body = new LinkedHashMap<>();
        body.put("status", "ok");
        body.put("service", "matchmaking-backend");
        body.put("version", version);
        body.put("time", clock.instant());
        return body;
    }

    /** The player started (or restarted) a search. Idempotent per account: a repeat never creates a duplicate. */
    @PostMapping("/matchmaking/search")
    public Map<String, Object> start(@Valid @RequestBody SearchRequest request) {
        SearchService.StartResult result = searches.start(request, "gc");
        Map<String, Object> body = new LinkedHashMap<>();
        body.put("result", result.outcome().name().toLowerCase());
        body.put("search", result.search());
        return body;
    }

    /**
     * The GC polls this while a search runs: it keeps the search alive (timeout), and answers with its state. Once the
     * match is complete the search has status WAITING_ACCEPT / READY_TO_CONNECT and an "assignment" (the server).
     */
    /**
     * The GC of a player asks whether its account has a live search. A party member's client never sends a search: the
     * leader's does, and the backend keeps a search for every member (RESEARCH_FINDINGS.md #65). Its GC polls this to find
     * it (the answer carries party_leader_id, request_id, mode and game_type), then follows it like any search. 404 = none.
     */
    @GetMapping("/matchmaking/account/{accountId}")
    public Map<String, Object> accountSearch(@PathVariable @Min(1) @Max(0xFFFFFFFFL) long accountId) {
        SearchView view = searches.pollAccount(accountId)
                .orElseThrow(() -> ApiException.notFound("no live search for account " + accountId));
        Map<String, Object> body = new LinkedHashMap<>();
        body.put("search", view);
        return body;
    }

    @GetMapping("/matchmaking/search/{requestId}")
    public Map<String, Object> state(@PathVariable @Pattern(regexp = "[A-Za-z0-9_.:-]{1,64}") String requestId) {
        SearchView view = searches.poll(requestId)
                .orElseThrow(() -> ApiException.notFound("no search with request_id " + requestId));
        Map<String, Object> body = new LinkedHashMap<>();
        body.put("search", view);
        return body;
    }

    /** A game server reporting itself (heartbeat): map and, optionally, AVAILABLE / BUSY. Registered servers only. */
    @PostMapping("/servers/state")
    public GameServer serverState(@Valid @RequestBody ServerReport report) {
        return servers.report(new GameServerService.Report(report.address(), report.port(), report.state(), report.map()));
    }

    /**
     * The srcds side asks who is in the match that is on it: the real players and the virtual test players (fake=true,
     * ids 0xFA4E0000 + n). 404 when the server is not registered or has no forming / ready match. This is the channel
     * that will replace the srcds own test roster (RESEARCH_FINDINGS.md #54); nothing consumes it yet.
     */
    @GetMapping("/servers/roster")
    public SearchService.MatchRoster roster(
            @RequestParam @Pattern(regexp = "[A-Za-z0-9._:-]{1,253}") String address,
            @RequestParam @Min(1) @Max(65535) int port) {
        return searches.rosterForServer(address, port)
                .orElseThrow(() -> ApiException.notFound("no forming or ready match on " + address + ":" + port));
    }

    /** what the game server confirms: "I armed the roster of this match" */
    public record RosterReady(
            @NotBlank @Size(max = 253) @Pattern(regexp = "[A-Za-z0-9._:-]*", message = "must be an IP address or hostname") String address,
            @NotNull @Min(1) @Max(65535) Integer port,
            @NotBlank @Pattern(regexp = "[A-Za-z0-9_.:-]{1,64}", message = "must be a match id such as m-5b0cf1f7") String matchId) {
    }

    /**
     * The srcds side confirms that it armed the roster of its match (reservation + fake participants at stage 1): only
     * then do the players of that match receive the server. 404: unknown server or not the match of that server.
     */
    @PostMapping("/servers/roster/ready")
    public Map<String, Object> rosterReady(@Valid @RequestBody RosterReady request) {
        int promoted = searches.confirmRoster(request.address(), request.port(), request.matchId())
                .orElseThrow(() -> ApiException.notFound("match " + request.matchId() + " is not on " + request.address() + ":" + request.port()));
        Map<String, Object> body = new LinkedHashMap<>();
        body.put("match_id", request.matchId());
        body.put("promoted", promoted);
        return body;
    }

    /** the GC of a player that accepted: the game server reported everybody accepted (reservation stage 2, awaiting 0) */
    public record AcceptedRequest(
            @NotNull @Min(1) @Max(0xFFFFFFFFL) Long accountId,
            @Pattern(regexp = "[A-Za-z0-9_.:-]{1,64}", message = "must be 1-64 chars of [A-Za-z0-9_.:-]") String requestId) {
    }

    /**
     * "I accepted" (RESEARCH_FINDINGS.md #63): the player must be in a match that is ACCEPTING, otherwise nothing changes and
     * the answer says why (accepted=false). When every real player of the match reported, the match is ACCEPTED.
     */
    @PostMapping("/matchmaking/accepted")
    public Map<String, Object> accepted(@Valid @RequestBody AcceptedRequest request) {
        SearchService.AcceptResult result = searches.accept(request.accountId(), request.requestId());
        Map<String, Object> body = new LinkedHashMap<>();
        body.put("accepted", result.accepted());
        body.put("match_accepted", result.matchAccepted());
        if (result.reason() != null) {
            body.put("reason", result.reason());
        }
        if (result.search() != null) {
            body.put("search", result.search());
        }
        return body;
    }

    /** The player stopped searching (MatchmakingStop). Cancelling a search that does not exist is not an error. */
    @PostMapping("/matchmaking/cancel")
    public Map<String, Object> cancel(@Valid @RequestBody CancelRequest request) {
        SearchService.CancelResult result = searches.cancel(request.accountId(), request.requestId());
        Map<String, Object> body = new LinkedHashMap<>();
        body.put("cancelled", result.cancelled());
        if (result.reason() != null) {
            body.put("reason", result.reason());
        }
        return body;
    }

    @GetMapping("/matchmaking/searches")
    public Map<String, Object> list(@RequestParam(name = "include_finished", defaultValue = "false") boolean includeFinished) {
        List<SearchView> list = searches.list(includeFinished);
        Map<String, Object> body = new LinkedHashMap<>();
        body.put("count", list.stream().filter(s -> s.status().equals("SEARCHING")).count());
        body.put("stale_search_timeout_seconds", props.staleSearchTimeout().toSeconds());
        body.put("server_time", clock.instant());
        body.put("searches", list);
        return body;
    }
}
