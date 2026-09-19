package dev.csgogc.mm.search;

public enum SearchStatus {
    /** in the queue, no server yet */
    SEARCHING,
    /** Accept modes: placed in a forming match on a reserved server, the other players are still missing */
    MATCHED,
    /** Accept modes: the match is complete and the server data was issued, the retail Accept phase is running */
    WAITING_ACCEPT,
    /** classic modes: a server was assigned, the client connects to it */
    READY_TO_CONNECT,
    /** the GC reported the player stopped searching (MatchmakingStop) */
    CANCELLED,
    /** not refreshed within backend.stale-search-timeout */
    EXPIRED,
    /** ended by an admin */
    REMOVED,
    /** an assigned search (WAITING_ACCEPT / READY_TO_CONNECT) that ran out backend.assigned-search-timeout */
    COMPLETED;

    /** the states a player can be in while a search exists; only one live search per account */
    public boolean isLive() {
        return this == SEARCHING || this == MATCHED || this == WAITING_ACCEPT || this == READY_TO_CONNECT;
    }

    /** the server data was handed out */
    public boolean isAssigned() {
        return this == WAITING_ACCEPT || this == READY_TO_CONNECT;
    }

    /** SQL list of the live states, for queries */
    public static final String LIVE_SQL = "('SEARCHING','MATCHED','WAITING_ACCEPT','READY_TO_CONNECT')";
}
