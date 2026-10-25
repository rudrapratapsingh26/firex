/**
 * Episode traces: what a unit's sensors said around an episode, recorded on the
 * unit and sent to the gateway on its own topic, like a photo (docs/transport.md).
 *
 * File format, little-endian: a 32-byte header
 *
 *   "FXTR" | u8 version 1 | u8 modalities 5 | u16 period_ms | u32 pre_samples | u32 samples |
 *   u8 trigger state | 3 zero | u32 trigger_uptime_ms | u32 boot_count | u32 zero
 *
 * then one 16-byte record per sample: five u16 scores in thousandths (gas, smoke,
 * flame, heat, humidity; 0xFFFF for invalid), u8 fusion state, u8 alarm state,
 * u8 fan duty, u8 fan state, u16 zero. Sample i was taken at device uptime
 * trigger_uptime_ms + (i - pre_samples) * period_ms.
 *
 * The unit's `trace_captured` ledger entry carries the file's SHA-256, so a
 * stored trace is linked to its entry and verifiable like a photo.
 *
 * traceToCapture turns a trace into the M4 capture format (tools/capture), so
 * pilot traces replay through tools/validation/replay.py. Its state_change
 * echoes come from the unit's own ledger, matched as described there.
 */
import { mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

import type { Db, Row } from './db.ts';

export const TRACE_MAX_BYTES = 1024 * 1024;
const HEADER_BYTES = 32;
const RECORD_BYTES = 16;
const INVALID = 0xffff;

export const TRACE_MODALITIES = ['gas', 'smoke', 'flame', 'heat', 'humidity'] as const;
export const FUSION_STATES = ['WARMUP', 'NORMAL', 'ELEVATED', 'CRITICAL'];
export const ALARM_STATES = ['BOOT', 'WARMUP', 'NORMAL', 'ELEVATED', 'CRITICAL', 'SILENCED', 'FAULT', 'SELF_TEST'];
const FAN_STATES: Record<number, string> = { 0: 'off', 1: 'on', 2: 'failed', 255: 'absent' };

/** The columns tools/validation/fxreplay.py reads, in the order tools/capture writes them. */
export const CAPTURE_COLUMNS = [
  't_ms', 'gas', 'gas_ok', 'smoke', 'smoke_ok', 'flame', 'flame_ok', 'heat', 'heat_ok', 'rh', 'rh_ok',
  'fan_duty', 'fan_rpm', 'fan_state', 'alarm', 'host_s', 'host_unix_ms',
];

export class TraceError extends Error {}

export interface TraceSample {
  /** 0-1, or null for an invalid modality. */
  scores: (number | null)[];
  fusion: string;
  alarm: string;
  fanDuty: number;
  fanState: string;
}

export interface Trace {
  version: number;
  periodMs: number;
  preSamples: number;
  trigger: string;
  triggerUptimeMs: number;
  bootCount: number;
  samples: TraceSample[];
}

function stateName(names: string[], value: number): string {
  return names[value] ?? `state-${value}`;
}

export function parseTrace(bytes: Buffer): Trace {
  if (bytes.length < HEADER_BYTES) throw new TraceError('shorter than a trace header');
  if (bytes.toString('latin1', 0, 4) !== 'FXTR') throw new TraceError('not a trace (no FXTR magic)');
  const version = bytes[4];
  if (version !== 1) throw new TraceError(`trace format version ${version} is not understood`);
  if (bytes[5] !== TRACE_MODALITIES.length) throw new TraceError(`trace has ${bytes[5]} modalities, expected ${TRACE_MODALITIES.length}`);
  const periodMs = bytes.readUInt16LE(6);
  const preSamples = bytes.readUInt32LE(8);
  const count = bytes.readUInt32LE(12);
  if (!periodMs) throw new TraceError('trace period is zero');
  if (bytes.length !== HEADER_BYTES + count * RECORD_BYTES) {
    throw new TraceError(`trace announces ${count} samples but holds ${(bytes.length - HEADER_BYTES) / RECORD_BYTES}`);
  }
  if (preSamples > count) throw new TraceError('trace trigger lies after its last sample');
  const samples: TraceSample[] = [];
  for (let i = 0; i < count; i++) {
    const at = HEADER_BYTES + i * RECORD_BYTES;
    const scores: (number | null)[] = [];
    for (let m = 0; m < TRACE_MODALITIES.length; m++) {
      const raw = bytes.readUInt16LE(at + 2 * m);
      scores.push(raw === INVALID ? null : Math.min(raw, 1000) / 1000);
    }
    samples.push({
      scores,
      fusion: stateName(FUSION_STATES, bytes[at + 10]),
      alarm: stateName(ALARM_STATES, bytes[at + 11]),
      fanDuty: bytes[at + 12],
      fanState: FAN_STATES[bytes[at + 13]] ?? 'absent',
    });
  }
  return {
    version,
    periodMs,
    preSamples,
    trigger: stateName(FUSION_STATES, bytes[16]),
    triggerUptimeMs: bytes.readUInt32LE(20),
    bootCount: bytes.readUInt32LE(24),
    samples,
  };
}

/** Build a trace file from samples, the inverse of parseTrace. For the node simulator and the tests. */
export function encodeTrace(trace: Omit<Trace, 'version'>): Buffer {
  const out = Buffer.alloc(HEADER_BYTES + trace.samples.length * RECORD_BYTES);
  out.write('FXTR', 0, 'latin1');
  out[4] = 1;
  out[5] = TRACE_MODALITIES.length;
  out.writeUInt16LE(trace.periodMs, 6);
  out.writeUInt32LE(trace.preSamples, 8);
  out.writeUInt32LE(trace.samples.length, 12);
  out[16] = Math.max(0, FUSION_STATES.indexOf(trace.trigger));
  out.writeUInt32LE(trace.triggerUptimeMs, 20);
  out.writeUInt32LE(trace.bootCount, 24);
  trace.samples.forEach((s, i) => {
    const at = HEADER_BYTES + i * RECORD_BYTES;
    s.scores.forEach((v, m) => out.writeUInt16LE(v === null ? INVALID : Math.round(Math.max(0, Math.min(1, v)) * 1000), at + 2 * m));
    out[at + 10] = Math.max(0, FUSION_STATES.indexOf(s.fusion));
    out[at + 11] = Math.max(0, ALARM_STATES.indexOf(s.alarm));
    out[at + 12] = s.fanDuty;
    out[at + 13] = Number(Object.entries(FAN_STATES).find(([, name]) => name === s.fanState)?.[0] ?? 255);
  });
  return out;
}

export function sampleUptimeMs(trace: Trace, index: number): number {
  return trace.triggerUptimeMs + (index - trace.preSamples) * trace.periodMs;
}

export function tracePath(tracesDir: string, deviceId: string, sha: string): string {
  return join(tracesDir, deviceId, `${sha}.fxt`);
}

/** Store a trace whose hash the caller has checked, and link it to its trace_captured entry if that has arrived. */
export function storeTrace(db: Db, tracesDir: string, deviceId: string, sha: string, bytes: Buffer, now: number): Trace {
  const trace = parseTrace(bytes);
  const dir = join(tracesDir, deviceId);
  mkdirSync(dir, { recursive: true });
  const path = tracePath(tracesDir, deviceId, sha);
  writeFileSync(`${path}.part`, bytes);
  renameSync(`${path}.part`, path);
  const entry = db.prepare(
    "SELECT epoch, seq FROM ledger_entries WHERE device_id = ? AND type = 'trace_captured' AND json_extract(payload, '$.trace') = ? ORDER BY epoch DESC, seq DESC LIMIT 1",
  ).get(deviceId, sha) as Row | undefined;
  db.prepare(
    `INSERT INTO traces (sha256, device_id, epoch, seq, samples, period_ms, pre_samples, trigger, trigger_uptime_ms, boot_count, size, received_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
     ON CONFLICT(sha256) DO UPDATE SET epoch = COALESCE(traces.epoch, excluded.epoch), seq = COALESCE(traces.seq, excluded.seq)`,
  ).run(sha, deviceId, entry ? Number(entry.epoch) : null, entry ? Number(entry.seq) : null, trace.samples.length, trace.periodMs,
    trace.preSamples, trace.trigger, trace.triggerUptimeMs, trace.bootCount, bytes.length, now);
  return trace;
}

/** A trace_captured entry arrived: link it to the stored trace, if the file came first. */
export function linkTraceEntry(db: Db, deviceId: string, epoch: number, seq: number, sha: string): void {
  db.prepare('UPDATE traces SET epoch = ?, seq = ? WHERE sha256 = ? AND device_id = ? AND seq IS NULL').run(epoch, seq, sha, deviceId);
}

interface StateEntry {
  seq: number;
  ts: number;
  tsSrc: string;
  from: string;
  to: string;
  reason: unknown;
}

export interface TraceCapture {
  csv: string;
  events: string;
  /** State changes in the trace, and how many of them were matched to ledger entries. */
  transitions: number;
  matched: number;
  incidentId: number | null;
  trial: { cls: 'hazard' | 'nuisance'; name: string } | null;
  clockSynced: boolean;
}

function slug(text: string): string {
  return text.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '') || 'unit';
}

/**
 * The trace in the M4 capture format.
 *
 * Matching the unit's ledger to the trace. The trace records the fusion state
 * of every sample, so its state changes are known by uptime; the ledger's
 * state_change entries are known by seq. The trace file is closed, and its
 * trace_captured entry written, only after the episode has ended, so the
 * episode's state changes are the ones immediately before that entry. The two
 * lists are paired backwards from there: the trace's last change with the last
 * state_change before trace_captured, and so on, while from and to agree and
 * the entry belongs to the same boot (after the `boot` entry whose boot_count
 * is the trace's). The first disagreement stops the pairing. A trace whose
 * trace_captured entry has not arrived gets no echoes.
 *
 * Paired entries give the echoes their seq and reasons, the incident the trace
 * belongs to (the one that starts at a paired entry), and, when a paired entry's
 * clock was set from the gateway or NTP, host_unix_ms for every row, to within
 * the entry's one-second resolution.
 */
export function traceToCapture(db: Db, tracesDir: string, sha: string): TraceCapture {
  const row = db.prepare('SELECT * FROM traces WHERE sha256 = ?').get(sha) as Row | undefined;
  if (!row) throw new TraceError(`no trace ${sha}`);
  const deviceId = String(row.device_id);
  const trace = parseTrace(readFileSync(tracePath(tracesDir, deviceId, sha)));

  const changes: { index: number; from: string; to: string }[] = [];
  for (let i = 1; i < trace.samples.length; i++) {
    if (trace.samples[i].fusion !== trace.samples[i - 1].fusion) {
      changes.push({ index: i, from: trace.samples[i - 1].fusion, to: trace.samples[i].fusion });
    }
  }

  const pairs: { index: number; entry: StateEntry }[] = [];
  if (row.seq !== null && row.epoch !== null) {
    const epoch = Number(row.epoch);
    const traceSeq = Number(row.seq);
    const boots = db.prepare("SELECT seq, payload FROM ledger_entries WHERE device_id = ? AND epoch = ? AND type = 'boot' AND seq < ? ORDER BY seq")
      .all(deviceId, epoch, traceSeq) as Row[];
    const boot = [...boots].reverse().find((b) => Number(JSON.parse(String(b.payload)).boot_count) === trace.bootCount)
      ?? boots[boots.length - 1];
    const after = boot ? Number(boot.seq) : -1;
    const states = (db.prepare(
      "SELECT seq, ts, ts_src, payload FROM ledger_entries WHERE device_id = ? AND epoch = ? AND type = 'state_change' AND seq > ? AND seq < ? ORDER BY seq",
    ).all(deviceId, epoch, after, traceSeq) as Row[]).map((r): StateEntry => {
      const p = JSON.parse(String(r.payload));
      return { seq: Number(r.seq), ts: Number(r.ts), tsSrc: String(r.ts_src), from: String(p.from), to: String(p.to), reason: p.reason ?? [] };
    });
    let k = changes.length - 1;
    let j = states.length - 1;
    while (k >= 0 && j >= 0 && changes[k].from === states[j].from && changes[k].to === states[j].to) {
      pairs.unshift({ index: changes[k].index, entry: states[j] });
      k--;
      j--;
    }
  }

  const synced = pairs.find((p) => p.entry.tsSrc === 'gateway' || p.entry.tsSrc === 'ntp');
  const unixOffset = synced ? synced.entry.ts * 1000 - sampleUptimeMs(trace, synced.index) : null;
  const hostUnix = (t: number) => (unixOffset === null ? '' : String(t + unixOffset));

  let incidentId: number | null = null;
  let trial: TraceCapture['trial'] = null;
  let trialStart: number | null = null;
  let trialEnd: number | null = null;
  if (pairs.length) {
    const seqs = pairs.map((p) => p.entry.seq);
    const incident = db.prepare(
      `SELECT i.*, d.label AS device_label FROM incidents i JOIN devices d ON d.id = i.device_id
       WHERE i.device_id = ? AND i.epoch = ? AND i.start_seq IN (${seqs.map(() => '?').join(',')}) ORDER BY i.id LIMIT 1`,
    ).get(deviceId, Number(row.epoch), ...seqs) as Row | undefined;
    if (incident) {
      incidentId = Number(incident.id);
      const startPair = pairs.find((p) => p.entry.seq === Number(incident.start_seq));
      const endPair = incident.end_seq === null ? undefined : pairs.find((p) => p.entry.seq === Number(incident.end_seq));
      trialStart = startPair ? sampleUptimeMs(trace, startPair.index) : null;
      trialEnd = endPair ? sampleUptimeMs(trace, endPair.index) : sampleUptimeMs(trace, trace.samples.length - 1);
      if (incident.label === 'true') trial = { cls: 'hazard', name: `unit-${slug(String(incident.device_label ?? ''))}-incident-${incidentId}` };
      else if (incident.label === 'nuisance') trial = { cls: 'nuisance', name: `other-incident-${incidentId}` };
    }
  }

  const lines = [CAPTURE_COLUMNS.join(',')];
  let first: number | null = null;
  trace.samples.forEach((s, i) => {
    const t = sampleUptimeMs(trace, i);
    if (t < 0) return;
    first ??= t;
    const cells: string[] = [String(t)];
    s.scores.forEach((v) => cells.push(v === null ? '' : v.toFixed(3), v === null ? '0' : '1'));
    cells.push(String(s.fanDuty), '', s.fanState, s.alarm, (t / 1000).toFixed(3), hostUnix(t));
    lines.push(cells.join(','));
  });

  const events: Record<string, unknown>[] = [];
  const event = (t: number, fields: Record<string, unknown>) => {
    const e: Record<string, unknown> = { t_ms: t, host_s: Number((t / 1000).toFixed(3)), ...fields };
    if (unixOffset !== null) e.host_unix_ms = t + unixOffset;
    events.push(e);
  };
  if (first !== null) event(first, { type: 'mark', text: `pilot trace ${sha}` });
  for (const p of pairs) {
    const t = sampleUptimeMs(trace, p.index);
    if (t >= 0) event(t, { type: 'state_change', seq: p.entry.seq, from: p.entry.from, to: p.entry.to, reason: p.entry.reason });
  }
  if (trial && trialStart !== null && trialStart >= 0) {
    event(trialStart, { type: 'mark', text: `trial ${trial.cls} ${trial.name}` });
    event(Math.max(trialStart, trialEnd ?? trialStart), { type: 'mark', text: 'trial end' });
  } else {
    trial = null;
  }
  events.sort((a, b) => Number(a.t_ms) - Number(b.t_ms));

  return {
    csv: `${lines.join('\n')}\n`,
    events: events.map((e) => JSON.stringify(e)).join('\n') + (events.length ? '\n' : ''),
    transitions: changes.length,
    matched: pairs.length,
    incidentId,
    trial,
    clockSynced: unixOffset !== null,
  };
}
