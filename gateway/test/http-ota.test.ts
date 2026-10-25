import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import type { FastifyInstance } from 'fastify';
import { afterEach, beforeEach, describe as suite, expect, test } from 'vitest';

import { createUser } from '../server/auth.ts';
import { loadConfig } from '../server/config.ts';
import { ensureSite, openDb, type Db, type Row } from '../server/db.ts';
import { buildApp } from '../server/http.ts';
import { LiveBus } from '../server/live.ts';
import { ensureServiceCertificates, loadOrCreateCa } from '../server/pki.ts';
import { RolloutManager } from '../server/rollout.ts';
import { SiteLedger } from '../server/site-ledger.ts';

const SITE = 'site-001';
const fixture = (name: string) => readFileSync(join(import.meta.dirname, 'fixtures', name));
const KEY_A = fixture('key-a.digest.hex').toString('utf8').trim();

let dir: string;
let db: Db;
let app: FastifyInstance;
let sent: { topic: string; body: Record<string, unknown> }[];
let admin: string;
let viewer: string;

async function login(username: string): Promise<string> {
  const res = await app.inject({ method: 'POST', url: '/api/login', payload: { username, password: `${username} password 1` } });
  return String(res.headers['set-cookie']).split(';')[0];
}

beforeEach(async () => {
  dir = mkdtempSync(join(tmpdir(), 'firex-http-ota-'));
  const config = loadConfig({ FIREX_DATA_DIR: dir, FIREX_SITE_ID: SITE, FIREX_FW_SIGNING_KEY_SHA256: KEY_A, FIREX_MQTT_ADVERTISED_HOST: '192.168.1.10' });
  db = openDb(':memory:');
  ensureSite(db, SITE, 'Test site');
  const pki = await loadOrCreateCa(config.pkiDir, 'Test');
  await ensureServiceCertificates(pki, ['localhost']);
  const live = new LiveBus();
  const siteLedger = new SiteLedger({ db, dir: join(dir, 'site-ledger'), siteId: SITE });
  sent = [];
  const rollouts = new RolloutManager({
    db, live, siteLedger, fwHost: config.advertisedMqttHost, fwPort: config.fwPort,
    publish: (topic, payload) => sent.push({ topic, body: JSON.parse(payload) }),
  });
  app = await buildApp({ db, config, live, mqttConnected: () => true, siteLedger, ota: { rollouts, pki, siteLedger } });
  createUser(db, 'admin', 'admin password 1', 'admin');
  createUser(db, 'viewer', 'viewer password 1', 'viewer');
  admin = await login('admin');
  viewer = await login('viewer');
  db.prepare("INSERT INTO devices (id, site_id, label, ledger_pub, tls_pub, cert_serial, provisioned_at, last_seen, fw_version, clock, ota) VALUES (?, ?, 'Room 206', '04', '04', '0a1b', 0, ?, '0.4.0', 'gateway', ?)")
    .run('ab'.repeat(16), SITE, Date.now(), JSON.stringify({ partition: 'ota_0', pending_verify: false }));
});

afterEach(async () => {
  await app.close();
  db.close();
  rmSync(dir, { recursive: true, force: true });
});

suite('firmware, rollouts and removal over the API', () => {
  test('an admin uploads a signed image and rolls it out; the unit is offered it', async () => {
    const upload = await app.inject({ method: 'POST', url: '/api/firmware', headers: { cookie: admin, 'content-type': 'application/octet-stream' }, payload: fixture('app-signed.bin') });
    expect(upload.statusCode).toBe(200);
    const { sha256, version, signature } = upload.json();
    expect({ version, signature }).toEqual({ version: '0.5.0-m5', signature: 'verified' });

    const listed = await app.inject({ method: 'GET', url: '/api/firmware', headers: { cookie: viewer } });
    expect(listed.json()).toEqual([expect.objectContaining({ sha256, signature: 'verified', project: 'firex_node' })]);

    const rollout = await app.inject({ method: 'POST', url: `/api/firmware/${sha256}/rollout`, headers: { cookie: admin }, payload: { units: 'all' } });
    expect(rollout.statusCode).toBe(200);
    expect(sent.map((s) => s.body)).toEqual([expect.objectContaining({ cmd: 'ota', sha256, url: `https://192.168.1.10:8443/fw/${sha256}.bin` })]);
    const rollouts = await app.inject({ method: 'GET', url: '/api/rollouts', headers: { cookie: viewer } });
    expect(rollouts.json()[0]).toMatchObject({ state: 'running', version: '0.5.0-m5', units: [{ label: 'Room 206', state: 'offered' }] });
    const again = await app.inject({ method: 'POST', url: `/api/firmware/${sha256}/rollout`, headers: { cookie: admin }, payload: { units: 'all' } });
    expect(again.statusCode).toBe(409);
  });

  test('an unsigned or foreign image is refused or kept from rollout; a viewer cannot upload', async () => {
    const foreign = await app.inject({ method: 'POST', url: '/api/firmware', headers: { cookie: admin, 'content-type': 'application/octet-stream' }, payload: fixture('app-wrong-project.bin') });
    expect(foreign.statusCode).toBe(400);
    const unsigned = await app.inject({ method: 'POST', url: '/api/firmware', headers: { cookie: admin, 'content-type': 'application/octet-stream' }, payload: fixture('app-unsigned.bin') });
    expect(unsigned.json().signature).toBe('missing');
    const rollout = await app.inject({ method: 'POST', url: `/api/firmware/${unsigned.json().sha256}/rollout`, headers: { cookie: admin }, payload: { units: 'all' } });
    expect(rollout.statusCode).toBe(409);
    const byViewer = await app.inject({ method: 'POST', url: '/api/firmware', headers: { cookie: viewer, 'content-type': 'application/octet-stream' }, payload: fixture('app-signed.bin') });
    expect(byViewer.statusCode).toBe(403);
  });

  test('the device view shows the clock, the slot and removal; an admin removes a unit', async () => {
    const before = (await app.inject({ method: 'GET', url: `/api/devices/${'ab'.repeat(16)}`, headers: { cookie: viewer } })).json();
    expect(before).toMatchObject({ clock: 'gateway', removed: false, ota: { partition: 'ota_0', pending_verify: false }, fw_updates: [], traces: [] });

    const removed = await app.inject({ method: 'DELETE', url: `/api/devices/${'ab'.repeat(16)}`, headers: { cookie: admin } });
    expect(removed.json()).toEqual({ ok: true, revoked: true, restart_broker: true });
    const after = (await app.inject({ method: 'GET', url: '/api/devices', headers: { cookie: viewer } })).json();
    expect(after[0]).toMatchObject({ removed: true });
    expect((db.prepare("SELECT COUNT(*) AS n FROM site_entries WHERE type = 'unit_removed'").get() as Row).n).toBe(1);
    expect((await app.inject({ method: 'DELETE', url: `/api/devices/${'ab'.repeat(16)}`, headers: { cookie: admin } })).statusCode).toBe(409);
  });
});
