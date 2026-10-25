/**
 * The escalation runner beside the real ingest, with no broker: a CRITICAL
 * entry arrives, the incident opens, and the site and the live services are
 * alerted, answered or chased. Everything lands in alert_events and, signed, in
 * the site ledger. Then services still in drill mode, late alarms from a
 * backlog, a gateway restart, SMS replies, the dashboard's acknowledgement and
 * drills.
 */
import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { afterEach, beforeEach, describe as suite, expect, test } from 'vitest';

import { toHex } from '../../ledger/src/cbor.ts';
import { deviceIdFor, entryFromJson, entryToJson, LedgerWriter, publicKeyFromPem, sha256, type EntryBody, type EntryJson } from '../../ledger/src/entry.ts';
import { verifyLog } from '../../ledger/src/verify.ts';
import { ConsoleAlert } from '../escalation/console-alert.ts';
import type { ModemPort } from '../escalation/modem.ts';
import type { Reply, ServiceContact, SiteRegistry } from '../escalation/provider.ts';
import { createUser, type Role } from '../server/auth.ts';
import { loadConfig } from '../server/config.ts';
import { ensureSite, openDb, type Db, type Row } from '../server/db.ts';
import { EscalationRunner, escalationFromConfig, queueDrill, type EscalationRunnerOptions } from '../server/escalate.ts';
import { buildApp, type AppDeps } from '../server/http.ts';
import { Ingest } from '../server/ingest.ts';
import { LiveBus } from '../server/live.ts';
import { SiteLedger } from '../server/site-ledger.ts';
import { newP256, pointHex, type KeyPair } from '../sim/keys.ts';

const SITE = 'site-001';
const T0 = 1_789_000_000;
const AGREEMENT = { reference: 'FS-42/2026/pilot', date: '2026-10-01' };

function registryOf(mode: 'live' | 'drill'): SiteRegistry {
  const service = (c: ServiceContact): ServiceContact => ({ ...c, mode, ...(mode === 'live' ? { agreement: AGREEMENT } : {}) });
  return {
    site_id: SITE, site_name: 'Hostel, Block B', address: '', location: { lat: 0, lon: 0 }, version: 2,
    contacts: [
      { service: 'site', name: 'Warden on duty', recipient: '+919800000001', channel: 'sms', mode: 'live' },
      service({ service: 'fire', name: 'Fire station 42', recipient: 'station-042', channel: 'sms', backup: { name: 'Fire station 17', recipient: 'station-017', channel: 'voice' } }),
      service({ service: 'police', name: 'Police station 17', recipient: 'ps-017', channel: 'sms' }),
      service({ service: 'ambulance', name: 'EMS north', recipient: 'ems-north', channel: 'api' }),
    ],
  };
}

/** ConsoleAlert that also hears replies, as the modem does. */
class Replying extends ConsoleAlert {
  private readonly listeners: ((reply: Reply) => void)[] = [];

  onReply(listener: (reply: Reply) => void): void {
    this.listeners.push(listener);
  }

  reply(from: string, body: string, at: number): void {
    for (const listener of this.listeners) listener({ from, body, at });
  }
}

let db: Db;
let live: LiveBus;
let ingest: Ingest;
let provider: Replying;
let site: SiteLedger;
let key: KeyPair;
let deviceId: string;
let writer: LedgerWriter;
let clock: number;
const runners: EscalationRunner[] = [];

const until = async (what: string, predicate: () => boolean, timeoutMs = 5000) => {
  const deadline = Date.now() + timeoutMs;
  while (!predicate()) {
    if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
    await new Promise((r) => setTimeout(r, 5));
  }
};

const parse = (rows: Row[]) => rows.map((r) => ({ ...r, payload: JSON.parse(String(r.payload)) }) as Row & { payload: Record<string, unknown> });
const events = () => parse(db.prepare('SELECT * FROM alert_events ORDER BY id').all() as Row[]);
const ofType = (type: string) => events().filter((e) => e.type === type);
const drillEvents = (type: string) => parse(db.prepare('SELECT * FROM drill_events WHERE type = ? ORDER BY id').all(type) as Row[]);
const incident = (id = 1) => db.prepare('SELECT late, escalation_state, escalation_reason FROM incidents WHERE id = ?').get(id) as Row;
const siteLog = () => site.bundle().log;
const siteVerifies = () => {
  const bundle = site.bundle();
  return verifyLog(bundle.log.map(entryFromJson), publicKeyFromPem(bundle.public_key_pem)).ok;
};

function publish(entry: EntryJson): void {
  ingest.handle(`firex/${SITE}/${deviceId}/ledger`, Buffer.from(JSON.stringify(entry)));
}

function append(type: Parameters<LedgerWriter['append']>[0], payload: Record<string, unknown>, ts = T0 + writer.entries.length, tsSrc: EntryBody['ts_src'] = 'uncertain'): EntryJson {
  return entryToJson(writer.append(type, payload as never, ts, tsSrc));
}

const critical = () => publish(append('state_change', { from: 'NORMAL', to: 'CRITICAL', reason: ['flame'], scores: { flame: 95 } }));

function startRunner(registry: SiteRegistry, extra: Partial<EscalationRunnerOptions> = {}): EscalationRunner {
  const runner = new EscalationRunner({
    db, live, registry, provider, siteLedger: site, ackTimeoutS: 60, maxReAlerts: 1, pollMs: 2, now: () => clock, drainWaitMs: 300, ...extra,
  });
  runner.start();
  runners.push(runner);
  return runner;
}

async function signIn(role: Role, deps: Partial<AppDeps> = {}) {
  const config = loadConfig({ FIREX_DATA_DIR: mkdtempSync(join(tmpdir(), 'firex-api-')), FIREX_SITE_ID: SITE });
  const app = await buildApp({ db, config, live, mqttConnected: () => false, now: () => clock, siteLedger: site, ...deps });
  createUser(db, role, `${role} password 1`, role, clock);
  const login = await app.inject({ method: 'POST', url: '/api/login', payload: { username: role, password: `${role} password 1` } });
  return { app, cookie: String(login.headers['set-cookie']).split(';')[0] };
}

beforeEach(() => {
  db = openDb(':memory:');
  ensureSite(db, SITE, 'Test site');
  live = new LiveBus();
  clock = 1_800_000_000_000;
  ingest = new Ingest({ db, mediaDir: mkdtempSync(join(tmpdir(), 'firex-media-')), live, now: () => clock, publish: () => {} });
  key = newP256();
  deviceId = toHex(deviceIdFor(key.publicKey));
  db.prepare("INSERT INTO devices (id, site_id, label, ledger_pub, tls_pub, provisioned_at) VALUES (?, ?, 'Room 206', ?, ?, 0)")
    .run(deviceId, SITE, pointHex(key.publicKey), pointHex(newP256().publicKey));
  writer = new LedgerWriter(deviceIdFor(key.publicKey), sha256(Buffer.from('fw')), key.privateKey);
  provider = new Replying(() => clock / 1000, () => {});
  site = new SiteLedger({ db, dir: mkdtempSync(join(tmpdir(), 'firex-site-')), siteId: SITE, now: () => clock });
});

afterEach(() => {
  for (const runner of runners.splice(0)) runner.stop();
});

suite('escalation on the gateway', () => {
  test('a verified CRITICAL alerts the site and every live service; answers, silence and escalation are recorded and signed', async () => {
    const runner = startRunner(registryOf('live'));
    publish(append('state_change', { from: 'ELEVATED', to: 'CRITICAL', reason: ['gas', 'heat'], scores: { gas: 81, heat: 64 } }));
    await until('four alerts', () => ofType('alert_sent').length === 4);

    const sent = ofType('alert_sent');
    expect(sent.map((e) => e.service)).toEqual(['site', 'fire', 'police', 'ambulance']);
    expect(sent.every((e) => e.at_ms === clock && e.incident_id === 1)).toBe(true);
    expect(sent[1].payload).toMatchObject({ recipient: 'station-042', incident_id: '1' });
    expect(provider.texts[0]).toBe('FIREX CRITICAL Hostel, Block B: Room 206 gas + heat agree. Reply 1 to acknowledge.');
    expect(incident()).toMatchObject({ late: 0, escalation_state: 'running', escalation_reason: null });

    const fire = sent.find((e) => e.service === 'fire')!;
    clock += 34_000;
    provider.acknowledge(String(fire.receipt_id), 'station-042');
    await until('the fire station answer', () => ofType('alert_ack').length === 1);
    expect(ofType('alert_ack')[0]).toMatchObject({ service: 'fire', at_ms: clock, payload: { latency_s: 34, by: 'station-042' } });

    clock += 27_000; // 61 s after the first alerts
    await until('re-alerts to the three silent recipients', () => ofType('alert_timeout').length === 3);
    expect(ofType('alert_timeout').map((e) => e.payload.action)).toEqual(['re_alert', 're_alert', 're_alert']);
    await until('the re-alerts', () => ofType('alert_sent').length === 7);
    expect(ofType('alert_sent').slice(4).every((e) => typeof e.payload.retry_of === 'string')).toBe(true);

    clock += 61_000;
    await runner.idle();
    expect(ofType('alert_timeout').filter((e) => e.payload.action === 'escalate')).toHaveLength(3);
    expect(runner.running).toBe(0);
    expect(incident()).toMatchObject({ escalation_state: 'finished', escalation_reason: null });

    expect(siteVerifies()).toBe(true);
    const signed = siteLog();
    const stored = db.prepare('SELECT seq, hash FROM ledger_entries').get() as Row;
    expect(signed.filter((e) => e.type === 'alert_sent')).toHaveLength(7);
    expect(signed[0].payload).toMatchObject({
      service: 'site', unit: deviceId, incident: 1, incident_id: '1', critical_seq: stored.seq, critical_hash: stored.hash, drill: false,
    });
    expect(signed.find((e) => e.type === 'alert_ack')?.payload).toMatchObject({ service: 'fire', by: 'station-042', latency_s: 34, unit: deviceId, drill: false });
    expect(signed.filter((e) => e.type === 'alert_timeout').map((e) => e.payload.action)).toEqual(['re_alert', 're_alert', 're_alert', 'escalate', 'escalate', 'escalate']);
  });

  test('the incidents API lists what was sent, and how the escalation stands', async () => {
    startRunner(registryOf('live'));
    critical();
    await until('four alerts', () => ofType('alert_sent').length === 4);
    const { app, cookie } = await signIn('viewer');
    try {
      const incidents = (await app.inject({ method: 'GET', url: '/api/incidents', headers: { cookie } })).json() as {
        late: boolean; escalation: { state: string }; alerts: { type: string; service: string; payload: { recipient: string } }[];
      }[];
      expect(incidents[0].alerts.map((a) => `${a.type}:${a.service}`)).toEqual(['alert_sent:site', 'alert_sent:fire', 'alert_sent:police', 'alert_sent:ambulance']);
      expect(incidents[0].alerts[2].payload.recipient).toBe('ps-017');
      expect(incidents[0]).toMatchObject({ late: false, escalation: { state: 'running' } });
    } finally {
      await app.close();
    }
  });

  test('a CRITICAL entry that failed verification alerts nobody, and the withholding is signed', async () => {
    const runner = startRunner(registryOf('live'));
    const genuine = append('state_change', { from: 'NORMAL', to: 'CRITICAL', reason: ['flame'], scores: { flame: 95 } });
    publish({ ...genuine, payload: { ...genuine.payload, scores: { flame: 99 } } });
    expect((db.prepare('SELECT verify_result FROM ledger_entries').get() as Row).verify_result).toBe('chain');
    await runner.idle();
    expect(events()).toHaveLength(0);
    expect((db.prepare("SELECT COUNT(*) AS n FROM audit WHERE action = 'escalation_withheld'").get() as Row).n).toBe(1);
    expect(siteLog().map((e) => e.payload)).toEqual([{ unit: deviceId, incident: 1, critical_seq: 0, reason: 'verification' }]);
    expect(incident()).toMatchObject({ escalation_state: 'withheld', escalation_reason: 'verification' });
  });

  test('an incident is escalated once', async () => {
    const runner = startRunner(registryOf('live'));
    critical();
    await until('four alerts', () => ofType('alert_sent').length === 4);
    runner.escalate(1);
    live.publish({ kind: 'incident', device_id: deviceId, incident_id: 1, open: true });
    await new Promise((r) => setTimeout(r, 30));
    expect(ofType('alert_sent')).toHaveLength(4);
  });
});

suite('who a real incident reaches', () => {
  test('services still in drill mode are not alerted, and the site ledger says so', async () => {
    const runner = startRunner(registryOf('drill'));
    critical();
    await until('the site alert', () => siteLog().some((e) => e.type === 'escalation_withheld'));
    expect(ofType('alert_sent').map((e) => e.service)).toEqual(['site']);
    expect(siteLog().map((e) => e.type)).toEqual(['alert_sent', 'escalation_withheld']);
    expect(siteLog()[1].payload).toMatchObject({ reason: 'drill_mode', unit: deviceId, incident: 1 });

    provider.acknowledge(String(ofType('alert_sent')[0].receipt_id), 'warden');
    await until('the answer', () => ofType('alert_ack').length === 1);
    await runner.idle();
    expect(incident()).toMatchObject({ escalation_state: 'withheld', escalation_reason: 'drill_mode' });
  });

  test('a CRITICAL from a unit\'s backlog, already over, reaches the site only, marked late', async () => {
    const runner = startRunner(registryOf('live'));
    db.prepare('UPDATE devices SET hello_next_seq = 4 WHERE id = ?').run(deviceId); // the unit connected holding entries 0-3
    publish(append('state_change', { from: 'WARMUP', to: 'NORMAL', reason: [], scores: {} }));
    publish(append('state_change', { from: 'NORMAL', to: 'CRITICAL', reason: ['flame'], scores: { flame: 95 } }));
    publish(append('state_change', { from: 'CRITICAL', to: 'NORMAL', reason: [], scores: { flame: 10 } }));
    publish(append('threshold_cross', { modality: 'flame', score: 10, direction: 'down' }));

    await until('the late notice', () => siteLog().some((e) => e.type === 'escalation_withheld'));
    expect(ofType('alert_sent').map((e) => e.service)).toEqual(['site']);
    expect(provider.texts[0]).toBe('FIREX LATE Hostel, Block B: Room 206 flame. Reported late: unit was offline. Reply 1 to acknowledge.');
    expect(incident()).toMatchObject({ late: 1, escalation_state: 'running', escalation_reason: 'late' });
    expect(siteLog().find((e) => e.type === 'escalation_withheld')?.payload).toMatchObject({ reason: 'late', critical_seq: 1 });

    provider.acknowledge(String(ofType('alert_sent')[0].receipt_id), 'warden');
    await runner.idle();
    expect(incident()).toMatchObject({ escalation_state: 'withheld', escalation_reason: 'late' });
  });

  test('a backlog CRITICAL whose trusted time is recent is escalated in full', async () => {
    startRunner(registryOf('live'));
    db.prepare('UPDATE devices SET hello_next_seq = 3 WHERE id = ?').run(deviceId);
    const now = Math.floor(clock / 1000);
    publish(append('state_change', { from: 'NORMAL', to: 'CRITICAL', reason: ['flame'], scores: { flame: 95 } }, now - 30, 'gateway'));
    publish(append('state_change', { from: 'CRITICAL', to: 'NORMAL', reason: [], scores: { flame: 10 } }, now - 20, 'gateway'));
    publish(append('threshold_cross', { modality: 'flame', score: 10, direction: 'down' }, now - 19, 'gateway'));
    await until('four alerts', () => ofType('alert_sent').length === 4);
    expect(incident()).toMatchObject({ late: 0, escalation_reason: null });
  });
});

suite('acknowledgements and restarts', () => {
  test('an escalation carries on after the gateway restarts', async () => {
    const first = startRunner(registryOf('live'));
    critical();
    await until('four alerts', () => ofType('alert_sent').length === 4);
    first.stop();
    expect(incident().escalation_state).toBe('running');

    clock += 61_000;
    const second = startRunner(registryOf('live'));
    await until('timeouts after the restart', () => ofType('alert_timeout').length === 4);
    await until('the re-alerts', () => ofType('alert_sent').length === 8);
    expect(ofType('alert_sent').slice(4).every((e) => typeof e.payload.retry_of === 'string')).toBe(true);
    clock += 61_000;
    await second.idle();
    expect(incident().escalation_state).toBe('finished');
    expect(siteVerifies()).toBe(true);
  });

  test('an escalation older than an hour is not resumed', async () => {
    const first = startRunner(registryOf('live'));
    critical();
    await until('four alerts', () => ofType('alert_sent').length === 4);
    first.stop();
    clock += 2 * 3_600_000;
    const second = startRunner(registryOf('live'));
    await second.idle();
    expect(ofType('alert_timeout')).toHaveLength(0);
    expect(incident()).toMatchObject({ escalation_state: 'finished', escalation_reason: 'not resumed after a gateway restart' });
  });

  test('the site acknowledges on the dashboard', async () => {
    const runner = startRunner(registryOf('live'));
    critical();
    await until('four alerts', () => ofType('alert_sent').length === 4);
    const { app, cookie } = await signIn('operator', { escalation: runner });
    try {
      const res = await app.inject({ method: 'POST', url: '/api/incidents/1/ack', headers: { cookie } });
      expect(res.statusCode).toBe(200);
      expect(res.json()).toEqual({ ok: true, recorded: 'booked' });
      await until('the site answer', () => ofType('alert_ack').length === 1);
      expect(ofType('alert_ack')[0]).toMatchObject({ service: 'site', payload: { by: 'operator' } });
      expect((await app.inject({ method: 'POST', url: '/api/incidents/1/ack', headers: { cookie } })).statusCode).toBe(409);
      expect((await app.inject({ method: 'POST', url: '/api/incidents/99/ack', headers: { cookie } })).statusCode).toBe(404);
    } finally {
      await app.close();
    }
  });

  test('a label set on the dashboard is signed into the site ledger with the CRITICAL entry it names', async () => {
    startRunner(registryOf('live'));
    critical();
    await until('four alerts', () => ofType('alert_sent').length === 4);
    const { app, cookie } = await signIn('operator');
    try {
      expect((await app.inject({ method: 'POST', url: '/api/incidents/1/label', headers: { cookie }, payload: { label: 'test' } })).statusCode).toBe(200);
      const stored = db.prepare('SELECT seq, hash FROM ledger_entries').get() as Row;
      expect(siteLog().find((e) => e.type === 'incident_label')?.payload).toEqual({
        unit: deviceId, incident: 1, critical_seq: stored.seq, critical_hash: stored.hash, label: 'test', by: 'operator',
      });
      expect(siteVerifies()).toBe(true);
    } finally {
      await app.close();
    }
  });

  test('an SMS reply of 1, OK or ACK from a recipient acknowledges their newest alert; anything else is kept, unmatched', async () => {
    startRunner(registryOf('live'));
    critical();
    await until('four alerts', () => ofType('alert_sent').length === 4);
    provider.reply('+91 98000-00001', 'Fire?', clock / 1000);
    provider.reply('09800000001', ' ok ', clock / 1000 + 5);
    await until('the warden answer', () => ofType('alert_ack').length === 1);
    expect(ofType('alert_ack')[0]).toMatchObject({ service: 'site', payload: { by: '09800000001', latency_s: 5 } });
    const inbox = db.prepare('SELECT sender, body, matched_receipt FROM sms_inbox ORDER BY id').all() as Row[];
    expect(inbox).toEqual([
      { sender: '+91 98000-00001', body: 'Fire?', matched_receipt: null },
      { sender: '09800000001', body: ' ok ', matched_receipt: ofType('alert_sent')[0].receipt_id },
    ]);
  });
});

suite('drills', () => {
  test('a drill reaches every chosen contact whatever its mode, says DRILL, and is signed from start to finish', async () => {
    const runner = startRunner(registryOf('drill'));
    const { app, cookie } = await signIn('admin', { escalation: runner });
    try {
      const started = await app.inject({ method: 'POST', url: '/api/drills', headers: { cookie }, payload: { services: ['fire', 'police', 'ambulance', 'site'] } });
      expect(started.json()).toEqual({ id: 1 });
      await until('four drill alerts', () => drillEvents('alert_sent').length === 4);
      expect(provider.texts).toHaveLength(4);
      expect(provider.texts.every((t) => t.startsWith('DRILL - NOT A REAL INCIDENT. FIREX Hostel, Block B: test alert for '))).toBe(true);

      for (const e of drillEvents('alert_sent')) provider.acknowledge(String(e.receipt_id), `answered ${e.service}`);
      await until('four answers', () => drillEvents('alert_ack').length === 4);
      await runner.idle();

      const drills = (await app.inject({ method: 'GET', url: '/api/drills', headers: { cookie } })).json() as Row[];
      expect(drills[0]).toMatchObject({ id: 1, by: 'admin', services: ['fire', 'police', 'ambulance', 'site'], finished_at_ms: clock });
      expect(drills[0].alerts).toHaveLength(8);
      expect(events()).toHaveLength(0);

      expect(siteVerifies()).toBe(true);
      const signed = siteLog();
      expect(signed.map((e) => e.type)).toEqual(['drill', ...Array(4).fill('alert_sent'), ...Array(4).fill('alert_ack'), 'drill']);
      expect(signed[0].payload).toMatchObject({ drill_id: 1, by: 'admin', phase: 'started' });
      expect(signed[1].payload).toMatchObject({ drill: true, unit: '', incident: 0, critical_seq: 0, critical_hash: '00'.repeat(32), incident_id: 'drill-1' });
      expect(signed.at(-1)?.payload).toMatchObject({ drill_id: 1, phase: 'finished' });

      expect((await app.inject({ method: 'POST', url: '/api/drills', headers: { cookie }, payload: { services: ['navy'] } })).statusCode).toBe(400);
    } finally {
      await app.close();
    }
  });

  test('a drill queued from the command line is started by the running gateway', async () => {
    queueDrill(db, ['site'], 'cli', clock);
    startRunner(registryOf('drill'));
    await until('the queued drill', () => drillEvents('alert_sent').length === 1);
    expect(siteLog()[0]).toMatchObject({ type: 'drill', payload: { by: 'cli', services: ['site'], phase: 'started' } });
  });

  test('drills are refused when escalation is off', async () => {
    const { app, cookie } = await signIn('admin', { escalation: null });
    try {
      expect((await app.inject({ method: 'POST', url: '/api/drills', headers: { cookie }, payload: { services: ['site'] } })).statusCode).toBe(409);
    } finally {
      await app.close();
    }
  });
});

suite('escalation settings', () => {
  const env = (extra: Record<string, string>) => loadConfig({ FIREX_DATA_DIR: tmpdir(), FIREX_SITE_ID: SITE, ...extra });
  const registryFile = (doc: unknown) => {
    const path = join(mkdtempSync(join(tmpdir(), 'firex-registry-')), 'registry.json');
    writeFileSync(path, JSON.stringify(doc));
    return path;
  };
  /** A modem that says OK to everything. */
  const quietModem = (): ModemPort => {
    let listener: (text: string) => void = () => {};
    return {
      write: () => setImmediate(() => listener('\r\nOK\r\n')),
      onData: (l) => {
        listener = l;
      },
      close: async () => {},
    };
  };

  test('off unless asked for, and refused without a usable registry', () => {
    const deps = { db: openDb(':memory:'), live: new LiveBus() };
    expect(escalationFromConfig(env({}), deps)).toMatchObject({ runner: null, message: expect.stringMatching(/escalation off/) });
    expect(escalationFromConfig(env({ FIREX_ALERT_PROVIDER: 'http' }), deps).message).toMatch(/FIREX_SERVICE_REGISTRY is not set/);
    expect(escalationFromConfig(env({ FIREX_ALERT_PROVIDER: 'pager' }), deps).message).toMatch(/not off, console, http or modem/);

    const full = registryOf('live');
    const partial = registryFile({ ...full, contacts: full.contacts.slice(0, 3) });
    expect(escalationFromConfig(env({ FIREX_ALERT_PROVIDER: 'console', FIREX_SERVICE_REGISTRY: partial }), deps).message)
      .toMatch(/NOT started: service registry contacts: names no ambulance contact/);

    const elsewhere = registryFile({ ...full, site_id: 'site-999' });
    expect(escalationFromConfig(env({ FIREX_ALERT_PROVIDER: 'console', FIREX_SERVICE_REGISTRY: elsewhere }), deps).message)
      .toMatch(/registry is for site site-999/);
  });

  test('http names its target', () => {
    const deps = { db: openDb(':memory:'), live: new LiveBus() };
    const path = registryFile(registryOf('live'));
    expect(escalationFromConfig(env({ FIREX_ALERT_PROVIDER: 'http', FIREX_SERVICE_REGISTRY: path }), deps).message).toMatch(/FIREX_ALERT_HTTP_URL is not set/);
    const on = escalationFromConfig(env({ FIREX_ALERT_PROVIDER: 'http', FIREX_SERVICE_REGISTRY: path, FIREX_ALERT_HTTP_URL: 'http://127.0.0.1:9090' }), deps);
    expect(on.runner).not.toBeNull();
    expect(on.message).toMatch(/escalation ON: http to http:\/\/127\.0\.0\.1:9090 \(mock endpoints for a dry run/);
    expect(on.message).toMatch(/live services: fire, police, ambulance/);
  });

  test('the modem refuses contacts it cannot reach, and starts with phone numbers', () => {
    const deps = { db: openDb(':memory:'), live: new LiveBus(), modemPort: quietModem() };
    const ids = registryFile(registryOf('live'));
    expect(escalationFromConfig(env({ FIREX_ALERT_PROVIDER: 'modem', FIREX_SERVICE_REGISTRY: ids }), deps).message)
      .toMatch(/NOT started: the modem sends SMS and calls phone numbers; fire sms station-042, fire voice station-017, police sms ps-017, ambulance api ems-north cannot be reached/);

    const example = JSON.parse(readFileSync(fileURLToPath(new URL('../escalation/registry.example.json', import.meta.url)), 'utf8'));
    example.contacts[1] = { ...example.contacts[1], mode: 'live', agreement: AGREEMENT };
    const on = escalationFromConfig(env({ FIREX_ALERT_PROVIDER: 'modem', FIREX_SERVICE_REGISTRY: registryFile(example) }), deps);
    expect(on.runner).not.toBeNull();
    expect(on.message).toMatch(/escalation ON: GSM modem on \/dev\/ttyUSB2; Hostel, Block B, live services: fire; ack window 60 s/);
  });

  test('the registry is recorded in the site ledger when it changes, and only then', () => {
    const path = registryFile(registryOf('drill'));
    const deps = { db, live, siteLedger: site };
    const config = env({ FIREX_SERVICE_REGISTRY: path });
    const first = escalationFromConfig(config, deps);
    expect(first.registry?.site_id).toBe(SITE);
    escalationFromConfig(config, deps);
    expect(siteLog().map((e) => e.type)).toEqual(['registry_change']);

    writeFileSync(path, JSON.stringify(registryOf('live')));
    const changed = escalationFromConfig(config, deps);
    const log = siteLog();
    expect(log.map((e) => e.type)).toEqual(['registry_change', 'registry_change']);
    expect(log[0].payload).toMatchObject({ old_registry: null, new_registry: first.registrySha256, by: 'startup' });
    expect(log[1].payload).toMatchObject({ old_registry: first.registrySha256, new_registry: changed.registrySha256 });
  });

  test('the registry API masks phone numbers', async () => {
    const { app, cookie } = await signIn('viewer', { registry: registryOf('live'), registrySha256: 'ab'.repeat(32) });
    try {
      const body = (await app.inject({ method: 'GET', url: '/api/registry', headers: { cookie } })).json() as { contacts: Row[] } & Row;
      expect(body).toMatchObject({ site_id: SITE, sha256: 'ab'.repeat(32), version: 2, ack_timeout_s: 60, max_realerts: 1, offline_notify_min: 10 });
      expect(body.contacts[0]).toEqual({ service: 'site', name: 'Warden on duty', channel: 'sms', recipient_masked: '+91******0001', mode: 'live', agreement: null });
      expect(body.contacts[1]).toMatchObject({ mode: 'live', agreement: AGREEMENT, backup: { name: 'Fire station 17', channel: 'voice', recipient_masked: 'station-017' } });
    } finally {
      await app.close();
    }
  });
});
