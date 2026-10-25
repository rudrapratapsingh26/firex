import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { afterEach, beforeEach, describe as suite, expect, test } from 'vitest';

import { fromHex } from '../../ledger/src/cbor.ts';
import { entryFromJson, publicKeyFromPem } from '../../ledger/src/entry.ts';
import { verifyLog } from '../../ledger/src/verify.ts';
import { openDb, type Db } from '../server/db.ts';
import { SiteLedger } from '../server/site-ledger.ts';

let dir: string;
let db: Db;
let clock: number;

beforeEach(() => {
  dir = mkdtempSync(join(tmpdir(), 'firex-site-ledger-'));
  db = openDb(':memory:');
  clock = 1_789_000_000_000;
});

afterEach(() => {
  db.close();
  rmSync(dir, { recursive: true, force: true });
});

const ledger = () => new SiteLedger({ db, dir, siteId: 'site-001', version: 'test', now: () => clock, clockSource: () => 'ntp' });

suite('site ledger', () => {
  test('entries written by the gateway verify with the reference verifier', () => {
    const site = ledger();
    site.append('alert_sent', {
      service: 'fire', channel: 'sms', recipient: '+910000000042', receipt_id: 'r-0001', incident_id: '7',
      unit: 'ab'.repeat(16), incident: 7, critical_seq: 41, critical_hash: fromHex('cd'.repeat(32)), drill: false,
    });
    clock += 34_000;
    site.append('alert_ack', { service: 'fire', receipt_id: 'r-0001', by: 'station-042', latency_s: 34, unit: 'ab'.repeat(16), incident: 7, drill: false });
    site.append('heartbeat_gap', { unit: 'ab'.repeat(16), start_ts: 1_789_000_100, end_ts: 1_789_000_700, seconds: 600 });

    const bundle = site.bundle();
    expect(bundle.log.map((e) => e.seq)).toEqual([0, 1, 2]);
    expect(bundle.log[0].payload.critical_hash).toBe('cd'.repeat(32));
    const result = verifyLog(bundle.log.map(entryFromJson), publicKeyFromPem(bundle.public_key_pem));
    expect(result.ok, JSON.stringify(result.failure)).toBe(true);
  });

  test('a restarted gateway continues the same chain with the same key', () => {
    const first = ledger();
    first.append('drill', { drill_id: 1, by: 'admin', services: ['fire', 'police', 'ambulance', 'site'], phase: 'started' });
    const second = ledger();
    expect(second.deviceId).toBe(first.deviceId);
    second.append('drill', { drill_id: 1, by: 'admin', services: ['fire', 'police', 'ambulance', 'site'], phase: 'finished' });
    const bundle = second.bundle();
    expect(verifyLog(bundle.log.map(entryFromJson), publicKeyFromPem(bundle.public_key_pem)).ok).toBe(true);
  });

  test('a clock that steps back is held at the last entry, never written backwards', () => {
    const site = ledger();
    site.append('registry_change', { old_registry: null, new_registry: fromHex('11'.repeat(32)), by: 'admin' });
    clock -= 3_600_000;
    const held = site.append('registry_change', { old_registry: fromHex('11'.repeat(32)), new_registry: fromHex('22'.repeat(32)), by: 'admin' });
    expect(held.ts).toBe(Math.floor(1_789_000_000_000 / 1000));
    expect(site.entries({ limit: 1 })[0].seq).toBe(1);
  });

  test('an edited row fails verification at that entry', () => {
    const site = ledger();
    site.append('incident_label', { unit: 'ab'.repeat(16), incident: 7, critical_seq: 41, critical_hash: fromHex('cd'.repeat(32)), label: 'true', by: 'warden' });
    site.append('incident_label', { unit: 'ab'.repeat(16), incident: 8, critical_seq: 57, critical_hash: fromHex('ef'.repeat(32)), label: 'test', by: 'warden' });
    db.prepare("UPDATE site_entries SET payload = replace(payload, '\"test\"', '\"nuisance\"') WHERE seq = 1").run();
    const bundle = site.bundle();
    const result = verifyLog(bundle.log.map(entryFromJson), publicKeyFromPem(bundle.public_key_pem));
    expect(result.ok).toBe(false);
    expect(result.failure).toMatchObject({ seq: 1, tier: 'chain' });
  });
});
