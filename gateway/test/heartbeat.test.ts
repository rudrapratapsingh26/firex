/**
 * Heartbeat gaps: a unit goes quiet, the gap opens after 90 s, the site hears
 * about it once after the registry's offline_notify_min, and the gap closes into
 * a signed heartbeat_gap entry when the unit is heard again.
 */
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { beforeEach, describe as suite, expect, test } from 'vitest';

import { entryFromJson, publicKeyFromPem } from '../../ledger/src/entry.ts';
import { verifyLog } from '../../ledger/src/verify.ts';
import { createUser } from '../server/auth.ts';
import { loadConfig } from '../server/config.ts';
import { ensureSite, openDb, type Db, type Row } from '../server/db.ts';
import { HeartbeatMonitor } from '../server/heartbeat.ts';
import { buildApp } from '../server/http.ts';
import { LiveBus, type LiveEvent } from '../server/live.ts';
import { SiteLedger } from '../server/site-ledger.ts';

const SITE = 'site-001';
const DEVICE = 'ab'.repeat(16);

let db: Db;
let live: LiveBus;
let clock: number;
let site: SiteLedger;
let notices: string[];
let events: LiveEvent[];
let monitor: HeartbeatMonitor;

const gaps = () => db.prepare('SELECT * FROM heartbeat_gaps ORDER BY id').all() as Row[];

beforeEach(() => {
  db = openDb(':memory:');
  ensureSite(db, SITE, 'Test site');
  live = new LiveBus();
  clock = 1_800_000_000_000;
  site = new SiteLedger({ db, dir: mkdtempSync(join(tmpdir(), 'firex-hb-')), siteId: SITE, now: () => clock });
  db.prepare("INSERT INTO devices (id, site_id, label, ledger_pub, tls_pub, provisioned_at, last_seen) VALUES (?, ?, 'Room 206', 'x', 'y', 0, ?)")
    .run(DEVICE, SITE, clock);
  notices = [];
  events = [];
  live.subscribe((e) => events.push(e));
  monitor = new HeartbeatMonitor({ db, live, siteLedger: site, notify: async (text) => notices.push(text), notifyAfterMin: 10, now: () => clock });
  monitor.start();
});

suite('heartbeat gaps', () => {
  test('silence opens a gap, a long one notifies the site once, and hearing the unit closes it into the site ledger', () => {
    const lastSeen = clock;
    clock += 60_000;
    monitor.check();
    expect(gaps()).toHaveLength(0);

    clock += 35_000; // 95 s of silence
    monitor.check();
    expect(gaps()).toMatchObject([{ device_id: DEVICE, start_ms: lastSeen, end_ms: null }]);
    expect(events).toContainEqual({ kind: 'gap', device_id: DEVICE, open: true });

    clock = lastSeen + 9 * 60_000;
    monitor.check();
    expect(notices).toHaveLength(0);
    clock = lastSeen + 11 * 60_000;
    monitor.check();
    monitor.check();
    expect(notices).toHaveLength(1);
    expect(notices[0]).toMatch(/^FireX: unit Room 206 offline since /);

    clock = lastSeen + 20 * 60_000;
    db.prepare('UPDATE devices SET last_seen = ? WHERE id = ?').run(clock, DEVICE);
    live.publish({ kind: 'telemetry', device_id: DEVICE, scores: {}, at: clock });
    expect(gaps()[0].end_ms).toBe(clock);
    expect(events).toContainEqual({ kind: 'gap', device_id: DEVICE, open: false });

    const bundle = site.bundle();
    expect(bundle.log.map((e) => e.type)).toEqual(['heartbeat_gap']);
    expect(bundle.log[0].payload).toEqual({ unit: DEVICE, start_ts: lastSeen / 1000, end_ts: clock / 1000, seconds: 1200 });
    expect(verifyLog(bundle.log.map(entryFromJson), publicKeyFromPem(bundle.public_key_pem)).ok).toBe(true);
  });

  test('a gap found closed by the periodic check ends at the unit\'s last message', () => {
    clock += 120_000;
    monitor.check();
    const back = clock + 5_000;
    db.prepare('UPDATE devices SET last_seen = ? WHERE id = ?').run(back, DEVICE);
    clock += 10_000;
    monitor.check();
    expect(gaps()[0].end_ms).toBe(back);
    expect(site.bundle().log).toHaveLength(1);
  });

  test('a removed unit, and a unit never heard from, open no gap', () => {
    db.prepare("INSERT INTO devices (id, site_id, label, ledger_pub, tls_pub, provisioned_at) VALUES (?, ?, 'Spare', 'x', 'y', 0)").run('cd'.repeat(16), SITE);
    db.prepare('UPDATE devices SET removed_at = ? WHERE id = ?').run(clock, DEVICE);
    clock += 3_600_000;
    monitor.check();
    expect(gaps()).toHaveLength(0);
  });

  test('the devices API shows when a unit went offline and its gaps', async () => {
    clock += 100_000;
    monitor.check();
    const app = await buildApp({ db, config: loadConfig({ FIREX_DATA_DIR: tmpdir(), FIREX_SITE_ID: SITE }), live, mqttConnected: () => false, now: () => clock });
    try {
      createUser(db, 'viewer', 'viewer password 1', 'viewer');
      const login = await app.inject({ method: 'POST', url: '/api/login', payload: { username: 'viewer', password: 'viewer password 1' } });
      const cookie = String(login.headers['set-cookie']).split(';')[0];
      const list = (await app.inject({ method: 'GET', url: '/api/devices', headers: { cookie } })).json() as { offline_since: number | null }[];
      expect(list[0].offline_since).toBe(clock - 100_000);
      const detail = (await app.inject({ method: 'GET', url: `/api/devices/${DEVICE}`, headers: { cookie } })).json() as { gaps: unknown[] };
      expect(detail.gaps).toEqual([{ start_ms: clock - 100_000, end_ms: null, seconds: 100 }]);
    } finally {
      await app.close();
      monitor.stop();
    }
  });
});
