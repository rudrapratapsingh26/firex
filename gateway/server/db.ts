/**
 * SQLite through Node's built-in node:sqlite: no native module to compile on
 * the Pi and nothing to audit beyond Node itself (docs/adr/0005). All access
 * goes through this file and the modules that import it, so moving to
 * better-sqlite3 later touches one layer.
 *
 * Timestamps written by the gateway are Unix milliseconds; ledger `ts` values
 * stay in the seconds the node signed.
 */
import { createRequire } from 'node:module';
import type { DatabaseSync as DatabaseSyncType } from 'node:sqlite';

/* Loaded at run time rather than imported: Vite 5, under Vitest, does not know
 * node:sqlite is a built-in and tries to resolve a package called "sqlite". */
const { DatabaseSync } = createRequire(import.meta.url)('node:sqlite') as typeof import('node:sqlite');

export type Db = DatabaseSyncType;
export type Row = Record<string, unknown>;

const MIGRATIONS: string[] = [
  `
  CREATE TABLE sites (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    address TEXT NOT NULL DEFAULT '',
    timezone TEXT NOT NULL DEFAULT 'Asia/Kolkata',
    floor_plan BLOB,
    floor_plan_type TEXT
  );
  CREATE TABLE devices (
    id TEXT PRIMARY KEY,
    site_id TEXT NOT NULL REFERENCES sites(id),
    label TEXT NOT NULL DEFAULT '',
    floor_x REAL,
    floor_y REAL,
    ledger_pub TEXT NOT NULL,
    tls_pub TEXT NOT NULL,
    cert_serial TEXT,
    fw_version TEXT,
    boot_count INTEGER,
    provisioned_at INTEGER NOT NULL,
    last_seen INTEGER,
    last_state TEXT,
    last_alarm TEXT,
    faults TEXT,
    epoch INTEGER NOT NULL DEFAULT 0,
    chain_status TEXT NOT NULL DEFAULT 'empty',
    last_seq INTEGER,
    last_hash TEXT,
    last_ts INTEGER,
    resume_seq INTEGER,
    last_self_test TEXT,
    last_calibration_ts INTEGER
  );
  CREATE TABLE pairings (
    code TEXT PRIMARY KEY,
    site_id TEXT NOT NULL REFERENCES sites(id),
    label TEXT NOT NULL DEFAULT '',
    floor_x REAL,
    floor_y REAL,
    created_by TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    used_at INTEGER,
    device_id TEXT
  );
  CREATE TABLE ledger_entries (
    device_id TEXT NOT NULL,
    epoch INTEGER NOT NULL,
    seq INTEGER NOT NULL,
    ts INTEGER NOT NULL,
    ts_src TEXT NOT NULL,
    type TEXT NOT NULL,
    payload TEXT NOT NULL,
    fw_hash TEXT NOT NULL,
    prev_hash TEXT NOT NULL,
    hash TEXT NOT NULL,
    sig TEXT NOT NULL,
    received_at INTEGER NOT NULL,
    verify_result TEXT NOT NULL,
    PRIMARY KEY (device_id, epoch, seq)
  );
  CREATE INDEX ledger_by_type ON ledger_entries (device_id, epoch, type);
  CREATE INDEX ledger_by_received ON ledger_entries (received_at);
  CREATE TABLE chain_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    epoch INTEGER NOT NULL,
    at INTEGER NOT NULL,
    kind TEXT NOT NULL,
    from_seq INTEGER,
    to_seq INTEGER,
    detail TEXT NOT NULL DEFAULT ''
  );
  CREATE TABLE telemetry (
    device_id TEXT NOT NULL,
    minute INTEGER NOT NULL,
    scores TEXT NOT NULL,
    fan TEXT,
    PRIMARY KEY (device_id, minute)
  );
  CREATE TABLE media (
    sha256 TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    size INTEGER NOT NULL,
    received_at INTEGER NOT NULL,
    deleted_at INTEGER
  );
  CREATE TABLE media_refs (
    device_id TEXT NOT NULL,
    epoch INTEGER NOT NULL,
    seq INTEGER NOT NULL,
    idx INTEGER NOT NULL,
    sha256 TEXT NOT NULL,
    PRIMARY KEY (device_id, epoch, seq, idx)
  );
  CREATE INDEX media_refs_by_sha ON media_refs (sha256);
  CREATE TABLE incidents (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    epoch INTEGER NOT NULL,
    site_id TEXT NOT NULL,
    start_seq INTEGER NOT NULL,
    start_ts INTEGER NOT NULL,
    end_seq INTEGER,
    end_ts INTEGER,
    reason TEXT NOT NULL,
    label TEXT,
    labelled_by TEXT,
    labelled_at INTEGER,
    note TEXT
  );
  CREATE TABLE users (
    username TEXT PRIMARY KEY,
    role TEXT NOT NULL,
    salt TEXT NOT NULL,
    hash TEXT NOT NULL,
    created_at INTEGER NOT NULL
  );
  CREATE TABLE sessions (
    token_hash TEXT PRIMARY KEY,
    username TEXT NOT NULL REFERENCES users(username) ON DELETE CASCADE,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL
  );
  CREATE TABLE audit (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    at INTEGER NOT NULL,
    actor TEXT NOT NULL,
    action TEXT NOT NULL,
    detail TEXT NOT NULL DEFAULT '{}'
  );
  `,
  /* M4: what the escalation runner did for each incident. Ledger entries for these
   * events come in M5; until then this table is the record. */
  `
  CREATE TABLE alert_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    incident_id INTEGER NOT NULL REFERENCES incidents(id),
    at_ms INTEGER NOT NULL,
    type TEXT NOT NULL,
    service TEXT NOT NULL,
    receipt_id TEXT,
    payload TEXT NOT NULL DEFAULT '{}'
  );
  CREATE INDEX alert_events_by_incident ON alert_events (incident_id, at_ms);
  `,
  /* M5: the site ledger, the gateway's own signed chain (site-ledger.ts, SPEC v0.5). */
  `
  CREATE TABLE site_entries (
    seq INTEGER PRIMARY KEY,
    ts INTEGER NOT NULL,
    ts_src TEXT NOT NULL,
    type TEXT NOT NULL,
    payload TEXT NOT NULL,
    fw_hash TEXT NOT NULL,
    prev_hash TEXT NOT NULL,
    hash TEXT NOT NULL,
    sig TEXT NOT NULL,
    written_at INTEGER NOT NULL
  );
  CREATE INDEX site_entries_by_type ON site_entries (type, seq);
  `,
  /* M5: firmware and its rollouts, incident traces, unit removal, and what each
   * unit's hello says about its clock and its OTA slot. */
  `
  ALTER TABLE devices ADD COLUMN removed_at INTEGER;
  ALTER TABLE devices ADD COLUMN clock TEXT;
  ALTER TABLE devices ADD COLUMN ota TEXT;
  ALTER TABLE devices ADD COLUMN hello_next_seq INTEGER;
  CREATE TABLE firmware (
    sha256 TEXT PRIMARY KEY,
    version TEXT NOT NULL,
    project TEXT NOT NULL,
    size INTEGER NOT NULL,
    signature TEXT NOT NULL,
    uploaded_by TEXT NOT NULL,
    uploaded_at INTEGER NOT NULL
  );
  CREATE TABLE rollouts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sha256 TEXT NOT NULL,
    state TEXT NOT NULL,
    started_at INTEGER NOT NULL,
    finished_at INTEGER,
    by TEXT NOT NULL
  );
  CREATE TABLE rollout_units (
    rollout_id INTEGER NOT NULL,
    device_id TEXT NOT NULL,
    position INTEGER NOT NULL,
    state TEXT NOT NULL,
    detail TEXT NOT NULL DEFAULT '',
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (rollout_id, device_id)
  );
  CREATE TABLE traces (
    sha256 TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    epoch INTEGER,
    seq INTEGER,
    samples INTEGER NOT NULL,
    period_ms INTEGER NOT NULL,
    pre_samples INTEGER NOT NULL,
    trigger TEXT NOT NULL,
    trigger_uptime_ms INTEGER NOT NULL,
    boot_count INTEGER NOT NULL,
    size INTEGER NOT NULL,
    received_at INTEGER NOT NULL
  );
  CREATE INDEX traces_by_device ON traces (device_id, received_at);
  `,
  /* M5: live escalation. Drills and what they sent, heartbeat gaps, SMS replies, and
   * how each incident's escalation went (escalate.ts, heartbeat.ts). */
  `
  CREATE TABLE drills (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    started_at_ms INTEGER NOT NULL,
    finished_at_ms INTEGER,
    by TEXT NOT NULL,
    services TEXT NOT NULL
  );
  CREATE TABLE drill_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    drill_id INTEGER NOT NULL REFERENCES drills(id),
    at_ms INTEGER NOT NULL,
    type TEXT NOT NULL,
    service TEXT NOT NULL,
    receipt_id TEXT,
    payload TEXT NOT NULL DEFAULT '{}'
  );
  CREATE INDEX drill_events_by_drill ON drill_events (drill_id, at_ms);
  CREATE TABLE heartbeat_gaps (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    start_ms INTEGER NOT NULL,
    end_ms INTEGER,
    notified_at_ms INTEGER
  );
  CREATE INDEX heartbeat_gaps_by_device ON heartbeat_gaps (device_id, start_ms);
  CREATE TABLE sms_inbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    received_at_ms INTEGER NOT NULL,
    sender TEXT NOT NULL,
    body TEXT NOT NULL,
    matched_receipt TEXT
  );
  ALTER TABLE incidents ADD COLUMN late INTEGER NOT NULL DEFAULT 0;
  ALTER TABLE incidents ADD COLUMN escalation_state TEXT;
  ALTER TABLE incidents ADD COLUMN escalation_reason TEXT;
  `,
];

export function openDb(path: string): Db {
  const db = new DatabaseSync(path);
  db.exec('PRAGMA foreign_keys = ON; PRAGMA busy_timeout = 5000;');
  if (path !== ':memory:') {
    db.exec('PRAGMA journal_mode = WAL;');
  }
  migrate(db);
  return db;
}

function migrate(db: Db): void {
  db.exec('CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)');
  const row = db.prepare("SELECT value FROM meta WHERE key = 'schema_version'").get() as Row | undefined;
  let version = row ? Number(row.value) : 0;
  while (version < MIGRATIONS.length) {
    transaction(db, () => {
      db.exec(MIGRATIONS[version]);
      version += 1;
      db.prepare("INSERT INTO meta (key, value) VALUES ('schema_version', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value")
        .run(String(version));
    });
  }
}

export function transaction<T>(db: Db, fn: () => T): T {
  db.exec('BEGIN IMMEDIATE');
  try {
    const result = fn();
    db.exec('COMMIT');
    return result;
  } catch (err) {
    db.exec('ROLLBACK');
    throw err;
  }
}

export function audit(db: Db, actor: string, action: string, detail: Record<string, unknown> = {}, at = Date.now()): void {
  db.prepare('INSERT INTO audit (at, actor, action, detail) VALUES (?, ?, ?, ?)').run(at, actor, action, JSON.stringify(detail));
}

export function ensureSite(db: Db, id: string, name: string): void {
  db.prepare('INSERT INTO sites (id, name) VALUES (?, ?) ON CONFLICT(id) DO NOTHING').run(id, name);
}
