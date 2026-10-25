import 'reflect-metadata';

import { createHash } from 'node:crypto';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import * as x509 from '@peculiar/x509';
import { afterEach, beforeEach, describe as suite, expect, test } from 'vitest';

import { toHex } from '../../ledger/src/cbor.ts';
import { deviceIdFor, entryToJson, LedgerWriter, sha256, type EntryJson } from '../../ledger/src/entry.ts';
import { ensureSite, openDb, type Db, type Row } from '../server/db.ts';
import { Ingest } from '../server/ingest.ts';
import { LiveBus, type LiveEvent } from '../server/live.ts';
import { crlPath, ensureServiceCertificates, loadOrCreateCa } from '../server/pki.ts';
import { removeUnit } from '../server/removal.ts';
import { SiteLedger } from '../server/site-ledger.ts';
import { encodeTrace } from '../server/traces.ts';
import { quietEpisode } from '../sim/node-sim.ts';
import { newP256, pointHex, type KeyPair } from '../sim/keys.ts';

const SITE = 'site-001';
const T0 = 1_789_000_000;

let db: Db;
let dir: string;
let clock: number;
let sent: { topic: string; body: Record<string, unknown> }[];
let events: LiveEvent[];
let fwUpdates: { device: string; payload: Record<string, unknown> }[];
let ingest: Ingest;
let key: KeyPair;
let deviceId: string;
let writer: LedgerWriter;

const topic = (kind: string, id = deviceId) => `firex/${SITE}/${id}/${kind}`;
const publish = (t: string, body: unknown) => ingest.handle(t, Buffer.isBuffer(body) ? body : Buffer.from(JSON.stringify(body)));
const cmds = (name: string) => sent.filter((s) => s.body.cmd === name).map((s) => s.body);
const device = () => db.prepare('SELECT * FROM devices WHERE id = ?').get(deviceId) as Row;

function append(type: Parameters<LedgerWriter['append']>[0], payload: Record<string, unknown>): EntryJson {
  const json = entryToJson(writer.append(type, payload as never, T0 + writer.entries.length, 'gateway'));
  publish(topic('ledger'), json);
  return json;
}

beforeEach(() => {
  db = openDb(':memory:');
  ensureSite(db, SITE, 'Test site');
  dir = mkdtempSync(join(tmpdir(), 'firex-ingest-m5-'));
  clock = 1_800_000_000_000;
  sent = [];
  events = [];
  fwUpdates = [];
  const live = new LiveBus();
  live.subscribe((e) => events.push(e));
  ingest = new Ingest({
    db, mediaDir: join(dir, 'media'), tracesDir: join(dir, 'traces'), live, now: () => clock,
    publish: (t, payload) => sent.push({ topic: t, body: JSON.parse(payload) }),
    onFwUpdate: (d, payload) => fwUpdates.push({ device: d, payload }),
  });
  key = newP256();
  deviceId = toHex(deviceIdFor(key.publicKey));
  db.prepare('INSERT INTO devices (id, site_id, ledger_pub, tls_pub, cert_serial, provisioned_at) VALUES (?, ?, ?, ?, ?, 0)')
    .run(deviceId, SITE, pointHex(key.publicKey), pointHex(newP256().publicKey), '0a1b2c');
  writer = new LedgerWriter(deviceIdFor(key.publicKey), sha256(Buffer.from('fw')), key.privateKey);
});

afterEach(() => {
  db.close();
  rmSync(dir, { recursive: true, force: true });
});

suite('clock and hello', () => {
  test('hello records the clock, the OTA slot and next_seq; the time follows the sync', () => {
    publish(topic('hello'), { fw_version: '0.5.0', boot_count: 4, next_seq: 12, oldest_seq: 0, clock: 'uncertain', ota: { partition: 'ota_1', pending_verify: true } });
    expect(device()).toMatchObject({ clock: 'uncertain', hello_next_seq: 12 });
    expect(JSON.parse(String(device().ota))).toEqual({ partition: 'ota_1', pending_verify: true });
    expect(sent.map((s) => s.body.cmd)).toEqual(['sync', 'time']);
    expect(cmds('time')).toEqual([{ cmd: 'time', unix_ms: clock }]);
  });

  test('the hourly time goes only to units heard from in the last 90 s', () => {
    const quiet = newP256();
    db.prepare('INSERT INTO devices (id, site_id, ledger_pub, tls_pub, provisioned_at, last_seen) VALUES (?, ?, ?, ?, 0, ?)')
      .run(toHex(deviceIdFor(quiet.publicKey)), SITE, pointHex(quiet.publicKey), '04', clock - 10 * 60_000);
    publish(topic('telemetry'), { scores: { gas: 5 } });
    clock += 30_000;
    expect(ingest.sendTimeToConnected()).toBe(1);
    expect(sent.filter((s) => s.body.cmd === 'time').map((s) => s.topic)).toEqual([`firex/${SITE}/${deviceId}/cmd`]);
  });
});

suite('traces', () => {
  const trace = () => encodeTrace({ periodMs: 500, preSamples: 10, trigger: 'ELEVATED', triggerUptimeMs: 60_000, bootCount: 1, samples: quietEpisode() });

  test('a trace is stored, acknowledged, and linked to its entry whichever arrives first', () => {
    const bytes = trace();
    const sha = createHash('sha256').update(bytes).digest('hex');
    expect(publish(`${topic('trace')}/${sha}`, bytes)).toBe(true);
    expect(cmds('trace_ack')).toEqual([{ cmd: 'trace_ack', sha256: sha }]);
    expect(existsSync(join(dir, 'traces', deviceId, `${sha}.fxt`))).toBe(true);
    expect((db.prepare('SELECT seq FROM traces').get() as Row).seq).toBeNull();

    const entry = append('trace_captured', { trigger: 'ELEVATED', trace: Buffer.from(sha, 'hex'), samples: 30, period_ms: 500, pre_samples: 10 });
    expect(db.prepare('SELECT seq, samples, trigger FROM traces').get()).toEqual({ seq: entry.seq, samples: 30, trigger: 'ELEVATED' });
  });

  test('bytes that do not hash to the topic, or are not a trace, are not acknowledged', () => {
    const bytes = trace();
    expect(publish(`${topic('trace')}/${'ab'.repeat(32)}`, bytes)).toBe(false);
    const junk = Buffer.from('a correctly hashed file that is not a trace');
    expect(publish(`${topic('trace')}/${createHash('sha256').update(junk).digest('hex')}`, junk)).toBe(false);
    expect(cmds('trace_ack')).toEqual([]);
    const actions = (db.prepare('SELECT action FROM audit ORDER BY id').all() as Row[]).map((r) => r.action);
    expect(actions).toEqual(['trace_hash_mismatch', 'trace_unreadable']);
  });
});

suite('entries the gateway acts on', () => {
  test('fw_update entries reach the rollout and the dashboard; ELEVATED is announced', () => {
    append('fw_update', { phase: 'accepted', version: '0.5.0', from_version: '0.4.0', image: Buffer.alloc(32, 1), reason: '' });
    append('state_change', { from: 'NORMAL', to: 'ELEVATED', reason: ['gas'], scores: { gas: 66 } });
    expect(fwUpdates).toEqual([{ device: deviceId, payload: { phase: 'accepted', version: '0.5.0', from_version: '0.4.0', image: '01'.repeat(32), reason: '' } }]);
    expect(events.filter((e) => e.kind === 'fw' || e.kind === 'elevated')).toEqual([
      { kind: 'fw', device_id: deviceId, phase: 'accepted' },
      { kind: 'elevated', device_id: deviceId },
    ]);
  });
});

suite('unit removal', () => {
  test('removal revokes the certificate, is written to the site ledger, and silences the unit at ingest', async () => {
    const pki = await loadOrCreateCa(join(dir, 'pki'), 'Test');
    await ensureServiceCertificates(pki, ['localhost']);
    expect(existsSync(crlPath(pki))).toBe(true);
    const site = new SiteLedger({ db, dir: join(dir, 'site-ledger'), siteId: SITE, now: () => clock });
    let reloaded = 0;

    const result = await removeUnit(deviceId, { db, pki, siteLedger: site, by: 'admin', now: clock, onCrlChanged: () => reloaded++ });
    expect(result).toEqual({ ok: true, certSerial: '0a1b2c' });
    expect(reloaded).toBe(1);
    const crl = new x509.X509Crl(readFileSync(crlPath(pki), 'utf8'));
    expect(crl.entries.map((e) => e.serialNumber.replace(/^0+/, ''))).toEqual(['a1b2c']);
    expect(site.bundle().log.map((e) => [e.type, e.payload.unit, e.payload.cert_serial])).toEqual([['unit_removed', deviceId, '0a1b2c']]);

    expect(publish(topic('state'), { fusion: 'CRITICAL', alarm: 'CRITICAL', faults: {} })).toBe(false);
    expect(device().last_state).toBeNull();
    expect((await removeUnit(deviceId, { db, pki, by: 'admin' })).error).toMatch(/already removed/);
  });
});
