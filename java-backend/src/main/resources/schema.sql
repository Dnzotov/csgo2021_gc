-- executed on every start (SchemaInitializer), everything is idempotent. Times are epoch milliseconds (UTC).
-- Columns added after stage 1 are added by SchemaInitializer.ensureColumn (ALTER TABLE ... ADD COLUMN), so an
-- existing database file keeps working.

CREATE TABLE IF NOT EXISTS game_server (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    host        TEXT    NOT NULL,
    port        INTEGER NOT NULL,
    category    TEXT    NOT NULL,
    map         TEXT    NOT NULL DEFAULT '',
    enabled     INTEGER NOT NULL DEFAULT 1,
    created_at  INTEGER NOT NULL,
    updated_at  INTEGER NOT NULL,
    UNIQUE (host, port)
);

CREATE TABLE IF NOT EXISTS matchmaking_search (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    account_id    INTEGER NOT NULL,
    game_type     INTEGER NOT NULL,
    category      TEXT    NOT NULL,
    game_mode     TEXT    NOT NULL,
    maps          TEXT    NOT NULL DEFAULT '',
    request_id    TEXT,
    status        TEXT    NOT NULL,
    source        TEXT    NOT NULL DEFAULT 'gc',
    started_at    INTEGER NOT NULL,
    last_seen_at  INTEGER NOT NULL,
    ended_at      INTEGER
);

-- a match = one reserved game server + the searches placed on it (1 for the classic modes, required_players for the
-- Accept modes). Players of a FORMING match are still waiting for the others.
CREATE TABLE IF NOT EXISTS matchmaking_match (
    id                TEXT    PRIMARY KEY,
    category          TEXT    NOT NULL,
    server_id         INTEGER NOT NULL,
    server_host       TEXT    NOT NULL,
    server_port       INTEGER NOT NULL,
    map               TEXT    NOT NULL DEFAULT '',
    required_players  INTEGER NOT NULL,
    accept_required   INTEGER NOT NULL DEFAULT 0,
    status            TEXT    NOT NULL,
    created_at        INTEGER NOT NULL,
    ready_at          INTEGER,
    ended_at          INTEGER
);

-- stage 1 had a unique index for status = 'SEARCHING' only; a live search is now any of the four live statuses
DROP INDEX IF EXISTS ux_search_active_account;
CREATE UNIQUE INDEX IF NOT EXISTS ux_search_live_account ON matchmaking_search (account_id)
    WHERE status IN ('SEARCHING', 'MATCHED', 'WAITING_ACCEPT', 'READY_TO_CONNECT');
CREATE INDEX IF NOT EXISTS ix_search_status ON matchmaking_search (status, ended_at);
CREATE INDEX IF NOT EXISTS ix_match_status ON matchmaking_match (status);

-- TEST tool (RESEARCH_FINDINGS.md #54): groups of virtual players the admin adds from the panel. They only fill a match
-- that a real player started (they never start one nor hold a server), so an empty table changes nothing.
CREATE TABLE IF NOT EXISTS fake_search (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    category    TEXT    NOT NULL,
    players     INTEGER NOT NULL,
    maps        TEXT    NOT NULL DEFAULT '',
    enabled     INTEGER NOT NULL DEFAULT 1,
    status      TEXT    NOT NULL,
    match_id    TEXT,
    created_at  INTEGER NOT NULL,
    updated_at  INTEGER NOT NULL,
    matched_at  INTEGER
);
CREATE INDEX IF NOT EXISTS ix_fake_match ON fake_search (match_id);

-- runtime settings edited from the admin panel (RESEARCH_FINDINGS.md #67): the Fake Players master switch, the gather window
CREATE TABLE IF NOT EXISTS backend_setting (
    key    TEXT PRIMARY KEY,
    value  TEXT NOT NULL
);
