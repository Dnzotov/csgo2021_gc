package dev.csgogc.mm.search;

import dev.csgogc.mm.config.BackendProperties;
import dev.csgogc.mm.fake.FakeSearch;
import dev.csgogc.mm.fake.FakeSearchRepository;
import dev.csgogc.mm.fake.FakeSearchRequest;
import dev.csgogc.mm.fake.FakeSearchView;
import dev.csgogc.mm.fake.FakeSettingsRepository;
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
import org.springframework.transaction.support.TransactionTemplate;

/**
 * The matchmaker. Searches come in from the GC, the matcher places each one on a free game server of its category
 * whose map is one of the maps the player selected:
 * <ul>
 *   <li>classic modes: a search is pointed at an AVAILABLE server that fits and is READY_TO_CONNECT at once; the server
 *       is NOT reserved (several players can be sent to it, it is spread by least recent assignment);</li>
 *   <li>Accept modes (RESEARCH_FINDINGS.md #63): the first search opens a FORMING match that has NO server, compatible
 *       searches (and fake players) join it (MATCHED) until required_players are there (FULL). Only then a free server is
 *       reserved for it, atomically (READY); once its game server armed the roster everybody is WAITING_ACCEPT with the same
 *       server data and the match is ACCEPTING, with a deadline the backend owns. Everybody accepted -> ACCEPTED (the GC
 *       reports it); the deadline passed or a player left -> CANCELLED: the server is free again, players and fake players
 *       search again.</li>
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
    /** a RESERVED server that no live match holds is given back after this long (a crash between the steps of a hand-over) */
    private static final Duration ORPHAN_RESERVATION_GRACE = Duration.ofMinutes(1);

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
                            Instant readyAt, Instant endedAt, boolean awaitingServer, Instant acceptDeadlineAt,
                            Instant acceptedAt) {
    }

    /** what a game server (srcds) asks for: the participants of the match that is on it (GET /api/v1/servers/roster) */
    public record MatchRoster(String matchId, String mode, String status, String map, boolean acceptRequired,
                              int requiredPlayers, int realPlayers, int fakePlayers, List<RosterEntry> players,
                              /** the real state of the match; status is the srcds view (READY while the players accept) */
                              String phase) {
    }

    /** POST /api/v1/matchmaking/accepted: what became of the report of a player that accepted */
    public record AcceptResult(boolean accepted, String reason, boolean matchAccepted, SearchView search) {
    }

    private final SearchRepository searches;
    private final MatchRepository matches;
    private final GameServerRepository servers;
    private final FakeSearchRepository fakes;
    private final FakeSettingsRepository fakeSettings;
    private final BackendProperties props;
    private final Clock clock;
    private final TransactionTemplate tx;

    /** full matches that already logged that no free server fits them (one line per match, not one per second) */
    private final Set<String> waitingForServerLogged = ConcurrentHashMap.newKeySet();

    /** game server id -> when it last asked for its roster: such a server reads the roster from here (RESEARCH_FINDINGS.md #55) */
    private final Map<Long, Instant> rosterPolls = new ConcurrentHashMap<>();
    /** matches whose game server confirmed that it armed the roster */
    private final Set<String> rosterAcks = ConcurrentHashMap.newKeySet();
    /** "match@fill time" that already logged that its fake players wait for the gather window (once, not every second) */
    private final Set<String> fillWaitLogged = ConcurrentHashMap.newKeySet();

    public SearchService(SearchRepository searches, MatchRepository matches, GameServerRepository servers,
                         FakeSearchRepository fakes, FakeSettingsRepository fakeSettings, BackendProperties props, Clock clock,
                         TransactionTemplate tx) {
        this.tx = tx;
        this.fakeSettings = fakeSettings;
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
        boolean partyToSync = false;
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
            partyToSync = true;
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
                finishPartyMembers(current, SearchStatus.CANCELLED);   // another search: the members' searches are rewritten below
                memberLeft(current);
                partyToSync = true;
                log.info("search replaced: account={} {} -> {} maps={}", request.accountId(), current.category(),
                        category.key(), maps);
                outcome = Outcome.REPLACED;
            }
        }

        // a repeated request (same request id) keeps the party it had, unless it names another one
        if (partyToSync || request.partyAccountIds() != null) {
            syncParty(id, request, category, gameMode, maps, variants, source, now);
        }

        runMatcher();
        return new StartResult(outcome, view(searches.findById(id).orElseThrow()));
    }

    /**
     * Gives every other member of the leader's party a search of its own (same mode, maps, kind), placed together with the
     * leader's. A member's client never sends a search: its GC finds this one by account id (GET /matchmaking/account/{id}).
     * A member that had a search of its own leaves it (a player is in one search only).
     */
    private void syncParty(long leaderId, SearchRequest request, ModeCategory category, String gameMode, List<String> maps,
                           List<SearchVariant> variants, String source, Instant now) {
        Set<Long> wanted = new LinkedHashSet<>();
        if (request.partyAccountIds() != null) {
            for (Long account : request.partyAccountIds()) {
                if (account != null && !account.equals(request.accountId())) {
                    wanted.add(account);
                }
            }
        }

        Set<Long> present = new LinkedHashSet<>();
        for (SearchRecord member : searches.findLiveMembers(leaderId)) {
            if (wanted.contains(member.accountId())) {
                present.add(member.accountId());
            } else {
                searches.finish(member.id(), SearchStatus.CANCELLED, now);   // left the party
                memberLeft(member);
            }
        }
        for (Long account : wanted) {
            if (present.contains(account)) {
                continue;
            }
            Optional<SearchRecord> own = searches.findLiveByAccount(account);
            if (own.isPresent()) {
                searches.finish(own.get().id(), SearchStatus.CANCELLED, now);   // it is searched with the party from now on
                finishPartyMembers(own.get(), SearchStatus.CANCELLED);
                memberLeft(own.get());
            }
            long memberId = searches.insertPartyMember(account, request.gameType(), category.key(), gameMode, maps, variants,
                    null, source, leaderId, now);
            searches.setRequestId(memberId, "pt" + memberId);
            log.info("party of search {}: account {} is searched together with account {} (search {})", leaderId, account,
                    request.accountId(), memberId);
        }
    }

    /** the members of a party that ends its search go with it, unless the leader already accepted (its stop is the connect) */
    private void finishPartyMembers(SearchRecord leader, SearchStatus status) {
        if (leader.acceptedAt() != null) {
            return;
        }
        Instant now = clock.instant();
        for (SearchRecord member : searches.findLiveMembers(leader.id())) {
            searches.finish(member.id(), status, now);
            memberLeft(member);
        }
    }

    /**
     * The GC of a player asks for the live search of its account. This is how a party member learns that its leader started
     * a search: the member's client sends nothing itself. Refreshes the search like a poll by request id does.
     */
    public synchronized Optional<SearchView> pollAccount(long accountId) {
        Optional<SearchRecord> found = searches.findLiveByAccount(accountId);
        if (found.isEmpty()) {
            return Optional.empty();
        }
        SearchRecord record = found.get();
        if (record.status() == SearchStatus.SEARCHING || record.status() == SearchStatus.MATCHED) {
            searches.touch(record.id(), clock.instant());
            runMatcher();
        }
        return Optional.of(view(noteAssignmentSeen(searches.findById(record.id()).orElseThrow())));
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
        return Optional.of(view(noteAssignmentSeen(searches.findById(record.id()).orElseThrow())));
    }

    /**
     * The first time a player's GC gets the server data of its match (= its own Match Found, 9107) is recorded and logged: with
     * the accepts it is the answer to "who of the match has been told about it at all".
     */
    private SearchRecord noteAssignmentSeen(SearchRecord record) {
        if (!record.status().isAssigned() || record.assignmentSeenAt() != null || record.matchId() == null) {
            return record;
        }
        searches.markAssignmentSeen(record.id(), clock.instant());
        log.info("match {}: account {} fetched the server data (Match Found on its client){}", record.matchId(), record.accountId(),
                record.isPartyMember() ? " [party member]" : "");
        return searches.findById(record.id()).orElse(record);
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
        finishPartyMembers(current, SearchStatus.CANCELLED);
        memberLeft(current);
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
        finishPartyMembers(record, SearchStatus.REMOVED);
        memberLeft(record);
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
            if (match.status() == MatchStatus.FORMING || match.status() == MatchStatus.FULL
                    || (match.acceptRequired() && (match.status() == MatchStatus.READY || match.status() == MatchStatus.ACCEPTING))) {
                dissolve(match, "its server was taken away");
            } else if (match.status().holdsServer()) {
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
            finishPartyMembers(stale, SearchStatus.EXPIRED);
            memberLeft(stale);
            log.info("search of account {} expired after {}", stale.accountId(), props.staleSearchTimeout());
        }

        // The Accept deadline is owned here (RESEARCH_FINDINGS.md #63): nobody else ends an Accept that ran out of time
        for (MatchRecord due : matches.findAcceptingDue(now)) {
            dissolve(due, "accept timeout, the players did not all accept within " + props.acceptTimeout() + " - "
                    + acceptState(due.id()));
        }

        // a match of an older version: FORMING with a server already reserved. Servers are only reserved for FULL matches now.
        for (MatchRecord legacy : matches.findFormingWithServer()) {
            dissolve(legacy, "it reserved a server while still gathering (older version)");
        }

        // a gathering match nobody is in any more (a restart, a crash between two steps) is not kept around
        for (MatchRecord gathering : matches.findGathering()) {
            if (searches.listLiveByMatch(gathering.id()).isEmpty()) {
                dissolve(gathering, "nobody is in it any more");
            }
        }

        for (SearchRecord assigned : searches.listAssignedBefore(now.minus(props.assignedSearchTimeout()))) {
            searches.finish(assigned.id(), SearchStatus.COMPLETED, now);
        }

        // a server handed out stays RESERVED for the TTL; with no srcds heartbeat yet that is the only way it comes back
        for (MatchRecord ready : matches.findHoldingServerBefore(now.minus(props.serverReservationTtl()))) {
            endMatch(ready.id(), MatchStatus.ENDED, now);
            servers.findById(ready.serverId()).ifPresent(server -> {
                if (ready.id().equals(server.reservedMatchId())) {
                    servers.setState(server.id(), ServerState.AVAILABLE.name(), now);
                    log.info("match {} ended after {}: server {}:{} is AVAILABLE again", ready.id(),
                            props.serverReservationTtl(), server.host(), server.port());
                }
            });
        }

        // a RESERVED server that no live match holds (a hand-over that was cut short) is given back
        for (GameServer reserved : servers.findReservedBefore(now.minus(ORPHAN_RESERVATION_GRACE))) {
            boolean held = reserved.reservedMatchId() != null && matches.findById(reserved.reservedMatchId())
                    .map(m -> m.status().holdsServer() && m.serverId() == reserved.id()).orElse(false);
            if (!held) {
                servers.setState(reserved.id(), ServerState.AVAILABLE.name(), now);
                log.warn("server {}:{} was RESERVED for match {} that does not hold it: AVAILABLE again", reserved.host(),
                        reserved.port(), reserved.reservedMatchId());
            }
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
        applyFakeProfiles();
        reserveServersForFullMatches();
        promoteMatchesWaitingForTheirServer();
    }

    /** a match ends (its virtual players were only a number on the match, a Fake Players profile is a setting: nothing to give back) */
    private void endMatch(String matchId, MatchStatus status, Instant now) {
        matches.end(matchId, status, now);
        rosterAcks.remove(matchId);
        waitingForServerLogged.remove(matchId);
        fillWaitLogged.removeIf(key -> key.startsWith(matchId + "@"));
    }

    private void place(SearchRecord search) {
        ModeCategory category = ModeCategory.fromKey(search.category()).orElse(null);
        if (category == null) {
            return;
        }

        if (category.acceptRequired()) {
            // Accept modes: players gather first, no server is involved until the match is full. A party goes into ONE match:
            // it needs room for all of its members.
            int partySize = 1 + searches.findLiveMembers(search.id()).size();
            for (MatchRecord forming : matches.findForming(category.key())) {
                if (playerCount(forming.id()) + partySize <= forming.requiredPlayers()
                        && joinable(forming, category, search.maps())) {
                    join(search, forming);
                    return;
                }
            }
            if (anyServerCanServe(category, narrow(Optional.empty(), search.maps()))) {
                join(search, createGatheringMatch(category));
            }
            // else: no game server of the category could ever run one of the maps it selected, it keeps searching
            return;
        }

        Optional<Picked> server = pickServer(search, category);
        if (server.isEmpty()) {
            return;
        }

        MatchRecord match = createMatch(server.get());
        join(search, match);
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
        for (GameServer server : servers.findFree(category.key(), clock.instant())) {
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

    // ---- the maps a gathering match can still be played on: what every member accepts. Empty Optional = no restriction yet.

    /** the maps every member of the match accepts, empty Optional when nobody named a map */
    private Optional<Set<String>> mapPool(String matchId) {
        Optional<Set<String>> pool = Optional.empty();
        for (SearchRecord member : searches.listLiveByMatch(matchId)) {
            pool = narrow(pool, member.maps());
        }
        // the Fake Players profile of the match limits the maps as well (the server has to run one of ITS maps too)
        Optional<FakeSearch> profile = matches.findById(matchId).map(MatchRecord::fakeSearchId).flatMap(fakes::findById);
        if (profile.isPresent()) {
            pool = narrow(pool, profile.get().maps());
        }
        return pool;
    }

    /** the pool after a member that accepts {@code maps} joined (no maps = accepts any map = no change) */
    static Optional<Set<String>> narrow(Optional<Set<String>> pool, List<String> maps) {
        if (maps.isEmpty()) {
            return pool;
        }
        Set<String> wanted = new LinkedHashSet<>(maps);
        pool.ifPresent(wanted::retainAll);
        return Optional.of(wanted);
    }

    /** a pool with no map left means the members cannot play together */
    static boolean poolUsable(Optional<Set<String>> pool) {
        return pool.isEmpty() || !pool.get().isEmpty();
    }

    static boolean serverFits(Optional<Set<String>> pool, String serverMap) {
        return pool.isEmpty() || (serverMap != null && !serverMap.isEmpty() && pool.get().contains(serverMap));
    }

    /** some enabled server of the category runs one of the maps (free or not: only whether the match could ever be served) */
    private boolean anyServerCanServe(ModeCategory category, Optional<Set<String>> pool) {
        return anyServerCanServe(category, pool, null);
    }

    /** {@code onlyServer} (a Fake Players profile bound to one game server) limits it to that server */
    private boolean anyServerCanServe(ModeCategory category, Optional<Set<String>> pool, Long onlyServer) {
        return servers.list().stream()
                .anyMatch(server -> server.enabled() && category.key().equals(server.category()) && serverFits(pool, server.map())
                        && (onlyServer == null || onlyServer == server.id()));
    }

    private boolean joinable(MatchRecord forming, ModeCategory category, List<String> maps) {
        Optional<Set<String>> pool = narrow(mapPool(forming.id()), maps);
        return poolUsable(pool) && anyServerCanServe(category, pool);
    }

    /** an Accept match that only gathers: it has no server, and none is reserved for it */
    private MatchRecord createGatheringMatch(ModeCategory category) {
        Instant now = clock.instant();
        String id = "m-" + String.format("%08x", RANDOM.nextInt());
        MatchRecord match = new MatchRecord(id, category.key(), MatchRecord.NO_SERVER, "", 0, "", requiredPlayers(category), true,
                MatchStatus.FORMING, now, null, null, null, null, null, 0, null);
        matches.insert(match);
        log.info("match {} created: {} gathering {} player(s), no server yet", id, category.key(), match.requiredPlayers());
        return match;
    }

    /** a classic mode just points the player at a server that fits; it stays AVAILABLE for the next player */
    private MatchRecord createMatch(Picked picked) {
        ModeCategory category = picked.category();
        Instant now = clock.instant();
        String id = "m-" + String.format("%08x", RANDOM.nextInt());
        servers.markAssigned(picked.server().id(), now);

        MatchRecord match = new MatchRecord(id, category.key(), picked.server().id(), picked.address(), picked.server().port(),
                picked.server().map(), requiredPlayers(category), false, MatchStatus.FORMING, now, null, null, null, null, null, 0, null);
        matches.insert(match);
        log.info("match {} created: {} on {}:{} map={} needs {} player(s), server {} is assigned (classic mode: no reservation)",
                id, category.key(), picked.address(), picked.server().port(), picked.server().map(), match.requiredPlayers(),
                picked.server().id());
        return match;
    }

    private void join(SearchRecord search, MatchRecord match) {
        Instant now = clock.instant();
        searches.place(search.id(), match.id(), SearchStatus.MATCHED, now);
        for (SearchRecord member : searches.findLiveMembers(search.id())) {
            searches.place(member.id(), match.id(), SearchStatus.MATCHED, now);   // the whole party, or nobody
        }
        int players = playerCount(match.id());
        if (players < match.requiredPlayers()) {
            log.info("match {}: account {} joined ({}/{})", match.id(), search.accountId(), players, match.requiredPlayers());
            return;
        }
        completeMatch(match, players);
    }

    /**
     * The match has all its players (real + virtual). A classic match is READY at once: everybody gets the server. An
     * Accept match is FULL and now looks for a free server, see {@link #reserveServer}.
     */
    private void completeMatch(MatchRecord match, int players) {
        Instant now = clock.instant();
        if (!match.acceptRequired()) {
            matches.markReady(match.id(), now);
            assignMembers(match);
            log.info("match {} complete ({}/{}): {} -> {}:{} map={}", match.id(), players, match.requiredPlayers(),
                    SearchStatus.READY_TO_CONNECT, match.serverHost(), match.serverPort(), match.map());
            return;
        }
        matches.markFull(match.id());
        log.info("match {} is full ({}/{}, {} virtual): looking for a free {} server", match.id(), players, match.requiredPlayers(),
                fakeCountOf(match.id()), match.category());
        matches.findById(match.id()).ifPresent(this::reserveServer);
    }

    /** the full matches that had no free server the last time: try again */
    private void reserveServersForFullMatches() {
        for (MatchRecord full : matches.findFull()) {
            reserveServer(full);
        }
    }

    /**
     * FULL -> READY. The one place where an Accept match gets its server. The server is taken with the conditional
     * {@code UPDATE ... WHERE state = 'AVAILABLE'} (GameServerRepository.reserve), together with the match row in one
     * transaction, so two full matches can never hold the same server and a server is never RESERVED without its match.
     * Nothing free that fits: the match stays FULL and is tried again by the next matcher run.
     */
    private void reserveServer(MatchRecord match) {
        if (match.status() != MatchStatus.FULL) {
            return;
        }
        ModeCategory category = ModeCategory.fromKey(match.category()).orElse(null);
        if (category == null) {
            return;
        }
        Instant now = clock.instant();
        Optional<Set<String>> pool = mapPool(match.id());
        Long onlyServer = match.fakeSearchId() == null ? null : fakes.findById(match.fakeSearchId()).map(FakeSearch::serverId).orElse(null);

        for (GameServer server : servers.findFree(category.key(), now)) {
            if (!serverFits(pool, server.map()) || (onlyServer != null && onlyServer != server.id())) {
                continue;
            }
            Optional<String> address = resolveIpv4(server.host());
            if (address.isEmpty()) {
                continue;
            }
            boolean reserved = Boolean.TRUE.equals(tx.execute(status -> {
                if (!servers.reserve(server.id(), match.id(), now)
                        || !matches.assignServer(match.id(), server.id(), address.get(), server.port(), server.map(), now)) {
                    status.setRollbackOnly();
                    return false;
                }
                return true;
            }));
            if (!reserved) {
                continue;   // taken meanwhile: the next candidate
            }
            waitingForServerLogged.remove(match.id());
            log.info("match {}: server {}:{} map={} is RESERVED", match.id(), address.get(), server.port(), server.map());
            matches.findById(match.id()).ifPresent(this::serverReserved);
            return;
        }

        if (waitingForServerLogged.add(match.id())) {
            log.info("match {} is full but no free {} server runs {}: it waits for one", match.id(), category.key(),
                    pool.map(p -> "one of " + p).orElse("a map"));
        }
    }

    /**
     * The match has its server. Everybody gets the server data - except when the game server reads its roster from this
     * backend: then the players wait (MATCHED, match READY) until that server confirmed it armed the roster (or
     * rosterAckTimeout passed), so the reservation check succeeds on the first try.
     */
    private void serverReserved(MatchRecord match) {
        if (serverReadsRoster(match.serverId())) {
            log.info("match {} complete ({}/{}, {} virtual) on {}:{}: waiting for the game server to arm the roster", match.id(),
                    playerCount(match.id()), match.requiredPlayers(), fakeCountOf(match.id()), match.serverHost(),
                    match.serverPort());
            return;
        }
        assignMembers(match);
        log.info("match {} complete ({}/{}, {} virtual): {} -> {}:{} map={}", match.id(), match.requiredPlayers(),
                playerCount(match.id()), match.requiredPlayers(), fakeCountOf(match.id()), SearchStatus.WAITING_ACCEPT, match.serverHost(),
                match.serverPort(), match.map());
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
        if (match.acceptRequired()) {
            startAccepting(match, now);
        }
        return promoted;
    }

    /**
     * READY -> ACCEPTING: the players have the server data, the Accept clock starts. A match with nobody who can accept (only
     * searches added from the admin panel) has nothing to wait for and is ACCEPTED at once.
     */
    private void startAccepting(MatchRecord match, Instant now) {
        if (!matches.markAccepting(match.id(), now.plus(props.acceptTimeout()))) {
            return;   // already accepting
        }
        List<SearchRecord> members = searches.listLiveByMatch(match.id());
        if (members.stream().noneMatch(SearchService::mustAccept)) {
            matches.markAccepted(match.id(), now);
            searches.promoteAccepted(match.id(), now);
            log.info("match {}: nobody has to accept, ACCEPTED", match.id());
            return;
        }
        log.info("match {} is ACCEPTING: {} player(s) have until {} ({})", match.id(), members.size(),
                now.plus(props.acceptTimeout()), props.acceptTimeout());
    }

    /**
     * "accepted 1/3: account 7 accepted, account 8 got Match Found but did not accept, account 9 never fetched the server data":
     * the answer to "who of the match is missing" that the log of an Accept that did not complete needs.
     */
    private String acceptState(String matchId) {
        List<SearchRecord> members = searches.listLiveByMatch(matchId).stream().filter(SearchService::mustAccept).toList();
        long accepted = members.stream().filter(m -> m.acceptedAt() != null).count();
        StringBuilder text = new StringBuilder("accepted ").append(accepted).append('/').append(members.size());
        String separator = ": ";
        for (SearchRecord member : members) {
            text.append(separator).append("account ").append(member.accountId()).append(member.acceptedAt() != null
                    ? " accepted" : member.assignmentSeenAt() != null ? " got Match Found but did not accept"
                    : " never fetched the server data");
            separator = ", ";
        }
        return text.toString();
    }

    /** the real players of the match that have to accept */
    private int mustAcceptCount(String matchId) {
        return (int) searches.listLiveByMatch(matchId).stream().filter(SearchService::mustAccept).count();
    }

    /** how many of them accepted (the match is ACCEPTED, and they connect, only when it is all of them) */
    private int acceptedCount(String matchId) {
        return (int) searches.listLiveByMatch(matchId).stream().filter(SearchService::mustAccept)
                .filter(m -> m.acceptedAt() != null).count();
    }

    /** a search that comes from the GC accepts through it; one added from the admin panel has nobody to accept for it */
    private static boolean mustAccept(SearchRecord search) {
        return "gc".equals(search.source());
    }

    /**
     * The GC of a player says the game server reported everybody accepted (reservation stage 2, awaiting 0): POST
     * /api/v1/matchmaking/accepted. The backend does not take it on trust: the player must be in a match that is ACCEPTING
     * and be one of its members. When every real player of the match reported, the match is ACCEPTED.
     */
    public synchronized AcceptResult accept(long accountId, String requestId) {
        Optional<SearchRecord> existing = searches.findLiveByAccount(accountId);
        if (existing.isEmpty()) {
            return new AcceptResult(false, "no_active_search", false, null);
        }
        SearchRecord current = existing.get();
        if (requestId != null && current.requestId() != null && !requestId.equals(current.requestId())) {
            return new AcceptResult(false, "request_id_mismatch", false, view(current));
        }
        if (current.matchId() == null) {
            return new AcceptResult(false, "not_in_a_match", false, view(current));
        }
        Optional<MatchRecord> found = matches.findById(current.matchId());
        if (found.isEmpty()) {
            return new AcceptResult(false, "not_in_a_match", false, view(current));
        }
        MatchRecord match = found.get();
        if (match.status() == MatchStatus.ACCEPTED && current.acceptedAt() != null) {
            return new AcceptResult(true, "already_accepted", true, view(current));   // a repeated report
        }
        if (match.status() != MatchStatus.ACCEPTING || current.status() != SearchStatus.WAITING_ACCEPT) {
            return new AcceptResult(false, "match_not_accepting", false, view(current));
        }

        Instant now = clock.instant();
        searches.markAccepted(current.id(), now);
        boolean everybody = searches.listLiveByMatch(match.id()).stream()
                .allMatch(member -> !mustAccept(member) || member.id() == current.id() || member.acceptedAt() != null);
        if (everybody) {
            matches.markAccepted(match.id(), now);
            searches.promoteAccepted(match.id(), now);
            log.info("match {}: everybody accepted, ACCEPTED (last: account {})", match.id(), accountId);
        } else {
            log.info("match {}: account {} accepted, the others are still to accept: {}", match.id(), accountId,
                    acceptState(match.id()));
        }
        runMatcher();
        return new AcceptResult(true, null, everybody, view(searches.findById(current.id()).orElseThrow()));
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
                .filter(m -> m.serverId() == server.get().id() && m.status().holdsServer());
        if (match.isEmpty()) {
            return Optional.empty();
        }
        rosterAcks.add(matchId);
        int promoted = match.get().status() == MatchStatus.READY ? assignMembers(match.get()) : 0;
        log.info("match {}: game server {}:{} armed the roster, {} player(s) get the server", matchId, address, port, promoted);
        return Optional.of(promoted);
    }

    /** real players placed in the match + the virtual ones it was given (what a FORMING match is counted by) */
    private int playerCount(String matchId) {
        return searches.listLiveByMatch(matchId).size() + fakeCountOf(matchId);
    }

    private int fakeCountOf(String matchId) {
        return matches.findById(matchId).map(MatchRecord::fakeCount).orElse(0);
    }

    /**
     * what the panel and the GC show as "players": a forming match counts who is waiting now, a complete or finished one
     * everybody it was made of (the client sends a stop when it connects, that must not make the match look smaller)
     */
    private int displayedPlayers(MatchRecord m) {
        return m.status().isGathering() ? playerCount(m.id()) : roster(m).size();
    }

    /**
     * TEST tool, Fake Players (RESEARCH_FINDINGS.md #67). Two separate questions:
     * <ul>
     *   <li>WHO is in the match - the gather window ({@code backend.fake-players.gather-window} or the panel): the real players
     *       (and parties) that search the same mode join one gathering match while it runs; every real player that joins
     *       restarts it;</li>
     *   <li>HOW MANY virtual players it gets - the Fake Players profile of the mode that fits the match: the enabled one with the
     *       highest priority whose maps (and server, if it is bound to one) the match can be played on. The match gets
     *       {@code min(profile count, capacity - real players)}; the capacity of the mode is never changed, the count of the
     *       profile is never "topped up" to it. A count of 0 starts the match with its real players only.</li>
     * </ul>
     * With the master switch off, no enabled profile of the mode or no profile that fits, the match keeps gathering real
     * players like without the tool. A profile is a setting: it is not used up, every match of the mode applies it afresh.
     */
    private void applyFakeProfiles() {
        if (!fakeMasterOn()) {
            return;
        }
        Instant now = clock.instant();
        Duration window = gatherWindow();
        for (ModeCategory category : ModeCategory.values()) {
            if (!category.acceptRequired()) {
                continue;
            }
            for (MatchRecord forming : matches.findForming(category.key())) {
                List<SearchRecord> real = searches.listLiveByMatch(forming.id());
                if (real.isEmpty() || real.size() >= forming.requiredPlayers()) {
                    continue;   // nobody real anchors it (about to be dissolved) / the real players fill it themselves
                }
                Instant lastJoin = real.stream().map(r -> r.matchedAt() != null ? r.matchedAt() : r.startedAt())
                        .max(Comparator.naturalOrder()).orElse(now);
                Instant fillAt = lastJoin.plus(window);
                if (now.isBefore(fillAt)) {
                    if (fillWaitLogged.add(forming.id() + "@" + fillAt.toEpochMilli())) {
                        log.info("match {}: {} real player(s) so far, the virtual players are decided at {} (gather window {}): "
                                + "real players who search until then join this match", forming.id(), real.size(), fillAt, window);
                    }
                    continue;
                }
                Optional<FakeSearch> profile = pickFakeProfile(forming, category);
                if (profile.isEmpty()) {
                    if (fillWaitLogged.add(forming.id() + "@none")) {
                        log.info("match {}: {} real player(s), no enabled Fake Players profile of {} fits it: it keeps waiting for real players",
                                forming.id(), real.size(), category.key());
                    }
                    continue;
                }
                int capacity = forming.requiredPlayers();
                int effective = Math.min(profile.get().players(), capacity - real.size());
                if (!matches.setFake(forming.id(), profile.get().id(), profile.get().players(), effective)) {
                    continue;
                }
                log.info("match {}: {} real + {} virtual player(s) (profile {}: {} configured, {} slots left of {}){}", forming.id(),
                        real.size(), effective, profile.get().id(), profile.get().players(), capacity - real.size(), capacity,
                        effective < profile.get().players() ? ", the rest is not used" : "");
                completeMatch(forming, real.size() + effective);
            }
        }
    }

    /** the enabled profile of the mode with the highest priority that the match can be played with (maps, bound server) */
    private Optional<FakeSearch> pickFakeProfile(MatchRecord forming, ModeCategory category) {
        Optional<Set<String>> base = mapPool(forming.id());
        for (FakeSearch profile : fakes.listEnabled(category.key())) {
            Optional<Set<String>> pool = narrow(base, profile.maps());
            if (poolUsable(pool) && anyServerCanServe(category, pool, profile.serverId())) {
                return Optional.of(profile);
            }
        }
        return Optional.empty();
    }

    /** the tool is on for this backend (property) and the master switch of the panel is not OFF */
    private boolean fakeMasterOn() {
        return props.fakePlayers().enabled() && fakeSettings.get(FakeSettingsRepository.MASTER).map(v -> !"0".equals(v)).orElse(true);
    }

    /** the panel's value when it was set there, the property otherwise */
    private Duration gatherWindow() {
        return fakeSettings.get(FakeSettingsRepository.GATHER_WINDOW_SECONDS).map(v -> {
            try {
                return Duration.ofSeconds(Math.max(0, Long.parseLong(v.trim())));
            } catch (NumberFormatException e) {
                return props.fakePlayers().gatherWindow();
            }
        }).orElse(props.fakePlayers().gatherWindow());
    }

    /**
     * A player who starts a new search has left the server the previous search gave them (a failed Accept / connect and
     * "search again" is the normal case, the client also does not tell the backend when a match is over). When they were
     * alone in that match, an Accept that was still running is cancelled, any other match ends and its server is AVAILABLE
     * again right away instead of after the reservation TTL. A match shared with other players is left alone.
     */
    private void releaseAbandonedMatches(long accountId) {
        Instant now = clock.instant();
        for (String matchId : searches.findMatchIdsByAccount(accountId)) {
            Optional<MatchRecord> found = matches.findById(matchId);
            if (found.isEmpty() || !found.get().status().holdsServer()) {
                continue;
            }
            MatchRecord match = found.get();
            if (searches.listByMatch(matchId).stream().anyMatch(member -> member.accountId() != accountId)) {
                continue;
            }
            if (match.acceptRequired() && match.status() != MatchStatus.ACCEPTED) {
                dissolve(match, "account " + accountId + " started a new search");
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

    /**
     * A participant left the match (cancelled, replaced or expired search). What that means depends on how far it is:
     * gathering - the match only ends when nobody real is left, a full one gathers again; a match whose server is handed out
     * and still to be accepted falls apart (the rest of the players search again); one that was accepted goes on (the player
     * is connecting - the client sends its stop when it does).
     */
    private void memberLeft(SearchRecord leaver) {
        if (leaver.matchId() == null) {
            return;
        }
        matches.findById(leaver.matchId()).ifPresent(match -> {
            switch (match.status()) {
                case FORMING, FULL -> {
                    if (searches.listLiveByMatch(match.id()).isEmpty()) {
                        dissolve(match, "nobody is in it any more");
                    } else {
                        revertIfUnderfilled(match);
                    }
                }
                case READY, ACCEPTING -> {
                    if (match.acceptRequired() && leaver.acceptedAt() == null) {
                        dissolve(match, "account " + leaver.accountId() + " left before the Accept was over");
                    }
                }
                default -> {
                }
            }
        });
    }

    /**
     * A real player left a full match: it gathers again. Its virtual players are decided afresh once the window is over (with the
     * same or another profile, and one real player less to count).
     */
    private void revertIfUnderfilled(MatchRecord match) {
        if (match.status() == MatchStatus.FULL) {
            matches.revertToForming(match.id());
            waitingForServerLogged.remove(match.id());
            log.info("match {} lost a player before it had a server: it gathers again ({} real)", match.id(),
                    searches.listLiveByMatch(match.id()).size());
        }
    }

    /**
     * The match falls apart: real players back to SEARCHING (a new match is made for them), virtual players back to
     * SEARCHING, the server free again. A server whose game server was already told about the match is not handed out
     * for serverReleaseCooldown: it needs that long to drop the reservation.
     */
    private void dissolve(MatchRecord match, String reason) {
        Instant now = clock.instant();
        int members = 0;
        for (SearchRecord member : searches.listLiveByMatch(match.id())) {
            searches.unplace(member.id());
            members++;
        }
        endMatch(match.id(), MatchStatus.CANCELLED, now);
        if (match.hasServer()) {
            boolean handedOut = match.status() == MatchStatus.READY || match.status() == MatchStatus.ACCEPTING;
            Instant availableAfter = handedOut ? now.plus(props.serverReleaseCooldown()) : null;
            servers.findById(match.serverId()).ifPresent(server -> {
                if (match.id().equals(server.reservedMatchId())) {
                    servers.release(server.id(), match.id(), availableAfter, now);
                }
            });
        }
        log.info("match {} cancelled ({}): {} player(s) search again{}", match.id(), reason, members,
                match.hasServer() ? ", server " + match.serverHost() + ":" + match.serverPort() + " is AVAILABLE again" : "");
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
                m.readyAt(), m.endedAt(), awaitingServer(m), m.acceptDeadlineAt(), m.acceptedAt());
    }

    /** complete, but the players are still held until a server is found / the game server confirms its roster */
    private boolean awaitingServer(MatchRecord m) {
        if (m.status() == MatchStatus.FULL) {
            return true;
        }
        return m.status() == MatchStatus.READY && m.acceptRequired()
                && searches.listLiveByMatch(m.id()).stream().anyMatch(r -> r.status() == SearchStatus.MATCHED);
    }

    /**
     * Everybody in the match: the real players (a forming match: who is waiting now, a finished one: everybody it was
     * made of) and the virtual ones, numbered in the order they joined (RosterEntry.fakeAccountId).
     */
    private List<RosterEntry> roster(MatchRecord m) {
        List<SearchRecord> members = m.status().isGathering() ? searches.listLiveByMatch(m.id()) : searches.listByMatch(m.id());
        List<RosterEntry> roster = new ArrayList<>();
        for (Long accountId : new LinkedHashSet<>(members.stream().map(SearchRecord::accountId).toList())) {
            roster.add(new RosterEntry(accountId, false));
        }
        for (int i = 1; i <= m.fakeCount(); i++) {
            roster.add(new RosterEntry(RosterEntry.fakeAccountId(i), true));
        }
        return List.copyOf(roster);
    }

    /**
     * The roster of the match that is on a game server, for the srcds side (GET /api/v1/servers/roster): the newest
     * match that holds the registered server host:port (READY / ACCEPTING / ACCEPTED, all READY to the srcds side). Empty when
     * the server is unknown or has no match - for a server that armed a roster that means the match is gone (cancelled,
     * ended) and its reservation has to be dropped.
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
            return new MatchRoster(m.id(), m.category(), m.status().serverView(), m.map(), m.acceptRequired(), roster.size(),
                    roster.size() - fake, fake, roster, m.status().name());
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
                        m.fakeCount(), m.requiredPlayers(), m.map(), m.serverId(), awaitingServer(m),
                        m.acceptDeadlineAt(), mustAcceptCount(m.id()), acceptedCount(m.id()));
                if (r.status().isAssigned()) {
                    List<RosterEntry> roster = roster(m);
                    int fake = (int) roster.stream().filter(RosterEntry::fake).count();
                    assignment = new SearchView.Assignment(m.id(), m.serverId(), m.serverHost(), m.serverPort(), m.map(),
                            m.acceptRequired(), roster.size(), roster, roster.size() - fake, fake);
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
                assignment,
                r.partyLeaderId());
    }

    // ------------------------------------------------------------------------------------------- fake players (TEST tool)

    /** what the panel shows and edits besides the profiles: the tool (property), its master switch, the gather window */
    public record FakeSettings(boolean available, boolean master, int gatherWindowSeconds) {
    }

    /** backend.fake-players.enabled: the tool exists on this backend at all */
    public boolean fakePlayersEnabled() {
        return props.fakePlayers().enabled();
    }

    public synchronized FakeSettings fakeSettings() {
        return new FakeSettings(props.fakePlayers().enabled(), masterSetting(), (int) gatherWindow().toSeconds());
    }

    private boolean masterSetting() {
        return fakeSettings.get(FakeSettingsRepository.MASTER).map(v -> !"0".equals(v)).orElse(true);
    }

    /** the master switch (Fake Players ON / OFF) and / or the gather window in seconds; null = leave as it is */
    public synchronized FakeSettings updateFakeSettings(Boolean master, Integer gatherWindowSeconds) {
        requireFakePlayers();
        if (gatherWindowSeconds != null && (gatherWindowSeconds < 0 || gatherWindowSeconds > 600)) {
            throw ApiException.badRequest("invalid_gather_window", "the gather window is 0..600 seconds");
        }
        if (master != null) {
            fakeSettings.put(FakeSettingsRepository.MASTER, master ? "1" : "0");
            log.info("Fake Players master switch: {}", master ? "ON" : "OFF");
        }
        if (gatherWindowSeconds != null) {
            fakeSettings.put(FakeSettingsRepository.GATHER_WINDOW_SECONDS, Integer.toString(gatherWindowSeconds));
            log.info("Fake Players gather window: {} s", gatherWindowSeconds);
        }
        fillWaitLogged.clear();
        runMatcher();
        return fakeSettings();
    }

    public synchronized List<FakeSearchView> listFakeSearches() {
        return fakes.list().stream().map(this::fakeView).toList();
    }

    public synchronized FakeSearchView addFakeSearch(FakeSearchRequest request) {
        requireFakePlayers();
        Normalized n = normalizeFake(request);
        boolean enabled = request.enabled() == null || request.enabled();
        long id = fakes.insert(n.category().key(), n.players(), n.maps(), enabled, n.priority(), n.serverId(), clock.instant());
        log.info("fake profile {} added: {} {} virtual player(s) maps={} priority={} server={} enabled={}", id, n.category().key(),
                n.players(), n.maps(), n.priority(), n.serverId(), enabled);
        runMatcher();
        return fakeView(fakes.findById(id).orElseThrow());
    }

    /** a change applies to the matches that are decided from now on; a match that already has its virtual players keeps them */
    public synchronized FakeSearchView updateFakeSearch(long id, FakeSearchRequest request) {
        requireFakePlayers();
        fakes.findById(id).orElseThrow(() -> ApiException.notFound("fake search " + id));
        Normalized n = normalizeFake(request);
        fakes.update(id, n.category().key(), n.players(), n.maps(), n.priority(), n.serverId(), clock.instant());
        log.info("fake profile {} updated: {} {} virtual player(s) maps={} priority={} server={}", id, n.category().key(), n.players(),
                n.maps(), n.priority(), n.serverId());
        fillWaitLogged.clear();
        runMatcher();
        return fakeView(fakes.findById(id).orElseThrow());
    }

    public synchronized FakeSearchView setFakeEnabled(long id, boolean enabled) {
        requireFakePlayers();
        FakeSearch current = fakes.findById(id).orElseThrow(() -> ApiException.notFound("fake search " + id));
        if (current.enabled() != enabled) {
            fakes.setEnabled(id, enabled, clock.instant());
            log.info("fake profile {} {}", id, enabled ? "switched on" : "switched off");
            fillWaitLogged.clear();
            runMatcher();
        }
        return fakeView(fakes.findById(id).orElseThrow());
    }

    public synchronized void deleteFakeSearch(long id) {
        if (!fakes.delete(id)) {
            throw ApiException.notFound("fake search " + id);
        }
        log.info("fake profile {} deleted", id);
        fillWaitLogged.clear();
        runMatcher();
    }

    private void requireFakePlayers() {
        if (!props.fakePlayers().enabled()) {
            throw ApiException.conflict("fake_players_disabled", "the fake players tool is switched off (backend.fake-players.enabled=false)");
        }
    }

    private record Normalized(ModeCategory category, int players, List<String> maps, int priority, Long serverId) {
    }

    /** fake players only make sense for the Accept modes: a match of a classic mode is complete on its own */
    private Normalized normalizeFake(FakeSearchRequest request) {
        ModeCategory category = ModeCategory.fromKey(request.mode()).orElseThrow(() ->
                ApiException.badRequest("unsupported_mode", "unknown mode '" + request.mode() + "'"));
        if (!category.acceptRequired()) {
            throw ApiException.badRequest("fake_mode_unsupported", category.label()
                    + " does not gather players, fake players are for Competitive, Wingman and Danger Zone");
        }
        int max = requiredPlayers(category) - 1;   // at least one real player is needed to start the match
        if (request.players() < 0 || request.players() > max) {
            throw ApiException.badRequest("invalid_players", category.label() + " takes 0-" + max + " fake players");
        }
        List<String> maps = request.maps() == null ? List.of() : List.copyOf(new LinkedHashSet<>(request.maps()));
        Long serverId = request.serverId();
        if (serverId != null) {
            GameServer server = servers.findById(serverId).orElseThrow(() ->
                    ApiException.badRequest("unknown_server", "game server " + serverId + " is not registered"));
            if (!category.key().equals(server.category())) {
                throw ApiException.badRequest("server_mismatch", "game server " + serverId + " serves " + server.category()
                        + ", not " + category.key());
            }
            if (!maps.isEmpty() && !server.map().isEmpty() && !maps.contains(server.map())) {
                throw ApiException.badRequest("server_map_mismatch", "game server " + serverId + " runs " + server.map()
                        + ", which is not one of the profile's maps " + maps);
            }
        }
        return new Normalized(category, request.players(), maps, request.priority() == null ? 0 : request.priority(), serverId);
    }

    private FakeSearchView fakeView(FakeSearch f) {
        ModeCategory category = ModeCategory.fromKey(f.category()).orElseThrow();
        List<FakeSearchView.Used> used = matches.findLiveByFakeProfile(f.id()).stream()
                .map(m -> new FakeSearchView.Used(m.id(), m.status().name(), searches.listLiveByMatch(m.id()).size(), m.fakeCount(),
                        m.fakeConfigured() == null ? m.fakeCount() : m.fakeConfigured(), m.requiredPlayers(), m.serverHost(),
                        m.serverPort(), m.map()))
                .toList();
        String serverLabel = f.serverId() == null ? null : servers.findById(f.serverId())
                .map(server -> server.host() + ":" + server.port() + (server.map().isEmpty() ? "" : " " + server.map()))
                .orElse("game server " + f.serverId() + " (deleted)");
        return new FakeSearchView(f.id(), category.key(), category.label(), f.players(), f.maps(), f.enabled(),
                f.enabled() ? "ON" : "OFF", f.priority(), f.serverId(), serverLabel, requiredPlayers(category) - 1, used, f.createdAt(),
                f.updatedAt());
    }
}
