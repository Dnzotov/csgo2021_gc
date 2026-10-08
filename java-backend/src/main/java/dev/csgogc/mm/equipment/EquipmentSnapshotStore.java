package dev.csgogc.mm.equipment;

import java.time.Clock;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.ConcurrentHashMap;
import org.springframework.stereotype.Component;

/**
 * The equipment snapshots of the players that are in matchmaking. Temporary matchmaking state, deliberately NOT persisted:
 * one snapshot per account (the newest wins), bound to the request_id of the search it was sent for, and removed by
 * SearchService.purgeEquipmentSnapshots as soon as that search / its match is over. A backend restart drops them and the
 * GC sends them again when it registers its search again. Same lifecycle as SkinSnapshotStore.
 */
@Component
public class EquipmentSnapshotStore {

    /** what the store keeps per account */
    public record Stored(long accountId, String requestId, Instant receivedAt, List<EquipmentSnapshotRequest.Entry> entries) {
    }

    private final Map<Long, Stored> byAccount = new ConcurrentHashMap<>();
    private final Clock clock;

    public EquipmentSnapshotStore(Clock clock) {
        this.clock = clock;
    }

    /** stores (replacing) the snapshot, returns the entry; the caller already checked the owner */
    public Stored put(EquipmentSnapshotRequest request) {
        Stored stored = new Stored(request.accountId(), request.requestId(), clock.instant(), List.copyOf(request.entries()));
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
