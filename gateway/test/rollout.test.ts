import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { afterEach, beforeEach, describe as suite, expect, test } from 'vitest';

import { entryFromJson, publicKeyFromPem } from '../../ledger/src/entry.ts';
import { verifyLog } from '../../ledger/src/verify.ts';
import { ensureSite, openDb, type Db } from '../server/db.ts';
import { LiveBus } from '../server/live.ts';
import { RolloutError, RolloutManager } from '../server/rollout.ts';
import { SiteLedger } from '../server/site-ledger.ts';

const SITE = 'site-001';
const SHA = 'ab'.repeat(32);

let db: Db;
let dir: string;
let clock: number;
let sent: { topic: string; body: Record<string, unknown> }[];
let site: SiteLedger;
let rollouts: RolloutManager;

function unit(id: string, label: string, fields: { fw_version?: string; last_seen?: number | null; removed_at?: number | null } = {}): string {
  db.prepare('INSERT INTO devices (id, site_id, label, ledger_pub, tls_pub, provisioned_at, fw_version, last_seen, removed_at) VALUES (?, ?, ?, ?, ?, 0, ?, ?, ?)')
    .run(id, SITE, label, '04', '04', fields.fw_version ?? '0.4.0', fields.last_seen === undefined ? clock : fields.last_seen, fields.removed_at ?? null);
  return id;
}

const units = () => rollouts.list()[0].units.map((u) => [u.label, u.state]);
const otaFor = (id: string) => sent.filter((s) => s.body.cmd === 'ota' && s.topic.includes(id)).map((s) => s.body);
const update = (id: string, phase: string, extra: Record<string, unknown> = {}) =>
  rollouts.onFwUpdate(id, { phase, version: '0.5.0', from_version: '0.4.0', image: SHA, reason: '', ...extra });

beforeEach(() => {
  db = openDb(':memory:');
  ensureSite(db, SITE, 'Test site');
  dir = mkdtempSync(join(tmpdir(), 'firex-rollout-'));
  clock = 1_800_000_000_000;
  sent = [];
  site = new SiteLedger({ db, dir, siteId: SITE, now: () => clock });
  rollouts = new RolloutManager({
    db, live: new LiveBus(), siteLedger: site, fwHost: '192.168.1.10', fwPort: 8443, now: () => clock,
    publish: (topic, payload) => sent.push({ topic, body: JSON.parse(payload) }),
  });
  db.prepare("INSERT INTO firmware (sha256, version, project, size, signature, uploaded_by, uploaded_at) VALUES (?, '0.5.0', 'firex_node', 1200000, 'verified', 'admin', 0)").run(SHA);
});

afterEach(() => {
  db.close();
  rmSync(dir, { recursive: true, force: true });
});

suite('rollouts', () => {
  test('one unit at a time: each is offered only after the one before confirmed', () => {
    const a = unit('a'.repeat(32), 'A room');
    const b = unit('b'.repeat(32), 'B room', { fw_version: '0.5.0' });
    const c = unit('c'.repeat(32), 'C room');
    const id = rollouts.start(SHA, 'all', 'admin');

    expect(otaFor(a)).toEqual([{ cmd: 'ota', version: '0.5.0', size: 1200000, sha256: SHA, url: `https://192.168.1.10:8443/fw/${SHA}.bin` }]);
    expect(otaFor(c)).toEqual([]);
    update(a, 'accepted');
    update(a, 'downloaded');
    expect(units()).toEqual([['A room', 'downloaded'], ['B room', 'pending'], ['C room', 'pending']]);

    clock += 60_000;
    db.prepare('UPDATE devices SET last_seen = ?').run(clock);
    update(a, 'confirmed');
    expect(units()).toEqual([['A room', 'confirmed'], ['B room', 'skipped'], ['C room', 'offered']]);
    expect(rollouts.list()[0].units[1].detail).toBe('already running 0.5.0');
    update(c, 'confirmed');
    expect(rollouts.list()[0]).toMatchObject({ id, state: 'done' });

    const offered = site.bundle().log.filter((e) => e.type === 'fw_offered');
    expect(offered.map((e) => e.payload.unit)).toEqual([a, c]);
    expect(offered[0].payload.image).toBe(SHA);
    const bundle = site.bundle();
    expect(verifyLog(bundle.log.map(entryFromJson), publicKeyFromPem(bundle.public_key_pem)).ok).toBe(true);
  });

  test('a unit that fails stops the rollout; the rest are skipped; its roll-back is recorded', () => {
    const a = unit('a'.repeat(32), 'A room');
    const b = unit('b'.repeat(32), 'B room');
    unit('c'.repeat(32), 'C room');
    rollouts.start(SHA, 'all', 'admin');
    update(a, 'confirmed');
    update(b, 'failed', { reason: 'not confirmed within 60 s' });
    expect(rollouts.list()[0].state).toBe('failed');
    expect(units()).toEqual([['A room', 'confirmed'], ['B room', 'failed'], ['C room', 'skipped']]);
    update(b, 'rolled_back', { reason: 'the old image is running' });
    expect(units()[1]).toEqual(['B room', 'rolled_back']);
    expect(sent.filter((s) => s.body.cmd === 'ota')).toHaveLength(2);
  });

  test('a unit that makes no progress for 15 minutes fails the rollout', () => {
    unit('a'.repeat(32), 'A room');
    unit('b'.repeat(32), 'B room');
    rollouts.start(SHA, 'all', 'admin');
    clock += 14 * 60_000;
    rollouts.tick();
    expect(rollouts.list()[0].state).toBe('running');
    clock += 60_000;
    rollouts.tick();
    expect(rollouts.list()[0]).toMatchObject({ state: 'failed' });
    expect(units()).toEqual([['A room', 'failed'], ['B room', 'skipped']]);
  });

  test('offline and removed units are skipped with the reason', () => {
    unit('a'.repeat(32), 'A room', { last_seen: clock - 10 * 60_000 });
    const b = unit('b'.repeat(32), 'B room', { removed_at: clock });
    const c = unit('c'.repeat(32), 'C room');
    rollouts.start(SHA, ['a'.repeat(32), b, c], 'admin');
    expect(rollouts.list()[0].units.map((u) => u.detail)).toEqual(['offline when its turn came', 'removed from the site', 'offered 0.5.0']);
  });

  test('an image a unit would refuse is not rolled out, and one rollout runs at a time', () => {
    unit('a'.repeat(32), 'A room');
    db.prepare("INSERT INTO firmware (sha256, version, project, size, signature, uploaded_by, uploaded_at) VALUES (?, '0.5.1', 'firex_node', 1, 'missing', 'admin', 0)").run('cd'.repeat(32));
    expect(() => rollouts.start('cd'.repeat(32), 'all', 'admin')).toThrow(/signature is missing/);
    rollouts.start(SHA, 'all', 'admin');
    expect(() => rollouts.start(SHA, 'all', 'admin')).toThrow(RolloutError);
    expect(rollouts.stop(rollouts.list()[0].id, 'admin')).toBe(true);
    expect(rollouts.list()[0].state).toBe('stopped');
  });
});
