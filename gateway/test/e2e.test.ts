/**
 * End to end without hardware: a simulated node provisions with a pairing code,
 * connects over mutual TLS to a broker that enforces per-device topics, and
 * mirrors an incident. The gateway verifies and stores it; the API exports a
 * bundle that the reference verifier accepts together with the photos
 * downloaded from the gateway. Then an outage, a rotation gap, and the ways
 * provisioning and the broker must say no.
 *
 * The broker is Aedes in-process, standing in for Mosquitto with the same
 * access rules as infra/mosquitto/acl.
 */
import { createHash, generateKeyPairSync } from 'node:crypto';
import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createServer, type Server } from 'node:tls';
import type { AddressInfo } from 'node:net';

import { Aedes } from 'aedes';
import type { FastifyInstance } from 'fastify';
import mqtt from 'mqtt';
import { afterAll, beforeAll, describe as suite, expect, test } from 'vitest';

import { entryFromJson, publicKeyFromPem } from '../../ledger/src/entry.ts';
import { verifyLog } from '../../ledger/src/verify.ts';
import { createUser } from '../server/auth.ts';
import { loadConfig } from '../server/config.ts';
import { ensureSite, openDb, type Db, type Row } from '../server/db.ts';
import { buildApp, buildProvisionApp } from '../server/http.ts';
import { Ingest } from '../server/ingest.ts';
import { LiveBus } from '../server/live.ts';
import { startBridge, type Bridge } from '../server/mqtt-bridge.ts';
import { ensureServiceCertificates, issueCertificate, loadOrCreateCa, readServiceCredentials, type Pki } from '../server/pki.ts';
import { resetProvisionRateLimit } from '../server/provision.ts';
import { certificateCommonName, mayPublish, maySubscribe } from '../sim/acl.ts';
import { SimNode } from '../sim/node-sim.ts';

const SITE = 'site-e2e';
let dataDir: string;
let db: Db;
let pki: Pki;
let broker: Aedes;
let tlsServer: Server;
let mqttPort: number;
let bridge: Bridge;
let app: FastifyInstance;
let provisionApp: FastifyInstance;
let provisionUrl: string;
let cookie: string;

const until = async (what: string, predicate: () => boolean, timeoutMs = 10_000) => {
  const deadline = Date.now() + timeoutMs;
  while (!predicate()) {
    if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
    await new Promise((r) => setTimeout(r, 50));
  }
};

async function api(method: 'GET' | 'POST', url: string, payload?: unknown) {
  const res = await app.inject({ method, url, payload: payload as never, headers: { cookie } });
  return { status: res.statusCode, body: res.headers['content-type']?.toString().includes('json') ? res.json() : res.rawPayload };
}

beforeAll(async () => {
  resetProvisionRateLimit();
  dataDir = mkdtempSync(join(tmpdir(), 'firex-e2e-'));
  const config = { ...loadConfig({ FIREX_DATA_DIR: dataDir, FIREX_SITE_ID: SITE, FIREX_MQTT_ADVERTISED_HOST: 'localhost', FIREX_BROKER_NAMES: 'localhost,127.0.0.1' }) };
  db = openDb(':memory:');
  ensureSite(db, SITE, 'E2E site');
  pki = await loadOrCreateCa(config.pkiDir, 'E2E');
  await ensureServiceCertificates(pki, config.brokerNames);

  broker = await Aedes.createBroker({
    authorizePublish: (client, packet, callback) => {
      callback(client && !mayPublish(certificateCommonName(client.conn), packet.topic) ? new Error('not your topic') : null);
    },
    authorizeSubscribe: (client, sub, callback) => {
      const ok = maySubscribe(certificateCommonName(client.conn), sub.topic);
      callback(ok ? null : new Error('not your topic'), ok ? sub : null);
    },
  });
  tlsServer = createServer({
    key: readFileSync(join(pki.dir, 'broker.key')),
    cert: readFileSync(join(pki.dir, 'broker.crt')),
    ca: pki.caCertPem,
    requestCert: true,
    rejectUnauthorized: true,
  }, broker.handle);
  await new Promise<void>((resolve) => tlsServer.listen(0, '127.0.0.1', resolve));
  mqttPort = (tlsServer.address() as AddressInfo).port;
  config.advertisedMqttPort = mqttPort;

  const live = new LiveBus();
  const ingest = new Ingest({ db, mediaDir: config.mediaDir, live, publish: (t, p, o) => bridge.publish(t, p, o) });
  bridge = startBridge({ url: `mqtts://localhost:${mqttPort}`, ...readServiceCredentials(pki), onMessage: (t, p) => ingest.handle(t, p) });
  await until('the gateway to reach the broker', () => bridge.connected());

  app = await buildApp({ db, config, live, mqttConnected: () => bridge.connected() });
  provisionApp = await buildProvisionApp({ db, pki, live, advertisedMqttHost: 'localhost', advertisedMqttPort: mqttPort });
  await provisionApp.listen({ host: '127.0.0.1', port: 0 });
  provisionUrl = `http://127.0.0.1:${(provisionApp.server.address() as AddressInfo).port}`;

  createUser(db, 'installer', 'installer password 1', 'operator');
  const login = await app.inject({ method: 'POST', url: '/api/login', payload: { username: 'installer', password: 'installer password 1' } });
  cookie = String(login.headers['set-cookie']).split(';')[0];
}, 30_000);

afterAll(async () => {
  await bridge?.close();
  await app?.close();
  await provisionApp?.close();
  await new Promise<void>((resolve) => broker.close(() => resolve()));
  tlsServer?.close();
  rmSync(dataDir, { recursive: true, force: true });
});

const gatewayDevice = (id: string) => db.prepare('SELECT * FROM devices WHERE id = ?').get(id) as Row;

suite('the M3 path, node to exported log', () => {
  const sim = new SimNode({ mqttUrl: undefined });
  let code: string;

  test('an installer adds the code from the label; the node provisions with it', async () => {
    const pairing = await api('POST', '/api/pairings', { code: '7k3qd-9m2xa', label: 'Room 206', floor_x: 0.4, floor_y: 0.6 });
    expect(pairing.status).toBe(200);
    code = (pairing.body as { code: string }).code.replace('-', '');
    await sim.provision(provisionUrl, code);
    const device = gatewayDevice(sim.deviceId);
    expect(device.label).toBe('Room 206');
    expect(device.chain_status).toBe('empty');
  });

  test('the same code cannot be used twice, and a wrong code is refused', async () => {
    await expect(new SimNode().provision(provisionUrl, code)).rejects.toThrow(/403/);
    await api('POST', '/api/pairings', { code: 'ABCDE-FGHJK' });
    await expect(new SimNode().provision(provisionUrl, 'ABCDEFGHJ0')).rejects.toThrow(/403/);
  });

  test('an incident is mirrored, verified, and exported with its photos', async () => {
    await sim.connect();
    sim.runIncident();
    await until('every entry and photo acknowledged', () => sim.ackedSeq === sim.lastSeq && sim.pendingMedia === 0);

    const device = gatewayDevice(sim.deviceId);
    expect(device.chain_status).toBe('ok');
    expect(device.last_seq).toBe(sim.lastSeq);
    const results = db.prepare('SELECT DISTINCT verify_result FROM ledger_entries WHERE device_id = ?').all(sim.deviceId) as Row[];
    expect(results.map((r) => r.verify_result)).toEqual(['ok']);

    const incidents = (await api('GET', '/api/incidents')).body as { end_seq: number | null; frames: { sha256: string; available: boolean }[] }[];
    expect(incidents).toHaveLength(1);
    expect(incidents[0].end_seq).not.toBeNull();
    expect(incidents[0].frames).toHaveLength(3);
    expect(incidents[0].frames.every((f) => f.available)).toBe(true);

    const bundle = (await api('GET', `/api/devices/${sim.deviceId}/bundle`)).body as { public_key_pem: string; log: never[] };
    const downloaded = new Set<string>();
    for (const frame of incidents[0].frames) {
      const image = await api('GET', `/api/media/${frame.sha256}`);
      expect(image.status).toBe(200);
      downloaded.add(createHash('sha256').update(image.body as Buffer).digest('hex'));
    }
    const outcome = verifyLog(bundle.log.map(entryFromJson), publicKeyFromPem(bundle.public_key_pem), downloaded);
    expect(outcome.ok, JSON.stringify(outcome.failure)).toBe(true);

    const accessLog = db.prepare("SELECT COUNT(*) AS n FROM audit WHERE action = 'media_accessed'").get() as Row;
    expect(accessLog.n).toBe(3);
  }, 30_000);

  test('entries written during an outage arrive after reconnecting', async () => {
    await sim.disconnect();
    for (let i = 0; i < 5; i++) sim.append('threshold_cross', { modality: 'smoke', score: 40 + i, direction: 'up' });
    expect(gatewayDevice(sim.deviceId).last_seq).toBeLessThan(sim.lastSeq);
    await sim.connect();
    await until('the backlog', () => gatewayDevice(sim.deviceId).last_seq === sim.lastSeq);
    expect(gatewayDevice(sim.deviceId).chain_status).toBe('ok');
  }, 30_000);

  test('entries rotated out before mirroring show as a gap, and the loss is itself logged', async () => {
    await sim.disconnect();
    for (let i = 0; i < 6; i++) sim.append('threshold_cross', { modality: 'heat', score: 30 + i, direction: 'up' });
    const mirrored = gatewayDevice(sim.deviceId).last_seq as number;
    sim.rotate(sim.lastSeq - sim.oldestSeq + 1); // the ring overwrites everything on flash, unmirrored entries included
    await sim.connect();
    await until('the entries after the gap', () => gatewayDevice(sim.deviceId).last_seq === sim.lastSeq);
    expect(gatewayDevice(sim.deviceId).chain_status).toBe('gap');
    const gap = db.prepare("SELECT from_seq, to_seq FROM chain_events WHERE kind = 'gap'").get() as Row;
    expect(Number(gap.from_seq)).toBe(mirrored + 1);
    const last = db.prepare('SELECT type, verify_result FROM ledger_entries WHERE device_id = ? ORDER BY seq DESC LIMIT 1').get(sim.deviceId) as Row;
    expect(last).toEqual({ type: 'log_overwrite', verify_result: 'gap' });
    await sim.disconnect();
  }, 30_000);
});

suite('the broker says no', () => {
  test('a client certificate from another CA cannot connect', async () => {
    const rogue = await loadOrCreateCa(mkdtempSync(join(tmpdir(), 'rogue-')), 'rogue');
    const keys = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
    const imported = await crypto.subtle.importKey('spki', keys.publicKey.export({ type: 'spki', format: 'der' }), { name: 'ECDSA', namedCurve: 'P-256' }, true, ['verify']);
    const cert = await issueCertificate(rogue, { commonName: 'ab'.repeat(16), publicKey: imported, usage: 'client' });
    /* Under TLS 1.3 the client's side of the handshake completes before the broker
     * checks its certificate, so refusal shows up as a closed connection, not a failed connect. */
    const client = mqtt.connect(`mqtts://localhost:${mqttPort}`, {
      ca: pki.caCertPem, cert: cert.certPem, key: keys.privateKey.export({ type: 'pkcs8', format: 'pem' }).toString(),
      reconnectPeriod: 0, connectTimeout: 3000,
    });
    const outcome = await new Promise<string>((resolve) => {
      client.on('connect', () => resolve('connected'));
      client.on('close', () => resolve('refused'));
      client.on('error', () => resolve('refused'));
    });
    client.end(true);
    expect(outcome).toBe('refused');
  });

  test('a provisioned device cannot publish as another device', async () => {
    const pairing = await api('POST', '/api/pairings', {});
    const node = new SimNode();
    await node.provision(provisionUrl, (pairing.body as { code: string }).code);
    await node.connect();
    const victim = db.prepare('SELECT id FROM devices WHERE id != ? LIMIT 1').get(node.deviceId) as Row;
    const before = gatewayDevice(String(victim.id)).last_state;
    const c = node.credentials!;
    const raw = await mqtt.connectAsync(`mqtts://localhost:${mqttPort}`, {
      clientId: `${node.deviceId}-2`, ca: c.caPem, cert: c.certPem,
      key: node.tlsKey.privateKey.export({ type: 'pkcs8', format: 'pem' }).toString(), reconnectPeriod: 0,
    });
    raw.publish(`firex/${SITE}/${victim.id}/state`, JSON.stringify({ fusion: 'CRITICAL', alarm: 'CRITICAL', faults: {} }), { qos: 1 });
    /* Mosquitto silently discards a publish its access list denies (MQTT 3.1.1 has no
     * negative acknowledgement), so the test asserts the effect: nothing arrives. */
    await new Promise((r) => setTimeout(r, 1500));
    expect(gatewayDevice(String(victim.id)).last_state).toBe(before);
    const forged = db.prepare("SELECT COUNT(*) AS n FROM audit WHERE action = 'unknown_device_message'").get() as Row;
    expect(forged.n).toBe(0);
    await node.disconnect();
    raw.end(true); /* the broker may already have dropped this client; a graceful end would wait forever */
  }, 30_000);
});
