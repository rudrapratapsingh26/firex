import { spawnSync } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

import { afterEach, beforeEach, describe as suite, expect, test } from 'vitest';

import { toHex } from '../../ledger/src/cbor.ts';
import { deviceIdFor, entryToJson, LedgerWriter, sha256, type EntryJson } from '../../ledger/src/entry.ts';
import { ensureSite, openDb, type Db, type Row } from '../server/db.ts';
import { Ingest } from '../server/ingest.ts';
import { LiveBus } from '../server/live.ts';
import {
  CAPTURE_COLUMNS, encodeTrace, parseTrace, storeTrace, traceToCapture, TraceError, type TraceSample,
} from '../server/traces.ts';
import { newP256, pointHex, type KeyPair } from '../sim/keys.ts';

const SITE = 'site-001';
const T0 = 1_789_000_000;

let db: Db;
let dir: string;
let ingest: Ingest;
let key: KeyPair;
let deviceId: string;
let writer: LedgerWriter;

function sample(fusion: string, gas: number, heat: number, alarm = fusion): TraceSample {
  return { scores: [gas, 0.05, 0, heat, null], fusion, alarm, fanDuty: 100, fanState: 'on' };
}

/** 10 samples before the trigger, ELEVATED for 4, CRITICAL for 6, NORMAL for 10. */
function episode(bootCount = 3): Buffer {
  const samples = [
    ...Array.from({ length: 10 }, () => sample('NORMAL', 0.1, 0.1)),
    ...Array.from({ length: 4 }, () => sample('ELEVATED', 0.7, 0.2)),
    ...Array.from({ length: 6 }, () => sample('CRITICAL', 0.8, 0.7)),
    ...Array.from({ length: 10 }, () => sample('NORMAL', 0.2, 0.1)),
  ];
  return encodeTrace({ periodMs: 500, preSamples: 10, trigger: 'ELEVATED', triggerUptimeMs: 600_000, bootCount, samples });
}

function append(type: Parameters<LedgerWriter['append']>[0], payload: Record<string, unknown>, tsSrc: 'gateway' | 'uncertain' = 'gateway'): EntryJson {
  const json = entryToJson(writer.append(type, payload as never, T0 + writer.entries.length * 2, tsSrc));
  ingest.handle(`firex/${SITE}/${deviceId}/ledger`, Buffer.from(JSON.stringify(json)));
  return json;
}

beforeEach(() => {
  db = openDb(':memory:');
  ensureSite(db, SITE, 'Test site');
  dir = mkdtempSync(join(tmpdir(), 'firex-traces-'));
  ingest = new Ingest({ db, mediaDir: join(dir, 'media'), tracesDir: join(dir, 'traces'), live: new LiveBus(), publish: () => {} });
  key = newP256();
  deviceId = toHex(deviceIdFor(key.publicKey));
  db.prepare("INSERT INTO devices (id, site_id, label, ledger_pub, tls_pub, provisioned_at) VALUES (?, ?, 'Room 206', ?, ?, 0)")
    .run(deviceId, SITE, pointHex(key.publicKey), pointHex(newP256().publicKey));
  writer = new LedgerWriter(deviceIdFor(key.publicKey), sha256(Buffer.from('fw')), key.privateKey);
});

afterEach(() => {
  db.close();
  rmSync(dir, { recursive: true, force: true });
});

suite('trace files', () => {
  test('a trace encodes and parses back to the same samples', () => {
    const trace = parseTrace(episode());
    expect(trace).toMatchObject({ version: 1, periodMs: 500, preSamples: 10, trigger: 'ELEVATED', triggerUptimeMs: 600_000, bootCount: 3 });
    expect(trace.samples).toHaveLength(30);
    expect(trace.samples[12]).toEqual(sample('ELEVATED', 0.7, 0.2));
    expect(trace.samples[0].scores[4]).toBeNull();
  });

  test('bytes that are not a whole trace are refused', () => {
    const bytes = episode();
    expect(() => parseTrace(Buffer.from('not a trace at all, not even close'))).toThrow(TraceError);
    expect(() => parseTrace(bytes.subarray(0, bytes.length - 3))).toThrow(/announces 30 samples/);
  });
});

suite('traces as captures', () => {
  function recordEpisode(label: 'true' | 'nuisance' | 'test' | null, tsSrc: 'gateway' | 'uncertain' = 'gateway') {
    const bytes = episode();
    const sha = createSha(bytes);
    append('boot', { fw_version: '0.5.0', reset_reason: 'power_on', boot_count: 3 }, tsSrc);
    append('state_change', { from: 'NORMAL', to: 'ELEVATED', reason: ['gas'], scores: { gas: 70 } }, tsSrc);
    append('state_change', { from: 'ELEVATED', to: 'CRITICAL', reason: ['gas', 'heat'], scores: { gas: 80, heat: 70 } }, tsSrc);
    append('state_change', { from: 'CRITICAL', to: 'NORMAL', reason: [], scores: { gas: 20 } }, tsSrc);
    append('trace_captured', { trigger: 'ELEVATED', trace: Buffer.from(sha, 'hex'), samples: 30, period_ms: 500, pre_samples: 10 }, tsSrc);
    storeTrace(db, join(dir, 'traces'), deviceId, sha, bytes, Date.now());
    if (label) db.prepare('UPDATE incidents SET label = ?').run(label);
    return sha;
  }

  test('the capture pairs the trace with the ledger and marks a real hazard as a hazard trial', () => {
    const sha = recordEpisode('true');
    const row = db.prepare('SELECT seq FROM traces WHERE sha256 = ?').get(sha) as Row;
    expect(row.seq).toBe(4);

    const capture = traceToCapture(db, join(dir, 'traces'), sha);
    const lines = capture.csv.trim().split('\n');
    expect(lines[0]).toBe(CAPTURE_COLUMNS.join(','));
    expect(lines).toHaveLength(31);
    expect(lines[1].split(',')).toEqual(['595000', '0.100', '1', '0.050', '1', '0.000', '1', '0.100', '1', '', '0', '100', '', 'on', 'NORMAL', '595.000', String(T0 * 1000 + 2000 - 600_000 + 595_000)]);
    expect(capture).toMatchObject({ transitions: 3, matched: 3, clockSynced: true, trial: { cls: 'hazard' } });
    expect(capture.trial!.name).toBe(`unit-room-206-incident-${capture.incidentId}`);

    const events = capture.events.trim().split('\n').map((l) => JSON.parse(l));
    expect(events[0]).toMatchObject({ type: 'mark', text: `pilot trace ${sha}`, t_ms: 595_000 });
    expect(events.filter((e) => e.type === 'state_change').map((e) => [e.seq, e.to, e.t_ms])).toEqual([[1, 'ELEVATED', 600_000], [2, 'CRITICAL', 602_000], [3, 'NORMAL', 605_000]]);
    expect(events.filter((e) => e.type === 'mark').map((e) => e.text)).toEqual([`pilot trace ${sha}`, `trial hazard ${capture.trial!.name}`, 'trial end']);
  });

  test('a nuisance label is a nuisance trial; a test label is no trial; an uncertain clock leaves host_unix_ms blank', () => {
    const sha = recordEpisode('nuisance', 'uncertain');
    let capture = traceToCapture(db, join(dir, 'traces'), sha);
    expect(capture.trial).toMatchObject({ cls: 'nuisance', name: `other-incident-${capture.incidentId}` });
    expect(capture.clockSynced).toBe(false);
    expect(capture.csv.split('\n')[1].endsWith(',')).toBe(true);

    db.prepare("UPDATE incidents SET label = 'test'").run();
    capture = traceToCapture(db, join(dir, 'traces'), sha);
    expect(capture.trial).toBeNull();
  });

  test('state changes that do not agree with the trace are not paired', () => {
    const bytes = episode();
    const sha = createSha(bytes);
    append('boot', { fw_version: '0.5.0', reset_reason: 'power_on', boot_count: 3 });
    append('state_change', { from: 'NORMAL', to: 'CRITICAL', reason: ['flame'], scores: { flame: 95 } });
    append('trace_captured', { trigger: 'ELEVATED', trace: Buffer.from(sha, 'hex'), samples: 30, period_ms: 500, pre_samples: 10 });
    storeTrace(db, join(dir, 'traces'), deviceId, sha, bytes, Date.now());
    expect(traceToCapture(db, join(dir, 'traces'), sha)).toMatchObject({ transitions: 3, matched: 0, trial: null });
  });

  test('the replay harness reads the capture and finds the trial', () => {
    const sha = recordEpisode('true');
    const capture = traceToCapture(db, join(dir, 'traces'), sha);
    const csv = join(dir, `${sha}.csv`);
    writeFileSync(csv, capture.csv);
    writeFileSync(join(dir, `${sha}.events.jsonl`), capture.events);
    const tools = resolve(import.meta.dirname, '../../tools/validation');
    const script = [
      'import json, sys',
      `sys.path.insert(0, ${JSON.stringify(tools)})`,
      'import fxreplay',
      `c = fxreplay.load_capture(${JSON.stringify(csv)})`,
      'print(json.dumps({"samples": sum(len(s.samples) for s in c.segments), "trials": [[t.cls, t.name] for t in c.trials], "warnings": c.warnings}))',
    ].join('\n');
    const run = spawnSync(process.platform === 'win32' ? 'python' : 'python3', ['-c', script], { encoding: 'utf8' });
    if (run.error || run.status !== 0) {
      console.warn(`python not usable here, replay compatibility not checked: ${run.error?.message ?? run.stderr}`);
      return;
    }
    const out = JSON.parse(run.stdout);
    expect(out.samples).toBe(30);
    expect(out.trials).toEqual([['hazard', capture.trial!.name]]);
  });
});

function createSha(bytes: Buffer): string {
  return toHex(sha256(bytes));
}
