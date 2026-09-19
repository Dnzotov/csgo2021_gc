package dev.csgogc.mm.search;

import dev.csgogc.mm.config.BackendProperties;
import dev.csgogc.mm.fake.FakeSearch;
import dev.csgogc.mm.fake.FakeSearchRepository;
import dev.csgogc.mm.fake.FakeSearchRequest;
import dev.csgogc.mm.fake.FakeSearchView;
import dev.csgogc.mm.fake.FakeStatus;
import dev.csgogc.mm.mode.ModeCategory;
import dev.csgogc.mm.server.GameServer;
import dev.csgogc.mm.server.GameServerRepository;
import dev.csgogc.mm.server.ServerState;
import dev.csgogc.mm.web.ApiException;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.security.SecureRandom;
import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.regex.Pattern;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Service;

/**
 * The matchmaker. Searches come in from the GC, the matcher places each one on a free game server of its category
 * whose map is one of the maps the player selected:
 * <ul>
 *   <li>classic modes: a search is pointed at an AVAILABLE server that fits and is READY_TO_CONNECT at once; the server
 *       is NOT reserved (several players can be sent to it, it is spread by least recent assignment);</li>
 *   <li>Accept modes: the first search reserves a server and opens a FORMING match, compatible searches join it
 *       (MATCHED) until required_players are there, then everybody is WAITING_ACCEPT with the same server data.</li>
 * </ul>
 * All state lives in the database (searches, matches, game servers). Every mutation is serialized on this object
 * (single SQLite writer, single backend instance), the matcher runs inside those calls and from a timer.
 * No reservation is made on the game server itself: that is still done by the srcds GC / the C++ GC (stage C).
 */
@Service
public class SearchService {

    private static final Logger log = LoggerFactory.getLogger(SearchService.class);
    private static final long STEAM_ID64_BASE = 76561197960265728L;
    private static final Pattern IPV4 = Pattern.compile("(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})");
    private static final SecureRandom RANDOM = new SecureRandom();

    public enum Outcome {
        /** no live search existed */
        CREATED,
        /** identical search (or a retry with the same request_id): only the timeout was refreshed */
        REFRESHED,
        /** the player changed mode/maps: the live search was rewritten and its clock restarted */
        REPLACED
    }

    public record StartResult(Outcome outcome, SearchView search) {
    }

    public record CancelResult(boolean cancelled, String reason, SearchView search) {
    }

    /**
     * a match for the admin panel; players = real + virtual participants, accountIds are the real ones, roster has both
     */
    public record MatchView(String id, String mode, String modeLabel, long serverId, String serverAddress, int serverPort,
                            String map, String status, int players, int fakePlayers, int requiredPlayers,
                            boolean acceptRequired, List<Long> accountIds, List<RosterEntry> roster, Instant createdAt,
                            Instant readyAt, Instant endedAt, boolean awaitingServer) {
    }

    /** what a game server (srcds) asks for: the participants of the match that is on it (GET /api/v1/servers/roster) */
    public record MatchRoster(String matchId, String mode, String status, String map, boolean acceptRequired,
                              int requiredPlayers, int realPlayers, int fakePlayers, List<RosterEntry> players) {
    }

    private final SearchRepository searches;
    private final MatchRepository matches;
    private final GameServerRepository servers;
    private final FakeSearchRepository fakes;
    private final BackendProperties props;
    private final Clock clock;

    /** game server id -> when it last asked for its roster: such a server reads the roster from here (RESEARCH_FINDINGS.md #55) */
    private final Map<Long, Instant> rosterPolls = new ConcurrentHashMap<>();
    /** matches whose game server confirmed that it armed the roster */
    private final Set<String> rosterAcks = ConcurrentHashMap.newKeySet();

    public SearchService(SearchRepository searches, MatchRepository matches, GameServerRepository servers,
                         FakeSearchRepository fakes, BackendProperties props, Clock clock) {
        this.searches = searches;
        this.matches = matches;
        this.servers = servers;
        this.fakes = fakes;
        this.props = props;
        this.clock = clock;
    }

    // ------------------------------------------------------------------------------------------- search API

    public synchronized StartResult start(SearchRequest request, String source) {
        ModeCategory category = resolveCategory(request);
        String gameMode = request.gameMode() != null ? request.gameMode() : category.srcdsGameMode();
        List<String> maps = request.maps() == null ? List.of() : List.copyOf(request.maps());
        List<SearchVariant> variants = resolveVariants(request, category);
        Instant now = clock.instant();

        Outcome outcome;
        long id;
        Optional<SearchRecord> existing = searches.findLiveByAccount(request.accountId());
        boolean repeat = existing.isPresent() && (request.requestId() != null
                ? request.requestId().equals(existing.get().requestId())
                : existing.get().requestId() == null && sameSearch(existing.get(), request.gameType(), category, maps, variants));
        if (!repeat) {
            releaseAbandonedMatches(request.accountId());   // a new search: the server of the previous one is given up
        }
        if (existing.isEmpty()) {
            id = searches.insert(request.accountId(), request.gameType(), category.key(), gameMode, maps, variants,
                    request.requestId(), source, now);
            log.info("search started: account={} mode={} maps={}{} game_type={} source={}",
                    request.accountId(), category.key(), maps, variants.isEmpty() ? "" : " variants=" + variants,
                    request.gameType(), source);
            outcome = Outcome.CREATED;
        } else {
            SearchRecord current = existing.get();
            id = current.id();
            boolean sameRequest = request.requestId() != null
                    ? request.requestId().equals(current.requestId())
                    : current.requestId() == null && sameSearch(current, request.gameType(), category, maps, variants);
            if (sameRequest) {
                searches.touch(id, now);
                outcome = Outcome.REFRESHED;
            } else {
                searches.replaceLive(id, request.gameType(), category.key(), gameMode, maps, variants, request.requestId(),
                        source, now);
                releaseMatchIfEmpty(current.matchId());
                log.info("search replaced: account={} {} -> {} maps={}", request.accountId(), current.category(),
                        category.key(), maps);
                outcome = Outcome.REPLACED;
            }
        }

        runMatcher();
        return new StartResult(outcome, view(searches.findById(id).orElseThrow()));
    }

    /** the GC polls this: it refreshes the timeout of a live search and gives the current state / assignment */
    public synchronized Optional<SearchView> poll(String requestId) {
        Optional<SearchRecord> found = searches.findByRequestId(requestId);
        if (found.isEmpty()) {
            return Optional.empty();
        }
        SearchRecord record = found.get();
        if (record.status() == SearchStatus.SEARCHING || record.status() == SearchStatus.MATCHED) {
            searches.touch(record.id(), clock.instant());
            runMatcher();
        }
        return Optional.of(view(searches.findById(record.id()).orElseThrow()));
    }

    public synchronized CancelResult cancel(long accountId, String requestId) {
        Optional<SearchRecord> existing = searches.findLiveByAccount(accountId);
        if (existing.isEmpty()) {
            return new CancelResult(false, "no_active_search", null);
        }
        SearchRecord current = existing.get();
        if (requestId != null && current.requestId() != null && !requestId.equals(current.requestId())) {
            // a late cancel of a previous search must not kill the newer one
            return new CancelResult(false, "request_id_mismatch", view(current));
        }
        searches.finish(current.id(), SearchStatus.CANCELLED, clock.instant());
        releaseMatchIfEmpty(current.matchId());
        log.info("search cancelled: account={}", accountId);
        runMatcher();
        return new CancelResult(true, null, view(searches.findById(current.id()).orElseThrow()));
    }

    /** admin: end a search by its row id */
    public synchronized SearchView remove(long id) {
        SearchRecord record = searches.findById(id).orElseThrow(() -> ApiException.notFound("search " + id));
        if (!record.status().isLive()) {
            throw ApiException.conflict("not_searching", "search " + id + " has already ended (" + record.status() + ")");
        }
        searches.finish(id, SearchStatus.REMOVED, clock.instant());
        releaseMatchIfEmpty(record.matchId());
        log.info("search removed by admin: id={} account={}", id, record.accountId());
        runMatcher();
        return view(searches.findById(id).orElseThrow());
    }

    public synchronized List<SearchView> list(boolean includeFinished) {
        tick();
        return searches.list(includeFinished).stream().map(this::view).toList();
    }

    public synchronized List<MatchView> listMatches() {
        return matches.recent(50).stream().map(this::matchView).toList();
    }

    // ------------------------------------------------------------------------------------------- server hooks

    /** a server was added / enabled / changed: a waiting search may fit now */
    public synchronized void serversChanged() {
        runMatcher();
    }

    /**
     * takes a server away from whatever holds it (admin "release", server going BUSY, deletion): a forming match falls
     * apart and its players go back to SEARCHING, a complete one just ends. Does not change the server row itself.
     */
    public synchronized void freeServer(GameServer server) {
        if (server.reservedMatchId() == null) {
            return;
        }
        matches.findById(server.reservedMatchId()).ifPresent(match -> {
            if (match.status() == MatchStatus.FORMING) {
                dissolve(match);
            } else if (match.status() == MatchStatus.READY) {
                endMatch(match.id(), MatchStatus.ENDED, clock.instant());
                log.info("match {} ended, server {}:{} released", match.id(), match.serverHost(), match.serverPort());
            }
        });
    }

    // ------------------------------------------------------------------------------------------- timers

    /** expiry of stale searches, of assigned searches and of server reservations, then the matcher */
    public synchronized void tick() {
        expire();
        runMatcher();
    }

    private void expire() {
        Instant now = clock.instant();

        for (SearchRecord stale : searches.listStale(now.minus(props.staleSearchTimeout()))) {
            searches.finish(stale.id(), SearchStatus.EXPIRED, now);
            releaseMatchIfEmpty(stale.matchId());
            log.info("search of account {} expired after {}", stale.accountId(), props.staleSearchTimeout());
        }

        for (SearchRecord assigned : searches.listAssignedBefore(now.minus(props.assignedSearchTimeout()))) {
            searches.finish(assigned.id(), SearchStatus.COMPLETED, now);
        }

        // a server handed out stays RESERVED for the TTL; with no srcds heartbeat yet that is the only way it comes back
        for (MatchRecord ready : matches.findReadyBefore(now.minus(props.serverReservationTtl()))) {
            endMatch(ready.id(), MatchStatus.ENDED, now);
            servers.findById(ready.serverId()).ifPresent(server -> {
                if (ready.id().equals(server.reservedMatchId())) {
                    servers.setState(server.id(), ServerState.AVAILABLE.name(), now);
                    log.info("match {} ended after {}: server {}:{} is AVAILABLE again", ready.id(),
                            props.serverReservationTtl(), server.host(), server.port());
                }
            });
        }

        Instant cutoff = now.minus(props.finishedSearchRetention());
        searches.deleteFinishedBefore(cutoff);
        matches.deleteEndedBefore(cutoff);
    }

    // ------------------------------------------------------------------------------------------- the matcher

    private void runMatcher() {
        for (SearchRecord search : searches.listUnplaced()) {
            place(search);
        }
        fillFormingMatches();
        promoteMatchesWaitingForTheirServer();
    }

    /** a match ends: its virtual participants finish with it (a match that fell apart re-queues them) */
    private void endMatch(String matchId, MatchStatus status, Instant now) {
        matches.end(matchId, status, now);
        rosterAcks.remove(matchId);
        if (status == MatchStatus.CANCELLED) {
            fakes.unplaceByMatch(matchId, now);
        } else {
            fakes.completeByMatch(matchId, now);
        }
    }

    private void place(SearchRecord search) {
        ModeCategory category = ModeCategory.fromKey(search.category()).orElse(null);
        if (category == null) {
            return;
        }

        if (category.acceptRequired()) {
            // Accept modes: join a match that is still gathering on a map the player accepts
            for (MatchRecord forming : matches.findForming(category.key())) {
                if (mapCompatible(search.maps(), forming.map())) {
                    join(search, forming);
                    return;
                }
            }
        }

        Optional<Picked> server = pickServer(search, category);
        if (server.isEmpty()) {
            return;
        }

        MatchRecord match = createMatch(server.get());
        if (match != null) {
            join(search, match);
        }
    }

    private record Picked(GameServer server, String address, ModeCategory category) {
    }

    /**
     * The server for a search: a plain search wants its own category and maps; a Skirmish search carries the modes the
     * player selected (variants), the best server over all of them wins (least recently assigned).
     */
    private Optional<Picked> pickServer(SearchRecord search, ModeCategory category) {
        if (search.variants().isEmpty()) {
            return pickServer(category, search.maps());
        }
        Picked best = null;
        for (SearchVariant variant : search.variants()) {
            Optional<ModeCategory> target = ModeCategory.fromKey(variant.category());
            if (target.isEmpty()) {
                continue;
            }
            Optional<Picked> candidate = pickServer(target.get(), variant.maps());
            if (candidate.isPresent() && (best == null || Comparator
                    .comparing((Picked p) -> p.server().lastAssignedAt() == null ? Instant.EPOCH : p.server().lastAssignedAt())
                    .thenComparingLong(p -> p.server().id())
                    .compare(candidate.get(), best) < 0)) {
                best = candidate.get();
            }
        }
        return Optional.ofNullable(best);
    }

    /** a free server of the category whose map is one of the requested maps (no maps requested = any map) */
    private Optional<Picked> pickServer(ModeCategory category, List<String> requestedMaps) {
        for (GameServer server : servers.findFree(category.key())) {
            if (!mapCompatible(requestedMaps, server.map())) {
                continue;
            }
            Optional<String> address = resolveIpv4(server.host());
            if (address.isPresent()) {
                return Optional.of(new Picked(server, address.get(), category));
            }
        }
        return Optional.empty();
    }

    /**
     * server.map has to be one of the maps the player selected. A search without maps (the mode's selection is not
     * decodable) fits any server; a server without a map only fits such a search, its map is unknown.
     */
    static boolean mapCompatible(List<String> requestedMaps, String serverMap) {
        if (requestedMaps.isEmpty()) {
            return true;
        }
        return serverMap != null && !serverMap.isEmpty() && requestedMaps.contains(serverMap);
    }

    private MatchRecord createMatch(Picked picked) {
        ModeCategory category = picked.category();
        Instant now = clock.instant();
        String id = "m-" + String.format("%08x", RANDOM.nextInt());
        if (category.acceptRequired()) {
            // reservation belongs to the Accept flow only: the server is held for the match until Accept / connect is over
            if (!servers.reserve(picked.server().id(), id, now)) {
                return null;
            }
        } else {
            // a classic mode just points the player at a server that fits; it stays AVAILABLE for the next player
            servers.markAssigned(picked.server().id(), now);
        }

        MatchRecord match = new MatchRecord(id, category.key(), picked.server().id(), picked.address(), picked.server().port(),
                picked.server().map(), requiredPlayers(category), category.acceptRequired(), MatchStatus.FORMING, now, null, null);
        matches.insert(match);
        log.info("match {} created: {} on {}:{} map={} needs {} player(s), server {} {}", id, category.key(), picked.address(),
                picked.server().port(), picked.server().map(), match.requiredPlayers(), picked.server().id(),
                category.acceptRequired() ? "is RESERVED" : "is assigned (classic mode: no reservation)");
        return match;
    }

    private void join(SearchRecord search, MatchRecord match) {
        searches.place(search.id(), match.id(), SearchStatus.MATCHED, clock.instant());
        int players = playerCount(match.id());
        if (players < match.requiredPlayers()) {
            log.info("match {}: account {} joined ({}/{})", match.id(), search.accountId(), players, match.requiredPlayers());
            return;
        }
        completeMatch(match, players);
    }

    /**
     * The match has all its players (real + virtual). Everybody gets the server data - except when the game server reads
     * its roster from this backend: then the players wait (MATCHED, match READY) until that server confirmed it armed
     * the roster (or rosterAckTimeout passed), so the reservation check succeeds on the first try.
     */
    private void completeMatch(MatchRecord match, int players) {
        Instant now = clock.instant();
        matches.markReady(match.id(), now);
        if (match.acceptRequired() && serverReadsRoster(match.serverId())) {
            log.info("match {} complete ({}/{}, {} virtual) on {}:{}: waiting for the game server to arm the roster", match.id(),
                    players, match.requiredPlayers(), fakes.countPlayersInMatch(match.id()), match.serverHost(),
                    match.serverPort());
            return;
        }
        assignMembers(match);
        log.info("match {} complete ({}/{}, {} virtual): {} -> {}:{} map={}", match.id(), players, match.requiredPlayers(),
                fakes.countPlayersInMatch(match.id()), match.acceptRequired() ? SearchStatus.WAITING_ACCEPT
                        : SearchStatus.READY_TO_CONNECT, match.serverHost(), match.serverPort(), match.map());
    }

    /** the real players of a complete match get the assignment */
    private int assignMembers(MatchRecord match) {
        Instant now = clock.instant();
        SearchStatus status = match.acceptRequired() ? SearchStatus.WAITING_ACCEPT : SearchStatus.READY_TO_CONNECT;
        int promoted = 0;
        for (SearchRecord member : searches.listLiveByMatch(match.id())) {
            if (member.status() != status) {
                searches.place(member.id(), match.id(), status, now);
                promoted++;
            }
        }
        return promoted;
    }

    private boolean serverReadsRoster(long serverId) {
        Instant last = rosterPolls.get(serverId);
        return last != null && !last.plus(props.rosterPollWindow()).isBefore(clock.instant());
    }

    /** complete matches whose players still wait: the server confirmed, timed out, or stopped asking for rosters */
    private void promoteMatchesWaitingForTheirServer() {
        Instant now = clock.instant();
        for (MatchRecord match : matches.findReady()) {
            if (!match.acceptRequired() || searches.listLiveByMatch(match.id()).stream()
                    .noneMatch(m -> m.status() == SearchStatus.MATCHED)) {
                continue;
            }
            String reason = null;
            if (rosterAcks.contains(match.id())) {
                reason = "the game server armed the roster";
            } else if (match.readyAt() != null && !match.readyAt().plus(props.rosterAckTimeout()).isAfter(now)) {
                reason = "the game server did not confirm within " + props.rosterAckTimeout();
            } else if (!serverReadsRoster(match.serverId())) {
                reason = "the game server does not read its roster from the backend";
            }
            if (reason != null) {
                int promoted = assignMembers(match);
                log.info("match {}: {} player(s) get {}:{} ({})", match.id(), promoted, match.serverHost(), match.serverPort(), reason);
            }
        }
    }

    /**
     * The game server says it armed the roster of its match (POST /api/v1/servers/roster/ready): the players get the
     * server. Returns how many searches were promoted. Empty when the server is not registered or that is not its match.
     */
    public synchronized Optional<Integer> confirmRoster(String address, int port, String matchId) {
        Optional<GameServer> server = servers.findByHostPort(address.trim().toLowerCase(java.util.Locale.ROOT), port);
        if (server.isEmpty()) {
            return Optional.empty();
        }
        rosterPolls.put(server.get().id(), clock.instant());
        Optional<MatchRecord> match = matches.findById(matchId)
                .filter(m -> m.serverId() == server.get().id() && (m.status() == MatchStatus.READY || m.status() == MatchStatus.FORMING));
        if (match.isEmpty()) {
            return Optional.empty();
        }
        rosterAcks.add(matchId);
        int promoted = match.get().status() == MatchStatus.READY ? assignMembers(match.get()) : 0;
        log.info("match {}: game server {}:{} armed the roster, {} player(s) get the server", matchId, address, port, promoted);
        return Optional.of(promoted);
    }

    /** real players placed in the match + the virtual ones that joined it (what a FORMING match is counted by) */
    private int playerCount(String matchId) {
        return searches.listLiveByMatch(matchId).size() + fakes.countPlayersInMatch(matchId);
    }

    /**
     * what the panel and the GC show as "players": a forming match counts who is waiting now, a complete or finished one
     * everybody it was made of (the client sends a stop when it connects, that must not make the match look smaller)
     */
    private int displayedPlayers(MatchRecord m) {
        return m.status() == MatchStatus.FORMING ? playerCount(m.id()) : roster(m).size();
    }

    /**
     * TEST tool: enabled fake searches fill matches that a real player has started. They never create a match and never
     * take a server, so with no (enabled) fake search this does nothing at all.
     */
    private void fillFormingMatches() {
        if (!props.fakePlayers().enabled()) {
            return;
        }
        List<FakeSearch> waiting = fakes.listSearching();
        if (waiting.isEmpty()) {
            return;
        }
        for (ModeCategory category : ModeCategory.values()) {
            if (!category.acceptRequired()) {
                continue;
            }
            for (MatchRecord forming : matches.findForming(category.key())) {
                if (searches.listLiveByMatch(forming.id()).isEmpty()) {
                    continue;   // no real player anchors it (it is about to be dissolved)
                }
                for (FakeSearch fake : fakes.listSearching()) {
                    int players = playerCount(forming.id());
                    if (players >= forming.requiredPlayers()) {
                        break;
                    }
                    if (!fake.category().equals(category.key()) || !mapCompatible(fake.maps(), forming.map())
                            || fake.players() > forming.requiredPlayers() - players) {
                        continue;
                    }
                    fakes.place(fake.id(), forming.id(), clock.instant());
                    players += fake.players();
                    log.info("match {}: {} virtual player(s) joined (fake search {}) ({}/{})", forming.id(), fake.players(),
                            fake.id(), players, forming.requiredPlayers());
                    if (players >= forming.requiredPlayers()) {
                        completeMatch(forming, players);
                    }
                }
            }
        }
    }

    /**
     * A player who starts a new search has left the server the previous search gave them (a failed Accept / connect and
     * "search again" is the normal case, the client also does not tell the backend when a match is over). When they were
     * alone in that complete match, the match ends and its server is AVAILABLE again right away instead of after the
     * reservation TTL. A match shared with other players is left alone.
     */
    private void releaseAbandonedMatches(long accountId) {
        Instant now = clock.instant();
        for (String matchId : searches.findMatchIdsByAccount(accountId)) {
            Optional<MatchRecord> found = matches.findById(matchId);
            if (found.isEmpty() || found.get().status() != MatchStatus.READY) {
                continue;
            }
            MatchRecord match = found.get();
            if (searches.listByMatch(matchId).stream().anyMatch(member -> member.accountId() != accountId)) {
                continue;
            }
            endMatch(matchId, MatchStatus.ENDED, now);
            servers.findById(match.serverId()).ifPresent(server -> {
                if (matchId.equals(server.reservedMatchId())) {
                    servers.setState(server.id(), ServerState.AVAILABLE.name(), now);
                }
            });
            log.info("match {} ended: account {} started a new search, server {}:{} is AVAILABLE again", matchId, accountId,
                    match.serverHost(), match.serverPort());
        }
    }

    /** a participant left: if a forming match has nobody left it is dissolved and its server freed */
    private void releaseMatchIfEmpty(String matchId) {
        if (matchId == null) {
            return;
        }
        matches.findById(matchId).ifPresent(match -> {
            if (match.status() == MatchStatus.FORMING && searches.listLiveByMatch(matchId).isEmpty()) {
                dissolve(match);
            }
        });
    }

    /** a forming match falls apart: players back to SEARCHING, server AVAILABLE */
    private void dissolve(MatchRecord match) {
        Instant now = clock.instant();
        for (SearchRecord member : searches.listLiveByMatch(match.id())) {
            searches.unplace(member.id());
        }
        endMatch(match.id(), MatchStatus.CANCELLED, now);
        servers.findById(match.serverId()).ifPresent(server -> {
            if (match.id().equals(server.reservedMatchId())) {
                servers.setState(server.id(), ServerState.AVAILABLE.name(), now);
            }
        });
        log.info("match {} dissolved, server {} is AVAILABLE again", match.id(), match.serverId());
    }

    private int requiredPlayers(ModeCategory category) {
        if (!category.acceptRequired()) {
            return 1;
        }
        Integer override = props.requiredPlayers().get(category.key());
        return override != null ? override : category.requiredPlayers();
    }

    /** the GC needs an IPv4 address (9107 direct_udp_ip), so a hostname of the registry is resolved here */
    private static Optional<String> resolveIpv4(String host) {
        if (IPV4.matcher(host).matches()) {
            return Optional.of(host);
        }
        try {
            for (InetAddress address : InetAddress.getAllByName(host)) {
                if (address instanceof Inet4Address) {
                    return Optional.of(address.getHostAddress());
                }
            }
        } catch (UnknownHostException e) {
            log.warn("cannot resolve the game server host '{}': {}", host, e.getMessage());
        }
        return Optional.empty();
    }

    // ------------------------------------------------------------------------------------------- helpers

    private static boolean sameSearch(SearchRecord current, long gameType, ModeCategory category, List<String> maps,
                                      List<SearchVariant> variants) {
        return current.gameType() == gameType && current.category().equals(category.key()) && current.maps().equals(maps)
                && current.variants().equals(variants);
    }

    /** variants exist for Skirmish only (the client selects modes there); each one is served by its own category */
    private static List<SearchVariant> resolveVariants(SearchRequest request, ModeCategory category) {
        if (request.variants() == null || request.variants().isEmpty()) {
            return List.of();
        }
        if (category != ModeCategory.SKIRMISH) {
            throw ApiException.badRequest("variants_not_allowed",
                    "variants are only for skirmish searches (eGame 12), this one is " + category.key());
        }
        List<SearchVariant> result = new ArrayList<>();
        for (SearchRequest.Variant v : request.variants()) {
            ModeCategory target = ModeCategory.forSkirmishGameMode(v.gameMode());
            result.add(new SearchVariant(v.name(), target.key(), v.maps() == null ? List.of() : List.copyOf(v.maps())));
        }
        return List.copyOf(result);
    }

    private static ModeCategory resolveCategory(SearchRequest request) {
        int eGame = (int) (request.gameType() & 0xF);
        ModeCategory derived = ModeCategory.fromEGame(eGame).orElseThrow(() -> ApiException.badRequest(
                "unsupported_game_type", "game_type " + request.gameType() + " (eGame " + eGame + ") is not an MVP category"));
        if (request.mode() != null) {
            ModeCategory given = ModeCategory.fromKey(request.mode()).orElseThrow(() -> ApiException.badRequest(
                    "unsupported_mode", "unknown mode '" + request.mode() + "'"));
            if (given != derived) {
                throw ApiException.badRequest("mode_mismatch", "mode '" + request.mode() + "' does not match game_type "
                        + request.gameType() + " (eGame " + eGame + " = " + derived.key() + ")");
            }
        }
        return derived;
    }

    private MatchView matchView(MatchRecord m) {
        ModeCategory category = ModeCategory.fromKey(m.category()).orElseThrow();
        List<RosterEntry> roster = roster(m);
        List<Long> real = roster.stream().filter(e -> !e.fake()).map(RosterEntry::accountId).toList();
        int fake = roster.size() - real.size();
        return new MatchView(m.id(), category.key(), category.label(), m.serverId(), m.serverHost(), m.serverPort(), m.map(),
                m.status().name(), roster.size(), fake, m.requiredPlayers(), m.acceptRequired(), real, roster, m.createdAt(),
                m.readyAt(), m.endedAt(), awaitingServer(m));
    }

    /** complete, but the players are still held until the game server confirms its roster */
    private boolean awaitingServer(MatchRecord m) {
        return m.status() == MatchStatus.READY && m.acceptRequired()
                && searches.listLiveByMatch(m.id()).stream().anyMatch(r -> r.status() == SearchStatus.MATCHED);
    }

    /**
     * Everybody in the match: the real players (a forming match: who is waiting now, a finished one: everybody it was
     * made of) and the virtual ones, numbered in the order they joined (RosterEntry.fakeAccountId).
     */
    private List<RosterEntry> roster(MatchRecord m) {
        List<SearchRecord> members = m.status() == MatchStatus.FORMING ? searches.listLiveByMatch(m.id()) : searches.listByMatch(m.id());
        List<RosterEntry> roster = new ArrayList<>();
        for (Long accountId : new LinkedHashSet<>(members.stream().map(SearchRecord::accountId).toList())) {
            roster.add(new RosterEntry(accountId, false));
        }
        int index = 1;
        for (FakeSearch fake : fakes.listByMatch(m.id())) {
            for (int i = 0; i < fake.players(); i++) {
                roster.add(new RosterEntry(RosterEntry.fakeAccountId(index++), true));
            }
        }
        return List.copyOf(roster);
    }

    /**
     * The roster of the match that is on a game server, for the srcds side (GET /api/v1/servers/roster): the newest
     * forming / ready match of the registered server host:port. Empty when the server is unknown or has no match.
     */
    public synchronized Optional<MatchRoster> rosterForServer(String address, int port) {
        Optional<GameServer> server = servers.findByHostPort(address.trim().toLowerCase(java.util.Locale.ROOT), port);
        if (server.isEmpty()) {
            return Optional.empty();
        }
        rosterPolls.put(server.get().id(), clock.instant());   // this server reads its roster from here
        return matches.findActiveByServer(server.get().id()).map(m -> {
            List<RosterEntry> roster = roster(m);
            int fake = (int) roster.stream().filter(RosterEntry::fake).count();
            return new MatchRoster(m.id(), m.category(), m.status().name(), m.map(), m.acceptRequired(), m.requiredPlayers(),
                    roster.size() - fake, fake, roster);
        });
    }

    private SearchView view(SearchRecord r) {
        ModeCategory category = ModeCategory.fromKey(r.category()).orElseThrow();
        Instant end = r.endedAt() != null ? r.endedAt() : clock.instant();
        long seconds = Math.max(0, Duration.between(r.startedAt(), end).toSeconds());

        SearchView.MatchProgress progress = null;
        SearchView.Assignment assignment = null;
        if (r.matchId() != null) {
            Optional<MatchRecord> found = matches.findById(r.matchId());
            if (found.isPresent()) {
                MatchRecord m = found.get();
                progress = new SearchView.MatchProgress(m.id(), m.status().name(), displayedPlayers(m),
                        fakes.countPlayersInMatch(m.id()), m.requiredPlayers(), m.map(), m.serverId(), awaitingServer(m));
                if (r.status().isAssigned()) {
                    List<RosterEntry> roster = roster(m);
                    int fake = (int) roster.stream().filter(RosterEntry::fake).count();
                    assignment = new SearchView.Assignment(m.id(), m.serverId(), m.serverHost(), m.serverPort(), m.map(),
                            m.acceptRequired(), m.requiredPlayers(), roster, roster.size() - fake, fake);
                }
            }
        }

        return new SearchView(
                r.id(),
                r.accountId(),
                Long.toString(STEAM_ID64_BASE + r.accountId()),
                r.gameType(),
                category.eGame(),
                category.key(),
                category.label(),
                category.acceptRequired(),
                r.gameMode(),
                r.maps(),
                r.variants(),
                r.requestId(),
                r.status().name(),
                r.source(),
                r.startedAt(),
                r.lastSeenAt(),
                r.endedAt(),
                seconds,
                progress,
                assignment);
    }

    // ------------------------------------------------------------------------------------------- fake players (TEST tool)

    public boolean fakePlayersEnabled() {
        return props.fakePlayers().enabled();
    }

    public synchronized List<FakeSearchView> listFakeSearches() {
        return fakes.list().stream().map(this::fakeView).toList();
    }

    public synchronized FakeSearchView addFakeSearch(FakeSearchRequest request) {
        requireFakePlayers();
        Normalized n = normalizeFake(request);
        boolean enabled = request.enabled() == null || request.enabled();
        long id = fakes.insert(n.category().key(), n.players(), n.maps(), enabled, clock.instant());
        log.info("fake search {} added: {} virtual {} player(s) maps={} enabled={}", id, n.players(), n.category().key(),
                n.maps(), enabled);
        runMatcher();
        return fakeView(fakes.findById(id).orElseThrow());
    }

    /** change players / mode / maps; not while it sits in a match (stop it first) */
    public synchronized FakeSearchView updateFakeSearch(long id, FakeSearchRequest request) {
        requireFakePlayers();
        FakeSearch current = fakes.findById(id).orElseThrow(() -> ApiException.notFound("fake search " + id));
        if (current.status() == FakeStatus.MATCHED) {
            throw ApiException.conflict("fake_search_in_match", "fake search " + id + " is in match " + current.matchId()
                    + ", stop it before changing it");
        }
        Normalized n = normalizeFake(request);
        fakes.update(id, n.category().key(), n.players(), n.maps(), clock.instant());
        log.info("fake search {} updated: {} virtual {} player(s) maps={}", id, n.players(), n.category().key(), n.maps());
        runMatcher();
        return fakeView(fakes.findById(id).orElseThrow());
    }

    /** enabled = false stops it (leaves a match that is still forming), true starts a fresh search */
    public synchronized FakeSearchView setFakeEnabled(long id, boolean enabled) {
        requireFakePlayers();
        FakeSearch current = fakes.findById(id).orElseThrow(() -> ApiException.notFound("fake search " + id));
        if (enabled) {
            if (current.status() != FakeStatus.MATCHED && current.status() != FakeStatus.SEARCHING) {
                fakes.start(id, clock.instant());
                log.info("fake search {} started", id);
                runMatcher();
            }
        } else if (current.status() != FakeStatus.STOPPED) {
            boolean forming = current.matchId() != null && matches.findById(current.matchId())
                    .map(m -> m.status() == MatchStatus.FORMING).orElse(false);
            fakes.stop(id, forming, clock.instant());
            log.info("fake search {} stopped{}", id, forming ? " (left the forming match " + current.matchId() + ")" : "");
        }
        return fakeView(fakes.findById(id).orElseThrow());
    }

    public synchronized void deleteFakeSearch(long id) {
        if (!fakes.delete(id)) {
            throw ApiException.notFound("fake search " + id);
        }
        log.info("fake search {} deleted", id);
    }

    private void requireFakePlayers() {
        if (!props.fakePlayers().enabled()) {
            throw ApiException.conflict("fake_players_disabled", "the fake players tool is switched off (backend.fake-players.enabled=false)");
        }
    }

    private record Normalized(ModeCategory category, int players, List<String> maps) {
    }

    /** fake players only make sense for the Accept modes: one search of a classic mode is complete on its own */
    private static Normalized normalizeFake(FakeSearchRequest request) {
        ModeCategory category = ModeCategory.fromKey(request.mode()).orElseThrow(() ->
                ApiException.badRequest("unsupported_mode", "unknown mode '" + request.mode() + "'"));
        if (!category.acceptRequired()) {
            throw ApiException.badRequest("fake_mode_unsupported", category.label()
                    + " does not gather players, fake players are for Competitive, Wingman and Danger Zone");
        }
        int max = category.requiredPlayers() - 1;   // at least one real player is needed to start the match
        if (request.players() < 1 || request.players() > max) {
            throw ApiException.badRequest("invalid_players", category.label() + " takes 1-" + max + " fake players per search");
        }
        List<String> maps = request.maps() == null ? List.of() : List.copyOf(new LinkedHashSet<>(request.maps()));
        return new Normalized(category, request.players(), maps);
    }

    private FakeSearchView fakeView(FakeSearch f) {
        ModeCategory category = ModeCategory.fromKey(f.category()).orElseThrow();
        FakeSearchView.Joined joined = null;
        if (f.matchId() != null) {
            Optional<MatchRecord> found = matches.findById(f.matchId());
            if (found.isPresent()) {
                MatchRecord m = found.get();
                joined = new FakeSearchView.Joined(m.id(), m.status().name(), m.serverId(), m.serverHost(), m.serverPort(),
                        m.map(), displayedPlayers(m), m.requiredPlayers());
            }
        }
        return new FakeSearchView(f.id(), category.key(), category.label(), f.players(), f.maps(), f.enabled(),
                f.status().name(), joined, f.createdAt(), f.updatedAt());
    }
}
