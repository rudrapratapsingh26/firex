import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { afterEach, beforeEach, describe as suite, expect, test } from 'vitest';

import { toHex } from '../../ledger/src/cbor.ts';
import { deviceIdFor, entryToJson, LedgerWriter, sha256 } from '../../ledger/src/entry.ts';
import { ensureSite, openDb, type Db } from '../server/db.ts';
import { Ingest } from '../server/ingest.ts';
import { LiveBus } from '../server/live.ts';
import { exportPilot } from '../server/pilot.ts';
import { SiteLedger } from '../server/site-ledger.ts';
import { encodeTrace, type TraceSample } from '../server/traces.ts';
import { newP256, pointHex } from '../sim/keys.ts';

const SITE = 'site-001';
const DAY = 86_400_000;
const FROM = Date.UTC(2026, 9, 1);
const TO = FROM + 7 * DAY;

let db: Db;
let dir: string;

beforeEach(() => {
  db = openDb(':memory:');
  ensureSite(db, SITE, 'Hostel, Block B');
  dir = mkdtempSync(join(tmpdir(), 'firex-pilot-'));
});

afterEach(() => {
  db.close();
  rmSync(dir, { recursive: true, force: true });
});

function has(table: string): boolean {
  return Boolean(db.prepare("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?").get(table));
}

suite('pilot export', () => {
  test('one site over a week: units, a labelled incident with its alerts and trace, a drill, and chains that verify', () => {
    let clock = FROM + DAY;
    const ingest = new Ingest({ db, mediaDir: join(dir, 'media'), tracesDir: join(dir, 'traces'), live: new LiveBus(), now: () => clock, publish: () => {} });
    const key = newP256();
    const id = toHex(deviceIdFor(key.publicKey));
    db.prepare("INSERT INTO devices (id, site_id, label, ledger_pub, tls_pub, provisioned_at, fw_version) VALUES (?, ?, 'Room 206', ?, ?, ?, '0.5.0')")
      .run(id, SITE, pointHex(key.publicKey), '04', FROM);
    const writer = new LedgerWriter(deviceIdFor(key.publicKey), sha256(Buffer.from('fw')), key.privateKey);
    const append = (type: Parameters<LedgerWriter['append']>[0], payload: Record<string, unknown>) => {
      clock += 1000;
      ingest.handle(`firex/${SITE}/${id}/ledger`, Buffer.from(JSON.stringify(entryToJson(writer.append(type, payload as never, Math.floor(clock / 1000), 'gateway')))));
    };

    const at = (fusion: string, gas: number, heat: number): TraceSample => ({ scores: [gas, 0.1, 0, heat, 0.4], fusion, alarm: fusion, fanDuty: 100, fanState: 'on' });
    const bytes = encodeTrace({
      periodMs: 500, preSamples: 4, trigger: 'CRITICAL', triggerUptimeMs: 900_000, bootCount: 2,
      samples: [...Array.from({ length: 4 }, () => at('NORMAL', 0.1, 0.1)), ...Array.from({ length: 6 }, () => at('CRITICAL', 0.8, 0.7)), ...Array.from({ length: 4 }, () => at('NORMAL', 0.2, 0.2))],
    });
    const traceSha = toHex(sha256(bytes));
    append('boot', { fw_version: '0.4.0', reset_reason: 'power_on', boot_count: 1 });
    append('fw_update', { phase: 'confirmed', version: '0.5.0', from_version: '0.4.0', image: Buffer.alloc(32, 7), reason: '' });
    append('boot', { fw_version: '0.5.0', reset_reason: 'software', boot_count: 2 });
    append('self_test', { result: 'pass', checks: { buzzer: true } });
    append('fault', { source: 'humidity', state: 'raised' });
    append('state_change', { from: 'NORMAL', to: 'ELEVATED', reason: ['gas'], scores: { gas: 66 } });
    append('state_change', { from: 'ELEVATED', to: 'NORMAL', reason: [], scores: { gas: 30 } });
    append('state_change', { from: 'NORMAL', to: 'CRITICAL', reason: ['gas', 'heat'], scores: { gas: 80, heat: 70 } });
    append('state_change', { from: 'CRITICAL', to: 'NORMAL', reason: [], scores: { gas: 20 } });
    append('trace_captured', { trigger: 'CRITICAL', trace: Buffer.from(traceSha, 'hex'), samples: 14, period_ms: 500, pre_samples: 4 });
    ingest.handle(`firex/${SITE}/${id}/trace/${traceSha}`, bytes);
    db.prepare("UPDATE incidents SET label = 'true', labelled_by = 'warden', labelled_at = ?").run(clock);

    const incident = Number((db.prepare('SELECT id FROM incidents').get() as { id: number }).id);
    const sent = clock + 500;
    db.prepare("INSERT INTO alert_events (incident_id, at_ms, type, service, receipt_id, payload) VALUES (?, ?, 'alert_sent', 'site', 'r-1', ?)")
      .run(incident, sent, JSON.stringify({ service: 'site', channel: 'sms', recipient: '+919876541234', receipt_id: 'r-1', delivered_at_ms: sent + 900 }));
    db.prepare("INSERT INTO alert_events (incident_id, at_ms, type, service, receipt_id, payload) VALUES (?, ?, 'alert_ack', 'site', 'r-1', ?)")
      .run(incident, sent + 41_000, JSON.stringify({ service: 'site', receipt_id: 'r-1', by: 'warden-1', latency_s: 41 }));
    if (has('drills')) {
      db.prepare("INSERT INTO drills (started_at_ms, finished_at_ms, by, services) VALUES (?, ?, 'admin', ?)").run(FROM + 2 * DAY, FROM + 2 * DAY + 90_000, JSON.stringify(['fire', 'police']));
      db.prepare("INSERT INTO drill_events (drill_id, at_ms, type, service, receipt_id, payload) VALUES (1, ?, 'alert_sent', 'fire', 'r-9', ?)")
        .run(FROM + 2 * DAY, JSON.stringify({ service: 'fire', channel: 'voice', recipient: '+911122334455', receipt_id: 'r-9' }));
      db.prepare("INSERT INTO drill_events (drill_id, at_ms, type, service, receipt_id, payload) VALUES (1, ?, 'alert_timeout', 'police', NULL, ?)")
        .run(FROM + 2 * DAY + 60_000, JSON.stringify({ service: 'police', elapsed_s: 60, action: 're_alert' }));
    }
    if (has('heartbeat_gaps')) {
      db.prepare('INSERT INTO heartbeat_gaps (device_id, start_ms, end_ms) VALUES (?, ?, ?)').run(id, FROM + 3 * DAY, FROM + 3 * DAY + 600_000);
    }
    const site = new SiteLedger({ db, dir: join(dir, 'site-ledger'), siteId: SITE, now: () => clock });
    site.append('drill', { drill_id: 1, by: 'admin', services: ['fire', 'police'], phase: 'started' });
    const registry = join(dir, 'registry.json');
    writeFileSync(registry, JSON.stringify({
      site_id: SITE, site_name: 'Hostel, Block B', contacts: [
        { service: 'site', name: 'Warden', recipient: '+919876541234', channel: 'sms', mode: 'live' },
        { service: 'fire', name: 'Station 42', recipient: '+911122334455', channel: 'voice' },
      ],
    }));

    const out = join(dir, 'export');
    const doc = exportPilot({ db, siteId: SITE, siteLedger: site, mediaDir: join(dir, 'media'), tracesDir: join(dir, 'traces'), registryPath: registry, fromMs: FROM, toMs: TO, out, now: () => clock }) as any;

    expect(doc).toMatchObject({ kind: 'pilot_site', synthetic: false, site: { id: SITE, name: 'Hostel, Block B' }, period: { from_ms: FROM, to_ms: TO } });
    expect(doc.registry.services).toEqual({ site: { mode: 'live', agreement: null }, fire: { mode: 'drill', agreement: null } });
    expect(doc.units).toHaveLength(1);
    expect(doc.units[0]).toMatchObject({
      id, label: 'Room 206', monitored_s: 7 * 86_400, self_tests: { pass: 1, fail: 0 }, faults_raised: 1, elevated_episodes: 1,
      fw_versions: ['0.4.0', '0.5.0'], fw_updates: [{ phase: 'confirmed', version: '0.5.0', from_version: '0.4.0' }],
    });
    if (has('heartbeat_gaps')) expect(doc.units[0]).toMatchObject({ gap_s: 600, gaps: [{ seconds: 600 }] });

    expect(doc.incidents).toHaveLength(1);
    expect(doc.incidents[0]).toMatchObject({ id: incident, unit: id, ts_src: 'gateway', reason: ['gas', 'heat'], label: 'true', labelled_by: 'warden', traces: [traceSha] });
    expect(doc.incidents[0].alerts).toEqual([{
      service: 'site', channel: 'sms', recipient_masked: '+91******1234', drill: false, sent_at_ms: sent, delivered_at_ms: sent + 900,
      acked_at_ms: sent + 41_000, acked_by: 'warden-1', latency_s: 41, timeouts: 0, re_alerts: 0, failed: 0,
    }]);
    if (has('drills')) {
      expect(doc.drills[0]).toMatchObject({ id: 1, by: 'admin', services: ['fire', 'police'] });
      expect(doc.drills[0].alerts.map((a: any) => [a.service, a.drill, a.recipient_masked, a.timeouts])).toEqual([['fire', true, '+91******4455', 0], ['police', true, null, 1]]);
    }

    expect(doc.site_ledger.verify).toMatch(/^PASS/);
    expect(doc.unit_bundles).toEqual([{ unit: id, file: `bundles/${id}.json`, entries: 10, verify: expect.stringMatching(/^PASS/) }]);
    expect(doc.traces).toEqual([expect.objectContaining({ sha256: traceSha, unit: id, incident, samples: 14, matched: 2 })]);
    for (const f of ['pilot.json', 'site-ledger.json', `bundles/${id}.json`, `traces/${traceSha}.csv`, `traces/${traceSha}.events.jsonl`]) {
      expect(existsSync(join(out, f)), f).toBe(true);
    }
    expect(readFileSync(join(out, 'pilot.json'), 'utf8')).not.toContain('9876541234');
    expect(readFileSync(join(out, `traces/${traceSha}.events.jsonl`), 'utf8')).toContain(`trial hazard unit-room-206-incident-${incident}`);

    /* The pilot report (tools/pilot) reads this export. The two sides were written from one
     * spec by different hands; this is where a disagreement between them shows. */
    const python = ['python3', 'python'].find((cmd) => spawnSync(cmd, ['--version']).status === 0);
    if (python) {
      const report = join(dir, 'report');
      const run = spawnSync(python, [join('tools', 'pilot', 'pilot_report.py'), out, '--out', report], { encoding: 'utf8' });
      expect(run.status, run.stderr || run.stdout).toBe(0);
      const parsed = JSON.parse(readFileSync(`${report}.json`, 'utf8'));
      expect(parsed).toMatchObject({ kind: 'pilot', synthetic: false });
      expect(parsed.sites).toHaveLength(1);
      expect(readFileSync(`${report}.md`, 'utf8')).not.toContain('9876541234');
    }
  });

  test('entries received outside the period are not counted', () => {
    const doc = exportPilot({
      db, siteId: SITE, siteLedger: new SiteLedger({ db, dir: join(dir, 'sl'), siteId: SITE }), mediaDir: join(dir, 'media'),
      tracesDir: join(dir, 'traces'), registryPath: null, fromMs: FROM, toMs: TO, out: join(dir, 'empty'),
    }) as any;
    expect(doc).toMatchObject({ units: [], incidents: [], drills: [], traces: [], registry: { sha256: null, services: {} }, site_ledger: { entries: 0, verify: 'empty' } });
  });
});
