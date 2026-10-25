/**
 * firex-server: the site gateway process.
 *
 *   npm run gateway                       run everything
 *   npm run gateway -- --init-only        create the database and PKI, then exit
 *
 * On first start it creates the site CA, the broker's certificate and its own
 * client certificate in <data>/pki, which the Mosquitto container mounts.
 */
import { mkdirSync } from 'node:fs';
import { join } from 'node:path';

import { userCount } from './auth.ts';
import { loadConfig } from './config.ts';
import { ensureSite, openDb } from './db.ts';
import { escalationFromConfig } from './escalate.ts';
import { createFirmwareServer } from './fw-server.ts';
import { HeartbeatMonitor } from './heartbeat.ts';
import { buildApp, buildProvisionApp } from './http.ts';
import { Ingest } from './ingest.ts';
import { LiveBus } from './live.ts';
import { startBridge, type Bridge } from './mqtt-bridge.ts';
import { ensureServiceCertificates, loadOrCreateCa, readServiceCredentials } from './pki.ts';
import { startRetention } from './retention.ts';
import { RolloutManager } from './rollout.ts';
import { SiteLedger } from './site-ledger.ts';

async function main(): Promise<void> {
  const config = loadConfig();
  mkdirSync(config.mediaDir, { recursive: true });
  const db = openDb(config.dbPath);
  ensureSite(db, config.siteId, config.siteName);
  const pki = await loadOrCreateCa(config.pkiDir, config.siteName);
  await ensureServiceCertificates(pki, config.brokerNames);
  console.log(`firex-server: data in ${config.dataDir}, site ${config.siteId}`);

  if (process.argv.includes('--init-only')) {
    console.log('firex-server: initialised; exiting (--init-only)');
    return;
  }

  const live = new LiveBus();
  const log = (line: string) => console.log(`firex-server: ${line}`);
  const siteLedger = new SiteLedger({ db, dir: join(config.dataDir, 'site-ledger'), siteId: config.siteId });
  const escalation = escalationFromConfig(config, { db, live, siteLedger, log });
  log(escalation.message);
  escalation.runner?.start();
  const heartbeat = new HeartbeatMonitor({
    db,
    live,
    siteLedger,
    log,
    notify: escalation.runner ? (text) => escalation.runner!.notifySite(text) : undefined,
    notifyAfterMin: escalation.registry?.offline_notify_min,
  });
  heartbeat.start();
  let bridge: Bridge | null = null;
  const rollouts = new RolloutManager({
    db, live, siteLedger, log, fwHost: config.advertisedMqttHost, fwPort: config.fwPort,
    publish: (topic, payload, options) => bridge?.publish(topic, payload, options),
  });
  const ingest = new Ingest({
    db,
    mediaDir: config.mediaDir,
    tracesDir: config.tracesDir,
    onFwUpdate: (deviceId, payload) => rollouts.onFwUpdate(deviceId, payload),
    live,
    publish: (topic, payload, options) => bridge?.publish(topic, payload, options),
  });
  bridge = startBridge({
    url: config.mqttUrl,
    ...readServiceCredentials(pki),
    onMessage: (topic, payload) => ingest.handle(topic, payload),
    onStatus: (connected) => console.log(`firex-server: broker ${connected ? 'connected' : 'disconnected'}`),
  });

  const app = await buildApp({
    db,
    config,
    live,
    mqttConnected: () => bridge?.connected() ?? false, ota: { rollouts, pki, siteLedger, reloadCrl: () => fwServer.reloadCrl() },
    escalation: escalation.runner,
    registry: escalation.registry,
    registrySha256: escalation.registrySha256,
    siteLedger,
  }, { serveDashboard: true });
  await app.listen({ host: config.httpHost, port: config.httpPort });
  const provisionApp = await buildProvisionApp({
    db, pki, live, advertisedMqttHost: config.advertisedMqttHost, advertisedMqttPort: config.advertisedMqttPort,
  });
  await provisionApp.listen({ host: config.httpHost, port: config.provisionPort });
  mkdirSync(config.firmwareDir, { recursive: true });
  const fwServer = createFirmwareServer({ db, pki, firmwareDir: config.firmwareDir });
  fwServer.listen(config.fwPort, config.httpHost);
  log(`firmware downloads on :${config.fwPort}${config.fwSigningKeySha256 ? '' : ' (FIREX_FW_SIGNING_KEY_SHA256 not set: signatures are checked, the signing key is not)'}`);
  /* A unit's clock drifts; the gateway's time goes out hourly as well as after every sync. */
  const timeTimer = setInterval(() => ingest.sendTimeToConnected(), 3_600_000);
  const rolloutTimer = setInterval(() => rollouts.tick(), 5000);
  rollouts.tick();
  const stopRetention = startRetention(db, config.mediaDir, { mediaDays: config.mediaRetentionDays, telemetryDays: config.telemetryRetentionDays });
  console.log(`firex-server: dashboard and API on :${config.httpPort}, provisioning on :${config.provisionPort}`);
  if (userCount(db) === 0) {
    console.log('firex-server: no accounts yet. Create the first admin with: npm run gateway:cli -- user add <name> --role admin');
  }

  const shutdown = async () => {
    stopRetention();
    clearInterval(timeTimer);
    clearInterval(rolloutTimer);
    fwServer.close();
    heartbeat.stop();
    escalation.runner?.stop();
    await Promise.allSettled([app.close(), provisionApp.close(), bridge?.close()]);
    db.close();
    process.exit(0);
  };
  process.on('SIGTERM', shutdown);
  process.on('SIGINT', shutdown);
}

main().catch((err) => {
  console.error('firex-server failed to start:', err);
  process.exit(1);
});
