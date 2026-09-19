package dev.csgogc.mm.search;

public enum MatchStatus {
    /** a server is reserved, players are still being gathered (Accept modes) */
    FORMING,
    /** complete: the server data was issued to every participant */
    READY,
    /** the reservation ended (server released or timed out) */
    ENDED,
    /** dissolved before it was complete */
    CANCELLED
}
