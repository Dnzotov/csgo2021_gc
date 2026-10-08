package dev.csgogc.mm.skin;

import java.time.Clock;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;
import org.springframework.stereotype.Component;

/**
 * The equipped-skin snapshots of the players that are in matchmaking. Temporary matchmaking state, deliberately NOT
 * persisted: one snapshot per account (the newest wins), bound to the request_id of the search it was sent for, and
 * removed by SearchService.purgeSkinSnapshots as soon as that search / its match is over. A backend restart drops them
 * and the GC sends them again when it registers its search again.
 */
@Component
public class SkinSnapshotStore {

    /** what the store keeps per account */
    public record Stored(long accountId, String requestId, Instant receivedAt, List<SkinSnapshotRequest.Item> items) {
    }

    private final Map<Long, Stored> byAccount = new ConcurrentHashMap<>();
    private final Clock clock;

    public SkinSnapshotStore(Clock clock) {
        this.clock = clock;
    }

    /** stores (replacing) the snapshot, returns the entry; the caller already checked the owner */
    public Stored put(SkinSnapshotRequest request) {
        Stored stored = new Stored(request.accountId(), request.requestId(), clock.instant(), List.copyOf(request.items()));
        byAccount.put(stored.accountId(), stored);
        return stored;
    }

    public Optional<Stored> get(long accountId) {
        return Optional.ofNullable(byAccount.get(accountId));
    }

    public boolean remove(long accountId) {
        return byAccount.remove(accountId) != null;
    }

    public List<Stored> all() {
        return new ArrayList<>(byAccount.values());
    }

    public int size() {
        return byAccount.size();
    }
}
