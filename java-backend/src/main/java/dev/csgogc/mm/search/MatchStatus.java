package dev.csgogc.mm.search;

/**
 * The life of a match. An Accept match (Competitive / Wingman / Danger Zone) goes
 * FORMING -> FULL -> READY -> ACCEPTING -> ACCEPTED -> ENDED, and falls to CANCELLED from any live state before ACCEPTED
 * (RESEARCH_FINDINGS.md #63). A classic match (Casual, Deathmatch, ...) is FORMING for an instant and READY until ENDED:
 * it needs no gathering, no reservation and no Accept.
 */
public enum MatchStatus {
    /** GATHERING: players (real and virtual) are being collected. No game server is involved, none is reserved. */
    FORMING,
    /** every required player is there, a free game server is being looked for (still no server, players wait) */
    FULL,
    /**
     * RESERVED: a game server is reserved for the match (Accept) / assigned to it (classic). Accept matches: the players
     * are held until the game server confirmed that it armed the roster, then they get the assignment.
     */
    READY,
    /** the assignment was issued, the players accept; the backend owns the deadline (accept_deadline_at) */
    ACCEPTING,
    /** every real player accepted, the players connect (ACCEPTED is not CONNECTED: nothing after this is tracked yet) */
    ACCEPTED,
    /** the reservation ended (server released or timed out) */
    ENDED,
    /** fell apart before it was accepted: players and virtual players are searching again, the server is free again */
    CANCELLED;

    /** the match still exists as a plan or as a reservation (not ENDED / CANCELLED) */
    public boolean isLive() {
        return this != ENDED && this != CANCELLED;
    }

    /** a game server is reserved / assigned for it */
    public boolean holdsServer() {
        return this == READY || this == ACCEPTING || this == ACCEPTED;
    }

    /** the states before a server is involved: players are still being collected */
    public boolean isGathering() {
        return this == FORMING || this == FULL;
    }

    /**
     * What the srcds side sees (GET /servers/roster, csgo_gc server_roster.h Snapshot::Complete()): a roster that is
     * complete is READY however far the Accept is, so a game server that armed the roster keeps it while the players accept.
     */
    public String serverView() {
        return holdsServer() ? READY.name() : name();
    }

    /** SQL list of the live states */
    public static final String LIVE_SQL = "('FORMING','FULL','READY','ACCEPTING','ACCEPTED')";
    /** SQL list of the states with a server */
    public static final String SERVER_SQL = "('READY','ACCEPTING','ACCEPTED')";
}
