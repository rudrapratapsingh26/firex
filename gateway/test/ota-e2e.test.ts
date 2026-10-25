/**
 * M5 end to end without hardware: a simulated unit takes the gateway's time,
 * sends an episode trace on its own topic, and is updated by a rollout. It
 * downloads the image from the firmware server with the certificate it was
 * provisioned with, reports each step in its ledger, and the rollout follows
 * those entries to done. A second rollout, whose unit fails to confirm, stops.
 */
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import type { AddressInfo } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createServer, type Server } from 'node:tls';

import { Aedes } from 'aedes';
import type { FastifyInstance } from 'fastify';
import { afterAll, beforeAll, describe as suite, expect, test } from 'vitest';

import { loadConfig } from '../server/config.ts';
import { ensureSite, openDb, type Db, type Row } from '../server/db.ts';
import { firmwarePath, createFirmwareServer, type FirmwareServer } from '../server/fw-server.ts';
import { buildProvisionApp } from '../server/http.ts';
import { Ingest } from '../server/ingest.ts';
import { LiveBus } from '../server/live.ts';
import { startBridge, type Bridge } from '../server/mqtt-bridge.ts';
import { ensureServiceCertificates, loadOrCreateCa, readServiceCredentials, type Pki } from '../server/pki.ts';
import { createPairing, resetProvisionRateLimit } from '../server/provision.ts';
import { RolloutManager } from '../server/rollout.ts';
import { SiteLedger } from '../server/site-ledger.ts';
import { certificateCommonName, mayPublish, maySubscribe } from '../sim/acl.ts';
import { SimNode } from '../sim/node-sim.ts';

const SITE = 'site-ota';
const IMAGE = readFileSync(join(import.meta.dirname, 'fixtures', 'app-signed.bin'));
const SHA = 'c912ccd58003e967fb92e21fa89c405ffc83571aa832dbc5459d5d03ea448bbe';

let dir: string;
let db: Db;
let pki: Pki;
let broker: Aedes;
let tlsServer: Server;
let bridge: Bridge;
let fwServer: FirmwareServer;
let provisionApp: FastifyInstance;
let provisionUrl: string;
let rollouts: RolloutManager;
let site: SiteLedger;

const until = async (what: string, predicate: () => boolean, timeoutMs = 10_000) => {
  const deadline = Date.now() + timeoutMs;
  while (!predicate()) {
    if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
    await new Promise((r) => setTimeout(r, 50));
  }
};

async function unit(options: ConstructorParameters<typeof SimNode>[0]): Promise<SimNode> {
  const pairing = createPairing(db, { siteId: SITE, label: `Room ${Math.random().toString(36).slice(2, 6)}`, by: 'test', ttlHours: 1 });
  const node = new SimNode({ fwVersion: '0.4.0', servername: 'localhost', ...options });
  await node.provision(provisionUrl, pairing.code);
  await node.connect();
  return node;
}

beforeAll(async () => {
  resetProvisionRateLimit();
  dir = mkdtempSync(join(tmpdir(), 'firex-ota-e2e-'));
  const config = loadConfig({ FIREX_DATA_DIR: dir, FIREX_SITE_ID: SITE, FIREX_MQTT_ADVERTISED_HOST: '127.0.0.1', FIREX_BROKER_NAMES: 'localhost,127.0.0.1' });
  db = openDb(':memory:');
  ensureSite(db, SITE, 'OTA site');
  pki = await loadOrCreateCa(config.pkiDir, 'OTA');
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
    key: readFileSync(join(pki.dir, 'broker.key')), cert: readFileSync(join(pki.dir, 'broker.crt')), ca: pki.caCertPem,
    requestCert: true, rejectUnauthorized: true,
  }, broker.handle);
  await new Promise<void>((resolve) => tlsServer.listen(0, '127.0.0.1', resolve));
  const mqttPort = (tlsServer.address() as AddressInfo).port;

  writeFileSync(firmwarePath(dir, SHA), IMAGE);
  db.prepare("INSERT INTO firmware (sha256, version, project, size, signature, uploaded_by, uploaded_at) VALUES (?, '0.5.0-m5', 'firex_node', ?, 'verified', 'admin', 0)").run(SHA, IMAGE.length);
  fwServer = createFirmwareServer({ db, pki, firmwareDir: dir });
  await new Promise<void>((resolve) => fwServer.listen(0, '127.0.0.1', resolve));

  const live = new LiveBus();
  site = new SiteLedger({ db, dir: join(dir, 'site-ledger'), siteId: SITE });
  rollouts = new RolloutManager({
    db, live, siteLedger: site, fwHost: '127.0.0.1', fwPort: (fwServer.address() as AddressInfo).port,
    publish: (t, p, o) => bridge.publish(t, p, o),
  });
  const ingest = new Ingest({
    db, mediaDir: join(dir, 'media'), tracesDir: join(dir, 'traces'), live,
    publish: (t, p, o) => bridge.publish(t, p, o), onFwUpdate: (id, payload) => rollouts.onFwUpdate(id, payload),
  });
  bridge = startBridge({ url: `mqtts://localhost:${mqttPort}`, ...readServiceCredentials(pki), onMessage: (t, p) => ingest.handle(t, p) });
  await until('the gateway to reach the broker', () => bridge.connected());

  provisionApp = await buildProvisionApp({ db, pki, live, advertisedMqttHost: '127.0.0.1', advertisedMqttPort: mqttPort });
  await provisionApp.listen({ host: '127.0.0.1', port: 0 });
  provisionUrl = `http://127.0.0.1:${(provisionApp.server.address() as AddressInfo).port}`;
}, 30_000);

afterAll(async () => {
  await bridge?.close();
  await provisionApp?.close();
  await new Promise<void>((resolve) => fwServer.close(() => resolve()));
  await new Promise<void>((resolve) => broker.close(() => resolve()));
  tlsServer?.close();
  db.close();
  rmSync(dir, { recursive: true, force: true });
});

suite('the M5 path over mutual TLS', () => {
  test('a unit takes the gateway time, and its trace reaches the gateway and is acknowledged', async () => {
    const node = await unit({});
    await until('the time command', () => node.clock === 'gateway');
    node.runTrace();
    await until('the trace to be acknowledged', () => node.pendingTraces === 0 && node.ackedSeq === node.lastSeq);
    const trace = db.prepare('SELECT seq, samples FROM traces WHERE device_id = ?').get(node.deviceId) as Row;
    expect(trace).toMatchObject({ samples: 30 });
    expect(trace.seq).not.toBeNull();
    const srcs = (db.prepare('SELECT DISTINCT ts_src FROM ledger_entries WHERE device_id = ?').all(node.deviceId) as Row[]).map((r) => r.ts_src);
    expect(srcs).toEqual(['gateway']);
    await node.disconnect();
  }, 30_000);

  test('a rollout updates a unit that downloads, verifies and confirms the image', async () => {
    const node = await unit({ ota: 'confirm', otaDownload: true });
    await until('the unit to be seen', () => (db.prepare('SELECT last_seen FROM devices WHERE id = ?').get(node.deviceId) as Row).last_seen !== null);
    const id = rollouts.start(SHA, [node.deviceId], 'admin');
    await until('the rollout to finish', () => rollouts.list()[0].state !== 'running');
    expect(rollouts.list()[0]).toMatchObject({ id, state: 'done', units: [{ state: 'confirmed' }] });
    expect(node.fwVersion).toBe('0.5.0-m5');
    const downloaded = db.prepare("SELECT actor FROM audit WHERE action = 'firmware_downloaded'").get() as Row;
    expect(downloaded.actor).toBe(node.deviceId);
    const phases = (db.prepare("SELECT payload FROM ledger_entries WHERE device_id = ? AND type = 'fw_update' ORDER BY seq").all(node.deviceId) as Row[])
      .map((r) => JSON.parse(String(r.payload)).phase);
    expect(phases).toEqual(['accepted', 'downloaded', 'confirmed']);
    await until('the new version on the gateway', () => (db.prepare('SELECT fw_version FROM devices WHERE id = ?').get(node.deviceId) as Row).fw_version === '0.5.0-m5');
    await node.disconnect();
  }, 30_000);

  test('a unit that does not confirm rolls back, and the rollout stops before the next unit', async () => {
    const failing = await unit({ ota: 'fail' });
    const waiting = await unit({});
    await until('both units to be seen', () => (db.prepare('SELECT COUNT(*) AS n FROM devices WHERE last_seen IS NOT NULL').get() as Row).n === 4);
    rollouts.start(SHA, [failing.deviceId, waiting.deviceId], 'admin');
    await until('the rollout to stop', () => rollouts.list()[0].state !== 'running');
    await until('the roll-back to be recorded', () => rollouts.list()[0].units[0].state === 'rolled_back');
    expect(rollouts.list()[0]).toMatchObject({ state: 'failed', units: [{ state: 'rolled_back' }, { state: 'skipped' }] });
    expect(waiting.fwVersion).toBe('0.4.0');
    const offered = site.bundle().log.filter((e) => e.type === 'fw_offered').map((e) => e.payload.unit);
    expect(offered).not.toContain(waiting.deviceId);
    await failing.disconnect();
    await waiting.disconnect();
  }, 30_000);
});
