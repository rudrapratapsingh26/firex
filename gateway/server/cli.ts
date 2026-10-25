/**
 * Gateway administration from the shell (inside the container:
 * `docker compose exec firex-server npm run gateway:cli -- ...`).
 *
 *   init                                        create database and PKI
 *   user add <name> --role admin|operator|viewer   password from FIREX_NEW_PASSWORD or a prompt
 *   pairing add <code> [--label L] [--x 0.4 --y 0.6]
 *   pairing new [--label L]                      generate a code (for the node simulator)
 *   devices                                      list registered nodes and chain status
 *   export <device_id> --out <dir> [--epoch N]   write bundle.json and the images it names
 *   drill start [--services fire,police,ambulance,site] [--by name]   queue a drill for the running gateway
 *   site-ledger export --out <file>              the gateway's signed chain, for the verifier
 *   registry check <file>                        load a service registry the way the gateway will
 *   firmware add <image.bin> | firmware list         check and keep a node image
 *   rollout <sha256> [--units id,id|all]           queue a rollout for the running gateway
 *   device remove <device_id>                    remove a unit and revoke its certificate
 *   traces export <device_id> --out <dir>        episode traces as replayable captures
 *   pilot export --from <date> --to <date> --out <dir>
 */
import { mkdirSync, copyFileSync, existsSync, writeFileSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { createInterface } from 'node:readline/promises';

import type { Service } from '../escalation/provider.ts';
import { loadRegistry, maskRecipient } from '../escalation/registry.ts';
import { createUser, ROLES, type Role } from './auth.ts';
import { buildBundle } from './bundle.ts';
import { loadConfig } from './config.ts';
import { ensureSite, openDb, type Row } from './db.ts';
import { mayRollOut } from './firmware.ts';
import { storeFirmware } from './http-ota.ts';
import { mediaPath } from './ingest.ts';
import { queueDrill } from './escalate.ts';
import { LiveBus } from './live.ts';
import { ensureServiceCertificates, loadOrCreateCa } from './pki.ts';
import { createPairing } from './provision.ts';
import { removeUnit } from './removal.ts';
import { RolloutManager } from './rollout.ts';
import { traceToCapture } from './traces.ts';
import { exportPilot } from './pilot.ts';
import { SiteLedger } from './site-ledger.ts';

function flag(args: string[], name: string): string | undefined {
  const i = args.indexOf(`--${name}`);
  return i >= 0 ? args[i + 1] : undefined;
}

async function main(): Promise<number> {
  const [command, sub, ...rest] = process.argv.slice(2);
  const config = loadConfig();
  mkdirSync(config.dataDir, { recursive: true });
  const db = openDb(config.dbPath);
  ensureSite(db, config.siteId, config.siteName);

  if (command === 'init') {
    const pki = await loadOrCreateCa(config.pkiDir, config.siteName);
    await ensureServiceCertificates(pki, config.brokerNames);
    console.log(`initialised ${config.dataDir}; broker names: ${config.brokerNames.join(', ')}`);
    return 0;
  }

  if (command === 'user' && sub === 'add') {
    const [name] = rest;
    const role = (flag(rest, 'role') ?? 'viewer') as Role;
    if (!name || !ROLES.includes(role)) {
      console.error('usage: user add <name> --role admin|operator|viewer');
      return 2;
    }
    let password = process.env.FIREX_NEW_PASSWORD;
    if (!password) {
      const rl = createInterface({ input: process.stdin, output: process.stdout });
      password = await rl.question(`password for ${name} (12+ characters): `);
      rl.close();
    }
    createUser(db, name, password, role);
    console.log(`created ${role} ${name}`);
    return 0;
  }

  if (command === 'pairing' && (sub === 'add' || sub === 'new')) {
    const code = sub === 'add' ? rest[0] : undefined;
    if (sub === 'add' && !code) {
      console.error('usage: pairing add <code> [--label L] [--x 0-1 --y 0-1]');
      return 2;
    }
    const x = flag(rest, 'x');
    const y = flag(rest, 'y');
    const pairing = createPairing(db, {
      code, siteId: config.siteId, label: flag(rest, 'label') ?? '', floorX: x ? Number(x) : null, floorY: y ? Number(y) : null,
      by: 'cli', ttlHours: config.pairingTtlHours,
    });
    console.log(`${pairing.code}  site ${pairing.site_id}  expires ${new Date(pairing.expires_at).toISOString()}`);
    return 0;
  }

  if (command === 'devices') {
    const rows = db.prepare('SELECT id, label, fw_version, chain_status, epoch, last_seq, last_seen FROM devices ORDER BY label').all() as Row[];
    for (const r of rows) {
      const seen = r.last_seen ? new Date(Number(r.last_seen)).toISOString() : 'never';
      console.log(`${r.id}  ${String(r.label || '-').padEnd(20)}  fw ${r.fw_version ?? '?'}  chain ${r.chain_status} (epoch ${r.epoch}, seq ${r.last_seq ?? '-'})  seen ${seen}`);
    }
    return 0;
  }

  if (command === 'export') {
    const deviceId = sub;
    const out = flag(rest, 'out');
    const epoch = flag(rest, 'epoch');
    if (!deviceId || !out) {
      console.error('usage: export <device_id> --out <dir> [--epoch N]');
      return 2;
    }
    const bundle = buildBundle(db, config.mediaDir, deviceId, { epoch: epoch ? Number(epoch) : undefined });
    if (!bundle) {
      console.error(`no device ${deviceId}`);
      return 1;
    }
    const mediaOut = join(out, 'media');
    mkdirSync(mediaOut, { recursive: true });
    for (const sha of bundle.media) {
      const src = mediaPath(config.mediaDir, deviceId, sha);
      if (existsSync(src)) copyFileSync(src, join(mediaOut, `${sha}.jpg`));
    }
    writeFileSync(join(out, 'bundle.json'), JSON.stringify(bundle, null, 2) + '\n');
    console.log(`${bundle.log.length} entries, ${bundle.media.length} images -> ${out}`);
    console.log(`verify: npm run verify -- ${join(out, 'bundle.json')} --media ${mediaOut}`);
    return 0;
  }

  if (command === 'drill' && sub === 'start') {
    const services = (flag(rest, 'services') ?? 'fire,police,ambulance,site').split(',').map((s) => s.trim()).filter(Boolean) as Service[];
    const id = queueDrill(db, services, flag(rest, 'by') ?? 'cli');
    console.log(`drill ${id} queued for ${services.join(', ')}; the running gateway starts it within a few seconds if its alert provider is on`);
    return 0;
  }

  if (command === 'site-ledger' && sub === 'export') {
    const out = flag(rest, 'out');
    if (!out) {
      console.error('usage: site-ledger export --out <file.json>');
      return 2;
    }
    const bundle = new SiteLedger({ db, dir: join(config.dataDir, 'site-ledger'), siteId: config.siteId }).bundle();
    writeFileSync(out, JSON.stringify(bundle, null, 2) + '\n');
    console.log(`${bundle.log.length} site ledger entries -> ${out}`);
    console.log(`verify: npm run verify -- ${out}`);
    return 0;
  }

  if (command === 'registry' && sub === 'check') {
    const [file] = rest;
    if (!file) {
      console.error('usage: registry check <registry.json>');
      return 2;
    }
    try {
      const registry = loadRegistry(file);
      console.log(`${registry.site_name} (${registry.site_id}), registry version ${registry.version}`);
      for (const c of registry.contacts) {
        const agreement = c.agreement ? `, agreement ${c.agreement.reference} (${c.agreement.date})` : '';
        const backup = c.backup ? `; backup ${c.backup.name} via ${c.backup.channel} ${maskRecipient(c.backup.recipient)}` : '';
        console.log(`  ${c.service.padEnd(9)} ${c.mode === 'live' ? 'LIVE ' : 'drill'}  ${c.name} via ${c.channel} ${maskRecipient(c.recipient)}${agreement}${backup}`);
      }
      console.log(`  ack window ${registry.ack_timeout_s ?? 60} s, ${registry.max_realerts ?? 1} re-alert(s), offline notice after ${registry.offline_notify_min ?? 10} min`);
      return 0;
    } catch (err) {
      console.error(String(err instanceof Error ? err.message : err));
      return 1;
    }
  }

  if (command === 'firmware' && sub === 'add') {
    const [file] = rest;
    if (!file) {
      console.error('usage: firmware add <image.bin>');
      return 2;
    }
    try {
      const image = storeFirmware(db, config, readFileSync(file), flag(rest, 'by') ?? 'cli', Date.now());
      console.log(`${image.sha256}  ${image.version}  ${image.size} bytes  signature ${image.signature}`);
      if (!mayRollOut(image.signature)) console.log('a unit would refuse this image: it cannot be rolled out');
      return 0;
    } catch (err) {
      console.error(String(err instanceof Error ? err.message : err));
      return 1;
    }
  }

  if (command === 'firmware' && sub === 'list') {
    for (const r of db.prepare('SELECT * FROM firmware ORDER BY uploaded_at DESC').all() as Row[]) {
      console.log(`${r.sha256}  ${String(r.version).padEnd(16)}  ${r.size} bytes  signature ${r.signature}  by ${r.uploaded_by}`);
    }
    return 0;
  }

  if (command === 'rollout') {
    const sha = sub;
    const unitsArg = flag(rest, 'units') ?? 'all';
    if (!sha) {
      console.error('usage: rollout <sha256> [--units id,id|all]');
      return 2;
    }
    const manager = new RolloutManager({ db, live: new LiveBus(), fwHost: config.advertisedMqttHost, fwPort: config.fwPort, publish: () => {} });
    try {
      const id = manager.start(sha, unitsArg === 'all' ? 'all' : unitsArg.split(','), flag(rest, 'by') ?? 'cli', false);
      console.log(`rollout ${id} queued; the running gateway offers the image to the first unit within a few seconds`);
      return 0;
    } catch (err) {
      console.error(String(err instanceof Error ? err.message : err));
      return 1;
    }
  }

  if (command === 'device' && sub === 'remove') {
    const [deviceId] = rest;
    if (!deviceId) {
      console.error('usage: device remove <device_id>');
      return 2;
    }
    const pki = await loadOrCreateCa(config.pkiDir, config.siteName);
    const siteLedger = new SiteLedger({ db, dir: join(config.dataDir, 'site-ledger'), siteId: config.siteId });
    const result = await removeUnit(deviceId, { db, pki, siteLedger, by: flag(rest, 'by') ?? 'cli' });
    if (!result.ok) {
      console.error(result.error);
      return 1;
    }
    console.log(`removed ${deviceId}${result.certSerial ? `; certificate ${result.certSerial} revoked` : ''}`);
    console.log('restart the broker so it reads the new CRL: docker compose restart mosquitto');
    return 0;
  }

  if (command === 'traces' && sub === 'export') {
    const [deviceId] = rest;
    const out = flag(rest, 'out');
    if (!deviceId || !out) {
      console.error('usage: traces export <device_id> --out <dir>');
      return 2;
    }
    mkdirSync(out, { recursive: true });
    const rows = db.prepare('SELECT sha256 FROM traces WHERE device_id = ? ORDER BY received_at').all(deviceId) as Row[];
    for (const r of rows) {
      const capture = traceToCapture(db, config.tracesDir, String(r.sha256));
      writeFileSync(join(out, `${r.sha256}.csv`), capture.csv);
      writeFileSync(join(out, `${r.sha256}.events.jsonl`), capture.events);
      console.log(`${r.sha256}  ${capture.matched}/${capture.transitions} state changes matched${capture.trial ? `, trial ${capture.trial.cls} ${capture.trial.name}` : ''}`);
    }
    console.log(`${rows.length} trace(s) -> ${out}; replay with python tools/validation/replay.py ${join(out, '*.csv')}`);
    return 0;
  }

  if (command === 'pilot' && sub === 'export') {
    const from = Date.parse(flag(rest, 'from') ?? '');
    const to = Date.parse(flag(rest, 'to') ?? '');
    const out = flag(rest, 'out');
    if (!Number.isFinite(from) || !Number.isFinite(to) || !out) {
      console.error('usage: pilot export --from <ISO date> --to <ISO date> --out <dir>');
      return 2;
    }
    const siteLedger = new SiteLedger({ db, dir: join(config.dataDir, 'site-ledger'), siteId: config.siteId });
    const doc = exportPilot({
      db, siteId: config.siteId, siteLedger, mediaDir: config.mediaDir, tracesDir: config.tracesDir,
      registryPath: config.serviceRegistryPath, fromMs: from, toMs: to, out,
    }) as { units: unknown[]; incidents: unknown[]; drills: unknown[]; traces: unknown[]; site_ledger: { verify: string } };
    console.log(`${doc.units.length} unit(s), ${doc.incidents.length} incident(s), ${doc.drills.length} drill(s), ${doc.traces.length} trace(s) -> ${out}`);
    console.log(`site ledger: ${doc.site_ledger.verify}`);
    return 0;
  }

  console.error('commands: init | user add | pairing add | pairing new | devices | export | drill start | site-ledger export | registry check | firmware add | firmware list | rollout | device remove | traces export | pilot export');
  return 2;
}

main().then((code) => process.exit(code), (err) => {
  console.error(String(err instanceof Error ? err.message : err));
  process.exit(1);
});
