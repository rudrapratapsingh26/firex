/**
 * Ingest: everything a node publishes, verified before it is acknowledged.
 *
 * Ledger entries are checked in order against the last stored entry of the
 * device's current epoch: sequence, prev_hash link, recomputed hash, and the
 * signature under the ledger key registered at provisioning. Every entry is
 * stored with its result, including the ones that fail: an inspector needs to
 * see the tampered record, not its absence. See docs/transport.md.
 *
 * MQTT is not imported here. Messages come in through handle(); commands go
 * out through the injected publish function, which is what makes this
 * testable without a broker.
 */
import { createHash } from 'node:crypto';
import { mkdirSync, renameSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';

import { toHex } from '../../ledger/src/cbor.ts';
import { entryFromJson, preimage, sha256, verifySignature, type Entry, type EntryJson } from '../../ledger/src/entry.ts';
import { audit, transaction, type Db, type Row } from './db.ts';
import type { LiveBus } from './live.ts';
import { p256PointToKey } from './pairing.ts';
import { linkTraceEntry, storeTrace, TRACE_MAX_BYTES } from './traces.ts';

export type Publish = (topic: string, payload: string, options: { qos: 0 | 1; retain?: boolean }) => void;
export type VerifyResult = 'ok' | 'start' | 'gap' | 'chain' | 'signature';

export interface IngestOptions {
  db: Db;
  mediaDir: string;
  /** Episode traces; beside the media directory unless given. */
  tracesDir?: string;
  publish: Publish;
  live: LiveBus;
  now?: () => number;
  /** A unit's fw_update entry was stored: the rollout follows the unit's own record. */
  onFwUpdate?: (deviceId: string, payload: Record<string, unknown>) => void;
}

const MAX_MEDIA_BYTES = 2 * 1024 * 1024;
const MAX_PENDING = 256;
const RESEND_INTERVAL_MS = 5000;
const ZERO_HASH = '0'.repeat(64);
/** A unit counts as connected for the hourly time command if it was heard from this recently. */
const CONNECTED_MS = 90_000;

interface Device {
  id: string;
  site_id: string;
  ledger_pub: string;
  epoch: number;
  chain_status: string;
  last_seq: number | null;
  last_hash: string | null;
  resume_seq: number | null;
  removed_at: number | null;
}

export function parseTopic(topic: string): { site: string; device: string; kind: string; arg?: string } | null {
  const parts = topic.split('/');
  if (parts[0] !== 'firex' || parts.length < 4 || parts.length > 5) return null;
  const [, site, device, kind, arg] = parts;
  if (!/^[0-9a-f]{32}$/.test(device)) return null;
  return { site, device, kind, arg };
}

export function cmdTopic(site: string, device: string): string {
  return `firex/${site}/${device}/cmd`;
}

export class Ingest {
  private readonly db: Db;
  private readonly mediaDir: string;
  private readonly tracesDir: string;
  private readonly publish: Publish;
  private readonly live: LiveBus;
  private readonly now: () => number;
  private readonly onFwUpdate?: (deviceId: string, payload: Record<string, unknown>) => void;
  private readonly pending = new Map<string, Map<number, EntryJson>>();
  private readonly lastResend = new Map<string, number>();

  constructor(options: IngestOptions) {
    this.db = options.db;
    this.mediaDir = options.mediaDir;
    this.tracesDir = options.tracesDir ?? join(dirname(options.mediaDir), 'traces');
    this.publish = options.publish;
    this.live = options.live;
    this.now = options.now ?? Date.now;
    this.onFwUpdate = options.onFwUpdate;
  }

  /** Route one MQTT message. Returns false when it was ignored. */
  handle(topic: string, payload: Buffer): boolean {
    const t = parseTopic(topic);
    if (!t) return false;
    const device = this.device(t.device);
    if (!device || device.site_id !== t.site) {
      audit(this.db, 'ingest', 'unknown_device_message', { topic }, this.now());
      return false;
    }
    if (device.removed_at !== null) {
      /* The broker refuses its certificate once Mosquitto has reloaded the CRL; until then, nothing it sends is taken. */
      audit(this.db, 'ingest', 'removed_device_message', { topic }, this.now());
      return false;
    }
    try {
      switch (t.kind) {
        case 'hello': this.onHello(device, JSON.parse(payload.toString('utf8'))); return true;
        case 'state': this.onState(device, JSON.parse(payload.toString('utf8'))); return true;
        case 'telemetry': this.onTelemetry(device, JSON.parse(payload.toString('utf8'))); return true;
        case 'ledger': this.onLedger(device, JSON.parse(payload.toString('utf8')) as EntryJson); return true;
        case 'media': return t.arg ? this.onMedia(device, t.arg, payload) : false;
        case 'trace': return t.arg ? this.onTrace(device, t.arg, payload) : false;
        default: return false;
      }
    } catch (err) {
      audit(this.db, 'ingest', 'malformed_message', { topic, error: String(err) }, this.now());
      return false;
    }
  }

  private device(id: string): Device | null {
    const row = this.db.prepare(
      'SELECT id, site_id, ledger_pub, epoch, chain_status, last_seq, last_hash, resume_seq, removed_at FROM devices WHERE id = ?',
    ).get(id) as Row | undefined;
    if (!row) return null;
    return {
      id: String(row.id),
      site_id: String(row.site_id),
      ledger_pub: String(row.ledger_pub),
      epoch: Number(row.epoch),
      chain_status: String(row.chain_status),
      last_seq: row.last_seq === null ? null : Number(row.last_seq),
      last_hash: row.last_hash === null ? null : String(row.last_hash),
      resume_seq: row.resume_seq === null ? null : Number(row.resume_seq),
      removed_at: row.removed_at === null || row.removed_at === undefined ? null : Number(row.removed_at),
    };
  }

  private send(device: Device, cmd: Record<string, unknown>): void {
    this.publish(cmdTopic(device.site_id, device.id), JSON.stringify(cmd), { qos: 1 });
  }

  private chainEvent(device: Device, kind: string, from: number | null, to: number | null, detail = ''): void {
    this.db.prepare('INSERT INTO chain_events (device_id, epoch, at, kind, from_seq, to_seq, detail) VALUES (?, ?, ?, ?, ?, ?, ?)')
      .run(device.id, device.epoch, this.now(), kind, from, to, detail);
    this.live.publish({ kind: 'chain', device_id: device.id, status: kind, detail });
  }

  onHello(device: Device, msg: {
    fw_version?: string; boot_count?: number; next_seq?: number; oldest_seq?: number;
    clock?: string; ota?: { partition?: string; pending_verify?: boolean };
  }): void {
    const now = this.now();
    const clock = msg.clock === 'gateway' || msg.clock === 'uncertain' ? msg.clock : null;
    const ota = msg.ota && typeof msg.ota === 'object'
      ? JSON.stringify({ partition: String(msg.ota.partition ?? ''), pending_verify: Boolean(msg.ota.pending_verify) })
      : null;
    this.db.prepare(
      `UPDATE devices SET fw_version = COALESCE(?, fw_version), boot_count = COALESCE(?, boot_count), last_seen = ?,
         clock = COALESCE(?, clock), ota = COALESCE(?, ota), hello_next_seq = ? WHERE id = ?`,
    ).run(msg.fw_version ?? null, msg.boot_count ?? null, now, clock, ota, Number.isFinite(msg.next_seq) ? Number(msg.next_seq) : null, device.id);

    const nextSeq = Number(msg.next_seq ?? 0);
    const oldest = Number(msg.oldest_seq ?? 0);

    /* The node's chain is shorter than ours: its ledger restarted. Keep the old
     * epoch and open a new one. */
    if (device.last_seq !== null && nextSeq <= device.last_seq) {
      this.db.prepare(
        "UPDATE devices SET epoch = epoch + 1, chain_status = 'empty', last_seq = NULL, last_hash = NULL, last_ts = NULL, resume_seq = NULL WHERE id = ?",
      ).run(device.id);
      this.chainEvent(device, 'reset', device.last_seq, nextSeq, `node reports next_seq ${nextSeq}; epoch ${device.epoch + 1} opened`);
      this.pending.delete(device.id);
      device = this.device(device.id)!;
    }

    const have = device.last_seq ?? -1;
    if (device.last_seq !== null && oldest > have + 1) {
      this.db.prepare('UPDATE devices SET resume_seq = ? WHERE id = ?').run(oldest, device.id);
      this.chainEvent(device, 'gap', have + 1, oldest - 1, 'entries rotated out on the node before they were mirrored');
    }
    this.send(device, { cmd: 'sync', have_seq: have });
    this.sendTime(device);
    this.live.publish({ kind: 'device', device_id: device.id });
  }

  onState(device: Device, msg: { fusion?: string; alarm?: string; faults?: Record<string, boolean> }): void {
    this.db.prepare('UPDATE devices SET last_state = ?, last_alarm = ?, faults = ?, last_seen = ? WHERE id = ?')
      .run(String(msg.fusion ?? ''), String(msg.alarm ?? ''), JSON.stringify(msg.faults ?? {}), this.now(), device.id);
    this.live.publish({ kind: 'state', device_id: device.id, fusion: String(msg.fusion ?? ''), alarm: String(msg.alarm ?? ''), faults: msg.faults ?? {} });
  }

  onTelemetry(device: Device, msg: { scores?: Record<string, number>; fan?: Record<string, unknown> }): void {
    const now = this.now();
    const minute = Math.floor(now / 60000);
    const scores = msg.scores ?? {};
    this.db.prepare(
      'INSERT INTO telemetry (device_id, minute, scores, fan) VALUES (?, ?, ?, ?) ON CONFLICT(device_id, minute) DO UPDATE SET scores = excluded.scores, fan = excluded.fan',
    ).run(device.id, minute, JSON.stringify(scores), msg.fan ? JSON.stringify(msg.fan) : null);
    this.db.prepare('UPDATE devices SET last_seen = ? WHERE id = ?').run(now, device.id);
    this.live.publish({ kind: 'telemetry', device_id: device.id, scores, at: now });
  }

  onLedger(device: Device, json: EntryJson): void {
    const entry = entryFromJson(json);
    if (toHex(entry.device_id) !== device.id) {
      audit(this.db, 'ingest', 'foreign_entry', { device: device.id, claimed: json.device_id, seq: json.seq }, this.now());
      return;
    }
    const expected = device.last_seq === null ? null : device.last_seq + 1;

    if (expected !== null && entry.seq < expected) {
      const stored = this.db.prepare('SELECT hash FROM ledger_entries WHERE device_id = ? AND epoch = ? AND seq = ?')
        .get(device.id, device.epoch, entry.seq) as Row | undefined;
      if (stored && stored.hash === toHex(entry.hash)) {
        this.send(device, { cmd: 'ack', seq: device.last_seq });
        return;
      }
      this.chainEvent(device, 'conflict', entry.seq, entry.seq, 'a second, different entry arrived for a stored sequence number');
      this.db.prepare("UPDATE devices SET chain_status = 'broken' WHERE id = ?").run(device.id);
      return;
    }

    const isResume = device.resume_seq !== null && entry.seq === device.resume_seq;
    if (expected !== null && entry.seq > expected && !isResume) {
      let queue = this.pending.get(device.id);
      if (!queue) this.pending.set(device.id, (queue = new Map()));
      if (queue.size < MAX_PENDING) queue.set(entry.seq, json);
      const last = this.lastResend.get(device.id) ?? 0;
      if (this.now() - last >= RESEND_INTERVAL_MS) {
        this.lastResend.set(device.id, this.now());
        this.send(device, { cmd: 'resend', from_seq: expected });
      }
      return;
    }

    const result = this.verify(device, entry, isResume);
    this.store(device, entry, json, result);

    const queue = this.pending.get(device.id);
    if (queue) {
      const fresh = this.device(device.id)!;
      const next = queue.get((fresh.last_seq ?? -1) + 1);
      if (next) {
        queue.delete(next.seq);
        this.onLedger(fresh, next);
      } else if (queue.size === 0) {
        this.pending.delete(device.id);
      }
    }
  }

  private verify(device: Device, entry: Entry, isResume: boolean): VerifyResult {
    const prev = toHex(entry.prev_hash);
    let result: VerifyResult = 'ok';
    if (device.last_seq === null) {
      if (entry.seq === 0) {
        if (prev !== ZERO_HASH) result = 'chain';
      } else {
        result = prev === ZERO_HASH ? 'chain' : 'start';
      }
    } else if (isResume) {
      result = 'gap';
    } else if (prev !== device.last_hash) {
      result = 'chain';
    }
    if (result !== 'chain' && toHex(sha256(preimage(entry))) !== toHex(entry.hash)) result = 'chain';
    if (result !== 'chain' && !verifySignature(entry, p256PointToKey(device.ledger_pub))) result = 'signature';
    return result;
  }

  private store(device: Device, entry: Entry, json: EntryJson, result: VerifyResult): void {
    const now = this.now();
    let incidentChange: { id: number; open: boolean } | null = null;

    transaction(this.db, () => {
      this.db.prepare(
        `INSERT INTO ledger_entries (device_id, epoch, seq, ts, ts_src, type, payload, fw_hash, prev_hash, hash, sig, received_at, verify_result)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      ).run(device.id, device.epoch, entry.seq, entry.ts, entry.ts_src, entry.type, JSON.stringify(json.payload),
        json.fw_hash, json.prev_hash, json.hash, json.sig, now, result);

      let status = device.chain_status;
      if (result === 'chain' || result === 'signature') status = 'broken';
      else if (status === 'empty') status = result === 'ok' ? 'ok' : 'partial';
      else if (result === 'gap' && status === 'ok') status = 'gap';
      this.db.prepare('UPDATE devices SET last_seq = ?, last_hash = ?, last_ts = ?, chain_status = ?, resume_seq = NULL, last_seen = ? WHERE id = ?')
        .run(entry.seq, json.hash, entry.ts, status, now, device.id);

      const payload = json.payload as Record<string, unknown>;
      if (entry.type === 'state_change') {
        incidentChange = this.trackIncident(device, entry, payload);
      } else if (entry.type === 'media_captured' && Array.isArray(payload.frames)) {
        payload.frames.forEach((sha, idx) => {
          this.db.prepare('INSERT OR IGNORE INTO media_refs (device_id, epoch, seq, idx, sha256) VALUES (?, ?, ?, ?, ?)')
            .run(device.id, device.epoch, entry.seq, idx, String(sha));
        });
      } else if (entry.type === 'self_test') {
        this.db.prepare('UPDATE devices SET last_self_test = ? WHERE id = ?').run(JSON.stringify({ ts: entry.ts, ...payload }), device.id);
      } else if (entry.type === 'calibration') {
        this.db.prepare('UPDATE devices SET last_calibration_ts = ? WHERE id = ?').run(entry.ts, device.id);
      } else if (entry.type === 'trace_captured' && typeof payload.trace === 'string') {
        linkTraceEntry(this.db, device.id, device.epoch, entry.seq, payload.trace);
      }
    });

    if (result === 'chain' || result === 'signature') {
      this.chainEvent(device, result, entry.seq, entry.seq, result === 'chain' ? 'hash or link does not match' : 'signature does not verify');
    }
    this.send(device, { cmd: 'ack', seq: entry.seq });
    this.live.publish({ kind: 'entry', device_id: device.id, seq: entry.seq, type: entry.type, verify_result: result });
    if (incidentChange) {
      const change = incidentChange as { id: number; open: boolean };
      this.live.publish({ kind: 'incident', device_id: device.id, incident_id: change.id, open: change.open });
    }
    const trusted = result !== 'chain' && result !== 'signature';
    const payload = json.payload as Record<string, unknown>;
    if (trusted && entry.type === 'state_change' && payload.to === 'ELEVATED') {
      this.live.publish({ kind: 'elevated', device_id: device.id });
    }
    if (trusted && entry.type === 'fw_update') {
      this.live.publish({ kind: 'fw', device_id: device.id, phase: String(payload.phase ?? '') });
      this.onFwUpdate?.(device.id, payload);
    }
  }

  /** The gateway's clock for the unit's ledger (transport v0.5): after every sync, and hourly while it is connected. */
  private sendTime(device: Device): void {
    this.send(device, { cmd: 'time', unix_ms: this.now() });
  }

  /** Call hourly. Every unit heard from in the last 90 s gets the time again; returns how many did. */
  sendTimeToConnected(): number {
    const rows = this.db.prepare('SELECT id FROM devices WHERE last_seen >= ? AND removed_at IS NULL').all(this.now() - CONNECTED_MS) as Row[];
    let sent = 0;
    for (const row of rows) {
      const device = this.device(String(row.id));
      if (device) {
        this.sendTime(device);
        sent++;
      }
    }
    return sent;
  }

  onTrace(device: Device, sha: string, bytes: Buffer): boolean {
    const now = this.now();
    if (!/^[0-9a-f]{64}$/.test(sha) || bytes.length === 0 || bytes.length > TRACE_MAX_BYTES) {
      audit(this.db, 'ingest', 'trace_rejected', { device: device.id, sha, size: bytes.length }, now);
      return false;
    }
    const actual = createHash('sha256').update(bytes).digest('hex');
    if (actual !== sha) {
      audit(this.db, 'ingest', 'trace_hash_mismatch', { device: device.id, named: sha, actual }, now);
      return false;
    }
    try {
      storeTrace(this.db, this.tracesDir, device.id, sha, bytes, now);
    } catch (err) {
      /* The hash is right but this gateway cannot read the format: no acknowledgement, so the unit keeps the file. */
      audit(this.db, 'ingest', 'trace_unreadable', { device: device.id, sha, error: String(err) }, now);
      return false;
    }
    this.send(device, { cmd: 'trace_ack', sha256: sha });
    return true;
  }

  private trackIncident(device: Device, entry: Entry, payload: Record<string, unknown>): { id: number; open: boolean } | null {
    const open = this.db.prepare('SELECT id FROM incidents WHERE device_id = ? AND end_seq IS NULL ORDER BY id DESC LIMIT 1')
      .get(device.id) as Row | undefined;
    if (payload.to === 'CRITICAL' && !open) {
      const info = this.db.prepare(
        'INSERT INTO incidents (device_id, epoch, site_id, start_seq, start_ts, reason) VALUES (?, ?, ?, ?, ?, ?)',
      ).run(device.id, device.epoch, device.site_id, entry.seq, entry.ts, JSON.stringify(payload.reason ?? []));
      return { id: Number(info.lastInsertRowid), open: true };
    }
    if (payload.from === 'CRITICAL' && payload.to !== 'CRITICAL' && open) {
      this.db.prepare('UPDATE incidents SET end_seq = ?, end_ts = ? WHERE id = ?').run(entry.seq, entry.ts, open.id as number);
      return { id: Number(open.id), open: false };
    }
    return null;
  }

  onMedia(device: Device, sha: string, bytes: Buffer): boolean {
    const now = this.now();
    if (!/^[0-9a-f]{64}$/.test(sha) || bytes.length === 0 || bytes.length > MAX_MEDIA_BYTES) {
      audit(this.db, 'ingest', 'media_rejected', { device: device.id, sha, size: bytes.length }, now);
      return false;
    }
    const actual = createHash('sha256').update(bytes).digest('hex');
    if (actual !== sha) {
      audit(this.db, 'ingest', 'media_hash_mismatch', { device: device.id, named: sha, actual }, now);
      return false;
    }
    const dir = join(this.mediaDir, device.id);
    mkdirSync(dir, { recursive: true });
    const path = join(dir, `${sha}.jpg`);
    writeFileSync(`${path}.part`, bytes);
    renameSync(`${path}.part`, path);
    this.db.prepare('INSERT INTO media (sha256, device_id, size, received_at) VALUES (?, ?, ?, ?) ON CONFLICT(sha256) DO UPDATE SET deleted_at = NULL')
      .run(sha, device.id, bytes.length, now);
    this.send(device, { cmd: 'media_ack', sha256: sha });
    this.live.publish({ kind: 'media', device_id: device.id, sha256: sha });
    return true;
  }
}

export function mediaPath(mediaDir: string, deviceId: string, sha: string): string {
  return join(mediaDir, deviceId, `${sha}.jpg`);
}
