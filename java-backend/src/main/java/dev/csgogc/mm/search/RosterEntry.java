package dev.csgogc.mm.search;

/**
 * A participant of a match. Fake (virtual test) participants get deterministic account ids {@code 0xFA4E0000 + n}
 * (n = 1, 2, ... in the order they joined the match) - the same scheme the srcds test roster uses
 * (csgo_gc/test_accept.h FakeAccountBase), so a srcds that is given this roster produces the same reservation.
 */
public record RosterEntry(long accountId, boolean fake) {

    public static final long FAKE_ACCOUNT_BASE = 0xFA4E0000L;

    public static long fakeAccountId(int index) {
        return FAKE_ACCOUNT_BASE + index;
    }
}
