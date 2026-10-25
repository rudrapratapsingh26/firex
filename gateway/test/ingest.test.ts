import { createHash } from 'node:crypto';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { beforeEach, describe as suite, expect, test } from 'vitest';

import { toHex } from '../../ledger/src/cbor.ts';
import {
  deviceIdFor, entryFromJson, entryToJson, LedgerWriter, publicKeyFromPem, sha256, type EntryJson,
} from '../../ledger/src/entry.ts';
import { verifyLog } from '../../ledger/src/verify.ts';
import { buildBundle } from '../server/bundle.ts';
import { ensureSite, openDb, type Db, type Row } from '../server/db.ts';
import { Ingest } from '../server/ingest.ts';
import { LiveBus } from '../server/live.ts';
import { newP256, pointHex, type KeyPair } from '../sim/keys.ts';

const SITE = 'site-001';
const T0 = 1_789_000_000;

let db: Db;
let mediaDir: string;
let sent: { topic: string; body: Record<string, unknown> }[];
let ingest: Ingest;
let key: KeyPair;
let deviceId: string;
let writer: LedgerWriter;
let clock: number;

function register(k: KeyPair): string {
  const id = toHex(deviceIdFor(k.publicKey));
  db.prepare('INSERT INTO devices (id, site_id, ledger_pub, tls_pub, provisioned_at) VALUES (?, ?, ?, ?, ?)')
    .run(id, SITE, pointHex(k.publicKey), pointHex(newP256().publicKey), 0);
  return id;
}

function publish(topic: string, body: unknown): boolean {
  const payload = Buffer.isBuffer(body) ? body : Buffer.from(JSON.stringify(body));
  return ingest.handle(topic, payload);
}

const topic = (kind: string, id = deviceId) => `firex/${SITE}/${id}/${kind}`;
const device = () => db.prepare('SELECT * FROM devices WHERE id = ?').get(deviceId) as Row;
const results = () => (db.prepare('SELECT seq, verify_result FROM ledger_entries WHERE device_id = ? ORDER BY epoch, seq').all(deviceId) as Row[])
  .map((r) => `${r.seq}:${r.verify_result}`);
const cmds = (name: string) => sent.filter((s) => s.body.cmd === name).map((s) => s.body);

function append(type: Parameters<LedgerWriter['append']>[0], payload: Record<string, unknown>): EntryJson {
  const e = writer.append(type, payload as never, T0 + writer.entries.length, 'uncertain');
  return entryToJson(e);
}

beforeEach(() => {
  db = openDb(':memory:');
  ensureSite(db, SITE, 'Test site');
  mediaDir = mkdtempSync(join(tmpdir(), 'firex-media-'));
  sent = [];
  clock = 1_800_000_000_000;
  ingest = new Ingest({
    db, mediaDir, live: new LiveBus(), now: () => clock,
    publish: (t, payload) => sent.push({ topic: t, body: JSON.parse(payload) }),
  });
  key = newP256();
  deviceId = register(key);
  writer = new LedgerWriter(deviceIdFor(key.publicKey), sha256(Buffer.from('fw')), key.privateKey);
});

suite('ledger mirroring', () => {
  test('hello on an empty chain asks for everything', () => {
    publish(topic('hello'), { fw_version: '0.4.0', boot_count: 3, next_seq: 5, oldest_seq: 0 });
    expect(cmds('sync')).toEqual([{ cmd: 'sync', have_seq: -1 }]);
    expect(sent[0].topic).toBe(`firex/${SITE}/${deviceId}/cmd`);
    expect(device().fw_version).toBe('0.4.0');
  });

  test('entries in order verify, are acknowledged, and export as a bundle that verifies', () => {
    for (let i = 0; i < 4; i++) publish(topic('ledger'), append('boot', { fw_version: 'x', reset_reason: 'power_on', boot_count: i }));
    expect(results()).toEqual(['0:ok', '1:ok', '2:ok', '3:ok']);
    expect(device().chain_status).toBe('ok');
    expect(cmds('ack').map((c) => c.seq)).toEqual([0, 1, 2, 3]);

    const bundle = buildBundle(db, mediaDir, deviceId)!;
    const outcome = verifyLog(bundle.log.map(entryFromJson), publicKeyFromPem(bundle.public_key_pem));
    expect(outcome.ok).toBe(true);
    expect(outcome.entries).toBe(4);
  });

  test('a payload edited in transit is stored, flagged, and breaks the chain status', () => {
    publish(topic('ledger'), append('boot', { fw_version: 'x', reset_reason: 'power_on', boot_count: 1 }));
    const forged = append('threshold_cross', { modality: 'gas', score: 68, direction: 'up' });
    publish(topic('ledger'), { ...forged, payload: { ...forged.payload, score: 12 } });
    expect(results()).toEqual(['0:ok', '1:chain']);
    expect(device().chain_status).toBe('broken');
    const events = db.prepare('SELECT kind FROM chain_events').all() as Row[];
    expect(events.map((e) => e.kind)).toContain('chain');
  });

  test('an entry signed by another key is a signature failure', () => {
    const intruder = newP256();
    const fake = new LedgerWriter(deviceIdFor(key.publicKey), sha256(Buffer.from('fw')), intruder.privateKey);
    publish(topic('ledger'), entryToJson(fake.append('boot', { fw_version: 'x', reset_reason: 'power_on', boot_count: 1 }, T0)));
    expect(results()).toEqual(['0:signature']);
  });

  test('entries from a different device are refused outright', () => {
    const other = new LedgerWriter(deviceIdFor(newP256().publicKey), sha256(Buffer.from('fw')), newP256().privateKey);
    publish(topic('ledger'), entryToJson(other.append('boot', { fw_version: 'x', reset_reason: 'x', boot_count: 1 }, T0)));
    expect(results()).toEqual([]);
  });

  test('a gap is held back, a resend is requested, and the chain completes in order', () => {
    const e0 = append('boot', { fw_version: 'x', reset_reason: 'x', boot_count: 0 });
    const e1 = append('boot', { fw_version: 'x', reset_reason: 'x', boot_count: 1 });
    const e2 = append('boot', { fw_version: 'x', reset_reason: 'x', boot_count: 2 });
    publish(topic('ledger'), e0);
    publish(topic('ledger'), e2);
    expect(results()).toEqual(['0:ok']);
    expect(cmds('resend')).toEqual([{ cmd: 'resend', from_seq: 1 }]);
    publish(topic('ledger'), e1);
    expect(results()).toEqual(['0:ok', '1:ok', '2:ok']);
  });

  test('a repeated entry is re-acknowledged; a different one for the same seq is a conflict', () => {
    const e0 = append('boot', { fw_version: 'x', reset_reason: 'x', boot_count: 0 });
    publish(topic('ledger'), e0);
    publish(topic('ledger'), e0);
    expect(results()).toEqual(['0:ok']);
    expect(cmds('ack').length).toBe(2);

    const rival = new LedgerWriter(deviceIdFor(key.publicKey), sha256(Buffer.from('fw')), key.privateKey);
    publish(topic('ledger'), entryToJson(rival.append('boot', { fw_version: 'y', reset_reason: 'x', boot_count: 9 }, T0)));
    expect(device().chain_status).toBe('broken');
    expect((db.prepare("SELECT COUNT(*) AS n FROM chain_events WHERE kind = 'conflict'").get() as Row).n).toBe(1);
  });

  test('a chain first seen mid-way is accepted from that point', () => {
    for (let i = 0; i < 3; i++) append('boot', { fw_version: 'x', reset_reason: 'x', boot_count: i });
    publish(topic('ledger'), append('boot', { fw_version: 'x', reset_reason: 'x', boot_count: 3 }));
    expect(results()).toEqual(['3:start']);
    expect(device().chain_status).toBe('partial');
  });

  test('entries rotated out before mirroring are recorded as a gap', () => {
    publish(topic('ledger'), append('boot', { fw_version: 'x', reset_reason: 'x', boot_count: 0 }));
    for (let i = 1; i < 5; i++) append('boot', { fw_version: 'x', reset_reason: 'x', boot_count: i }); // lost on the node
    publish(topic('hello'), { next_seq: 6, oldest_seq: 5 });
    expect(cmds('sync').pop()).toEqual({ cmd: 'sync', have_seq: 0 });
    publish(topic('ledger'), append('log_overwrite' as never, { first_seq: 1, last_seq: 4, mirrored: false }));
    expect(results()).toEqual(['0:ok', '5:gap']);
    expect(device().chain_status).toBe('gap');
    const gap = db.prepare("SELECT from_seq, to_seq FROM chain_events WHERE kind = 'gap'").get() as Row;
    expect([gap.from_seq, gap.to_seq]).toEqual([1, 4]);
  });

  test('a node whose ledger restarted opens a new epoch', () => {
    for (let i = 0; i < 3; i++) publish(topic('ledger'), append('boot', { fw_version: 'x', reset_reason: 'x', boot_count: i }));
    writer = new LedgerWriter(deviceIdFor(key.publicKey), sha256(Buffer.from('fw')), key.privateKey);
    publish(topic('hello'), { next_seq: 0, oldest_seq: 0 });
    expect(device().epoch).toBe(1);
    expect(cmds('sync').pop()).toEqual({ cmd: 'sync', have_seq: -1 });
    publish(topic('ledger'), append('boot', { fw_version: 'x', reset_reason: 'x', boot_count: 0 }));
    expect(results()).toEqual(['0:ok', '1:ok', '2:ok', '0:ok']);
  });
});

suite('incidents, media, state and telemetry', () => {
  test('CRITICAL opens an incident, its frames are linked, leaving CRITICAL closes it', () => {
    const jpeg = Buffer.concat([Buffer.from([0xff, 0xd8]), Buffer.from('frame bytes'), Buffer.from([0xff, 0xd9])]);
    const sha = createHash('sha256').update(jpeg).digest('hex');

    publish(topic('ledger'), append('state_change', { from: 'NORMAL', to: 'CRITICAL', reason: ['flame'], scores: { flame: 95 } }));
    publish(topic('ledger'), append('media_captured', { trigger: 'CRITICAL', frames: [Buffer.from(sha, 'hex')], clip: null, codec: 'jpeg', resolution: '640x480' }));
    let incident = db.prepare('SELECT * FROM incidents').get() as Row;
    expect(incident.end_seq).toBeNull();
    expect(JSON.parse(String(incident.reason))).toEqual(['flame']);

    expect(publish(`${topic('media')}/${sha}`, jpeg)).toBe(true);
    expect(cmds('media_ack')).toEqual([{ cmd: 'media_ack', sha256: sha }]);

    publish(topic('ledger'), append('state_change', { from: 'CRITICAL', to: 'NORMAL', reason: [], scores: { flame: 2 } }));
    incident = db.prepare('SELECT * FROM incidents').get() as Row;
    expect(incident.end_seq).toBe(2);

    const bundle = buildBundle(db, mediaDir, deviceId)!;
    expect(bundle.media).toEqual([sha]);
    const outcome = verifyLog(bundle.log.map(entryFromJson), publicKeyFromPem(bundle.public_key_pem), new Set(bundle.media));
    expect(outcome.ok, JSON.stringify(outcome.failure)).toBe(true);
  });

  test('an image that does not match its announced hash is refused', () => {
    const jpeg = Buffer.from('not what was announced');
    expect(publish(`${topic('media')}/${'ab'.repeat(32)}`, jpeg)).toBe(false);
    expect(cmds('media_ack')).toEqual([]);
    expect((db.prepare("SELECT COUNT(*) AS n FROM audit WHERE action = 'media_hash_mismatch'").get() as Row).n).toBe(1);
  });

  test('state and telemetry update the device', () => {
    publish(topic('state'), { fusion: 'ELEVATED', alarm: 'ELEVATED', faults: { sensor: true } });
    publish(topic('telemetry'), { scores: { gas: 62 }, fan: { state: 'on', duty: 100, rpm: 1400 } });
    expect(device().last_state).toBe('ELEVATED');
    expect(JSON.parse(String(device().faults))).toEqual({ sensor: true });
    const t = db.prepare('SELECT scores FROM telemetry').get() as Row;
    expect(JSON.parse(String(t.scores))).toEqual({ gas: 62 });
  });

  test('messages for unknown devices or the wrong site are ignored', () => {
    expect(publish(`firex/${SITE}/${'00'.repeat(16)}/state`, { fusion: 'NORMAL' })).toBe(false);
    expect(publish(`firex/other-site/${deviceId}/state`, { fusion: 'NORMAL' })).toBe(false);
    expect(publish('firex/garbage', {})).toBe(false);
  });
});
