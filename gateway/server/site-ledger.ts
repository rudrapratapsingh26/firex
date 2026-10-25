/**
 * The site ledger: the gateway's own signed, hash-chained log, for the events
 * only the gateway witnesses. Alerts sent, acknowledged and timed out, heartbeat
 * gaps, incident labels, drills, registry changes, firmware offered and units
 * removed (ledger SPEC v0.5, section 3).
 *
 * A unit's chain is signed by the unit, and nothing else can extend it. The site
 * chain uses exactly the same entry format, signed by a P-256 key the gateway
 * generates on first start and keeps in its data volume, so the reference
 * verifier checks it unchanged. Its entries name the unit entries they are
 * about by device ID, seq and hash, which ties the two chains together.
 *
 * Appending reads the newest stored entry every time instead of keeping it in
 * memory, so the server and the CLI can both write to the chain; a collision on
 * seq is retried once.
 */
import { createPrivateKey, createPublicKey, generateKeyPairSync, type KeyObject } from 'node:crypto';
import { chmodSync, existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

import { fromHex, toHex, type CborValue } from '../../ledger/src/cbor.ts';
import {
  deviceIdFor, entryFromJson, entryToJson, publicKeyToPem, sha256, signEntry, ZERO32,
  type EntryBody, type EntryJson, type EventType,
} from '../../ledger/src/entry.ts';
import type { Db, Row } from './db.ts';

export type SiteTsSrc = 'ntp' | 'uncertain';

export interface SiteLedgerOptions {
  db: Db;
  /** Directory for the signing key, normally <data>/site-ledger. */
  dir: string;
  siteId: string;
  /** Names the gateway software in every entry's fw_hash. */
  version?: string;
  /** Unix milliseconds. */
  now?: () => number;
  clockSource?: () => SiteTsSrc;
}

export interface SiteBundle {
  kind: 'site';
  site_id: string;
  device_id: string;
  exported_at: string;
  public_key_pem: string;
  log: EntryJson[];
}

/** `FIREX_CLOCK_SOURCE=ntp` once the host's clock is known to be synchronised; `uncertain` otherwise. */
export function clockSourceFromEnv(env: NodeJS.ProcessEnv = process.env): SiteTsSrc {
  return env.FIREX_CLOCK_SOURCE === 'ntp' ? 'ntp' : 'uncertain';
}

function loadOrCreateKey(dir: string): KeyObject {
  mkdirSync(dir, { recursive: true });
  const path = join(dir, 'key.pem');
  if (existsSync(path)) return createPrivateKey(readFileSync(path, 'utf8'));
  const { privateKey } = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
  writeFileSync(path, privateKey.export({ type: 'pkcs8', format: 'pem' }), { mode: 0o600 });
  try {
    chmodSync(path, 0o600);
  } catch {
    /* not supported on every filesystem; the volume permissions still apply */
  }
  return privateKey;
}

export class SiteLedger {
  readonly deviceId: string;
  readonly publicKeyPem: string;
  private readonly db: Db;
  private readonly siteId: string;
  private readonly key: KeyObject;
  private readonly fwHash: Uint8Array;
  private readonly now: () => number;
  private readonly clockSource: () => SiteTsSrc;

  constructor(options: SiteLedgerOptions) {
    this.db = options.db;
    this.siteId = options.siteId;
    this.key = loadOrCreateKey(options.dir);
    const publicKey = createPublicKey(this.key);
    this.deviceId = toHex(deviceIdFor(publicKey));
    this.publicKeyPem = publicKeyToPem(publicKey);
    this.fwHash = sha256(Buffer.from(`firex-server/${options.version ?? process.env.FIREX_GATEWAY_VERSION ?? 'dev'}`, 'utf8'));
    this.now = options.now ?? Date.now;
    this.clockSource = options.clockSource ?? (() => clockSourceFromEnv());
  }

  private last(): { seq: number; ts: number; hash: Uint8Array } | null {
    const row = this.db.prepare('SELECT seq, ts, hash FROM site_entries ORDER BY seq DESC LIMIT 1').get() as Row | undefined;
    return row ? { seq: Number(row.seq), ts: Number(row.ts), hash: fromHex(String(row.hash)) } : null;
  }

  /** Sign and store one entry. Time never runs backwards within the chain: an earlier clock is held at the last entry's ts. */
  append(type: EventType, payload: Record<string, CborValue>): EntryJson {
    for (let attempt = 0; ; attempt++) {
      const last = this.last();
      const body: EntryBody = {
        seq: last ? last.seq + 1 : 0,
        ts: Math.max(Math.floor(this.now() / 1000), last?.ts ?? 0),
        ts_src: this.clockSource(),
        device_id: fromHex(this.deviceId),
        type,
        payload,
        fw_hash: this.fwHash,
        prev_hash: last ? last.hash : ZERO32,
      };
      const json = entryToJson(signEntry(body, this.key));
      try {
        this.db.prepare(
          'INSERT INTO site_entries (seq, ts, ts_src, type, payload, fw_hash, prev_hash, hash, sig, written_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)',
        ).run(json.seq, json.ts, json.ts_src, json.type, JSON.stringify(json.payload), json.fw_hash, json.prev_hash, json.hash, json.sig, this.now());
        return json;
      } catch (err) {
        if (attempt >= 1 || !/UNIQUE|PRIMARY KEY/i.test(String(err))) throw err;
      }
    }
  }

  /** Newest first. */
  entries(options: { limit?: number; beforeSeq?: number; type?: string } = {}): EntryJson[] {
    const rows = this.db.prepare(
      `SELECT * FROM site_entries WHERE seq < ? ${options.type ? 'AND type = ?' : ''} ORDER BY seq DESC LIMIT ?`,
    ).all(...[options.beforeSeq ?? Number.MAX_SAFE_INTEGER, ...(options.type ? [options.type] : []), options.limit ?? 100]) as Row[];
    return rows.map((row) => this.rowToJson(row));
  }

  /** The whole chain, oldest first, in the bundle shape `npm run verify` reads. */
  bundle(): SiteBundle {
    const rows = this.db.prepare('SELECT * FROM site_entries ORDER BY seq').all() as Row[];
    return {
      kind: 'site',
      site_id: this.siteId,
      device_id: this.deviceId,
      exported_at: new Date(this.now()).toISOString(),
      public_key_pem: this.publicKeyPem,
      log: rows.map((row) => this.rowToJson(row)),
    };
  }

  private rowToJson(row: Row): EntryJson {
    const json: EntryJson = {
      seq: Number(row.seq),
      ts: Number(row.ts),
      ts_src: String(row.ts_src),
      device_id: this.deviceId,
      type: String(row.type),
      payload: JSON.parse(String(row.payload)),
      fw_hash: String(row.fw_hash),
      prev_hash: String(row.prev_hash),
      hash: String(row.hash),
      sig: String(row.sig),
    };
    entryFromJson(json); // throws on a row that no longer parses, rather than exporting it
    return json;
  }
}
