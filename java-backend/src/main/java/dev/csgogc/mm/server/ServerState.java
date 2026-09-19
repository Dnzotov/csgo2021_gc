package dev.csgogc.mm.server;

/** Availability of a game server for matchmaking (stored on the backend, not in any client config). */
public enum ServerState {
    /** free: matchmaking may assign it to a match */
    AVAILABLE,
    /** assigned to a match (forming, or ready and handed to the players); set and cleared by the matchmaker */
    RESERVED,
    /** in use / not for matchmaking, set by an admin or by the server itself (heartbeat) */
    BUSY
}
