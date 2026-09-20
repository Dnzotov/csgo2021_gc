package dev.csgogc.mm.admin;

import dev.csgogc.mm.fake.FakeSearchRequest;
import dev.csgogc.mm.fake.FakeSettingsRequest;
import dev.csgogc.mm.fake.FakeSearchView;
import dev.csgogc.mm.mode.ModeCategory;
import dev.csgogc.mm.search.SearchRequest;
import dev.csgogc.mm.search.SearchService;
import dev.csgogc.mm.search.SearchView;
import dev.csgogc.mm.server.GameServer;
import dev.csgogc.mm.server.GameServerRequest;
import dev.csgogc.mm.server.GameServerService;
import dev.csgogc.mm.server.ServerState;
import jakarta.validation.Valid;
import jakarta.validation.constraints.Max;
import jakarta.validation.constraints.Min;
import jakarta.validation.constraints.NotNull;
import jakarta.validation.constraints.Pattern;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.springframework.http.HttpStatus;
import org.springframework.web.bind.annotation.DeleteMapping;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.PutMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.ResponseStatus;
import org.springframework.web.bind.annotation.RestController;

/** JSON endpoints of the admin panel. Everything under /admin/** requires a logged-in session (SecurityConfig). */
@RestController
@RequestMapping("/admin/api")
public class AdminApiController {

    public record EnabledRequest(@NotNull Boolean enabled) {
    }

    public record TestSearchRequest(
            @NotNull @Min(1) @Max(0xFFFFFFFFL) Long accountId,
            @NotNull @Pattern(regexp = "[A-Za-z0-9_]+") String mode,
            @Pattern(regexp = "[A-Za-z0-9_]{0,64}") String map) {
    }

    public record StateRequest(@NotNull @Pattern(regexp = "AVAILABLE|BUSY") String state) {
    }

    private final GameServerService servers;
    private final SearchService searches;

    public AdminApiController(GameServerService servers, SearchService searches) {
        this.servers = servers;
        this.searches = searches;
    }

    @GetMapping("/categories")
    public List<Map<String, Object>> categories() {
        return Arrays.stream(ModeCategory.values()).map(c -> {
            Map<String, Object> m = new LinkedHashMap<>();
            m.put("key", c.key());
            m.put("label", c.label());
            m.put("accept_required", c.acceptRequired());
            m.put("required_players", c.requiredPlayers());
            m.put("srcds_game_type", c.srcdsGameType());
            m.put("srcds_game_mode", c.srcdsGameMode());
            return m;
        }).toList();
    }

    // ---- game servers -------------------------------------------------------------------------------------

    @GetMapping("/servers")
    public List<GameServer> listServers() {
        return servers.list();
    }

    @PostMapping("/servers")
    @ResponseStatus(HttpStatus.CREATED)
    public GameServer createServer(@Valid @RequestBody GameServerRequest request) {
        return servers.create(request);
    }

    @PutMapping("/servers/{id}")
    public GameServer updateServer(@PathVariable long id, @Valid @RequestBody GameServerRequest request) {
        return servers.update(id, request);
    }

    @PostMapping("/servers/{id}/enabled")
    public GameServer setServerEnabled(@PathVariable long id, @Valid @RequestBody EnabledRequest request) {
        return servers.setEnabled(id, request.enabled());
    }

    /** admin: AVAILABLE or BUSY; AVAILABLE on a RESERVED server releases it (its match ends) */
    @PostMapping("/servers/{id}/state")
    public GameServer setServerState(@PathVariable long id, @Valid @RequestBody StateRequest request) {
        return servers.setState(id, ServerState.valueOf(request.state()));
    }

    @DeleteMapping("/servers/{id}")
    @ResponseStatus(HttpStatus.NO_CONTENT)
    public void deleteServer(@PathVariable long id) {
        servers.delete(id);
    }

    // ---- fake players (TEST tool, RESEARCH_FINDINGS.md #54) ---------------------------------------------------

    /** the tool can be switched off with backend.fake-players.enabled=false; the panel greys the section out then */
    @GetMapping("/fake-searches")
    public Map<String, Object> fakeSearches() {
        Map<String, Object> body = new LinkedHashMap<>();
        SearchService.FakeSettings settings = searches.fakeSettings();
        body.put("enabled", settings.available());
        body.put("master", settings.master());
        body.put("gather_window_seconds", settings.gatherWindowSeconds());
        body.put("fake_searches", searches.listFakeSearches());
        return body;
    }

    /** the master switch (Fake Players ON / OFF) and the gather window, stored in the database */
    @PutMapping("/fake-settings")
    public SearchService.FakeSettings updateFakeSettings(@Valid @RequestBody FakeSettingsRequest request) {
        return searches.updateFakeSettings(request.master(), request.gatherWindowSeconds());
    }

    @PostMapping("/fake-searches")
    @ResponseStatus(HttpStatus.CREATED)
    public FakeSearchView addFakeSearch(@Valid @RequestBody FakeSearchRequest request) {
        return searches.addFakeSearch(request);
    }

    @PutMapping("/fake-searches/{id}")
    public FakeSearchView updateFakeSearch(@PathVariable long id, @Valid @RequestBody FakeSearchRequest request) {
        return searches.updateFakeSearch(id, request);
    }

    /** enabled=false stops the fake search (Stop), true starts it again (Start) */
    @PostMapping("/fake-searches/{id}/enabled")
    public FakeSearchView setFakeSearchEnabled(@PathVariable long id, @Valid @RequestBody EnabledRequest request) {
        return searches.setFakeEnabled(id, request.enabled());
    }

    @DeleteMapping("/fake-searches/{id}")
    @ResponseStatus(HttpStatus.NO_CONTENT)
    public void deleteFakeSearch(@PathVariable long id) {
        searches.deleteFakeSearch(id);
    }

    // ---- matches ----------------------------------------------------------------------------------------

    @GetMapping("/matches")
    public List<SearchService.MatchView> matches() {
        return searches.listMatches();
    }

    // ---- searches -----------------------------------------------------------------------------------------

    /** adds a TEST search from the panel (source "admin"), it expires like any other */
    @PostMapping("/searches")
    public SearchView addTestSearch(@Valid @RequestBody TestSearchRequest request) {
        ModeCategory category = ModeCategory.fromKey(request.mode()).orElseThrow(() ->
                dev.csgogc.mm.web.ApiException.badRequest("unsupported_mode", "unknown mode '" + request.mode() + "'"));
        List<String> maps = request.map() == null || request.map().isBlank() ? List.of() : List.of(request.map());
        SearchRequest search = new SearchRequest(request.accountId(), (long) category.eGame(), category.key(), null, maps, null, null, null);
        return searches.start(search, "admin").search();
    }

    @DeleteMapping("/searches/{id}")
    public SearchView endSearch(@PathVariable long id) {
        return searches.remove(id);
    }
}
