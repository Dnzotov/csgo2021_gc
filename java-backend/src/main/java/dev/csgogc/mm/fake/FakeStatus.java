package dev.csgogc.mm.fake;

/**
 * SEARCHING: enabled, waiting for a forming match it fits into. MATCHED: joined a match (forming or complete).
 * STOPPED: switched off by the admin. COMPLETED: the match it joined has ended (Start again re-queues it).
 */
public enum FakeStatus {
    SEARCHING,
    MATCHED,
    STOPPED,
    COMPLETED
}
