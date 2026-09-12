/**
 * Test vectors: one valid chain and nine broken ones. A third-party verifier
 * that agrees with all ten is interoperable with SPEC.md by definition.
 *
 *   npm run vectors     writes ledger/vectors/*.json
 */
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { fromHex, toHex } from './cbor.ts';
import {
  deviceIdFor, entryToJson, generateDeviceKey, LedgerWriter, publicKeyToPem, sha256,
  signEntry, type Entry, type EntryJson,
} from './entry.ts';
import type { Tier } from './verify.ts';

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, '..', 'vectors');

export interface Vector {
  name: string;
  description: string;
  expect: { ok: boolean; tier?: Tier; seq?: number };
  public_key_pem: string;
  media?: string[];
  log: EntryJson[];
}

const T0 = 1789000000; // a fixed timestamp keeps the vectors reproducible

/** The incident every vector is built from: a gas leak that becomes a fire. */
function buildValidChain() {
  const { privateKey, publicKey } = generateDeviceKey();
  const deviceId = deviceIdFor(publicKey);
  const fwHash = sha256(new TextEncoder().encode('firex-firmware-0.1.0-m0'));
  const w = new LedgerWriter(deviceId, fwHash, privateKey);

  const frameA = sha256(new TextEncoder().encode('frame-1-jpeg-bytes'));
  const frameB = sha256(new TextEncoder().encode('frame-2-jpeg-bytes'));
  const clip = sha256(new TextEncoder().encode('clip-mjpeg-bytes'));

  w.append('boot', { fw_version: '0.1.0-m0', reset_reason: 'power_on', boot_count: 12 }, T0);
  w.append('self_test', {
    result: 'pass',
    checks: { heater: true, buzzer: true, flame_loopback: true, fan: true, camera: true },
  }, T0 + 60);
  w.append('fan_state', { state: 'on', duty: 60, current_ma: 95 }, T0 + 61);
  w.append('threshold_cross', { modality: 'gas', score: 68, direction: 'up' }, T0 + 900);
  w.append('state_change', {
    from: 'NORMAL', to: 'ELEVATED', reason: ['gas'], scores: { gas: 68, smoke: 21, flame: 0, heat: 12, humidity: 44 },
  }, T0 + 905);
  w.append('threshold_cross', { modality: 'heat', score: 64, direction: 'up' }, T0 + 940);
  w.append('state_change', {
    from: 'ELEVATED', to: 'CRITICAL', reason: ['gas', 'heat'], scores: { gas: 81, smoke: 35, flame: 0, heat: 64, humidity: 41 },
  }, T0 + 950);
  w.append('media_captured', {
    trigger: 'CRITICAL', frames: [frameA, frameB], clip, codec: 'mjpeg', resolution: '640x480',
  }, T0 + 951);
  w.append('alert_sent', { service: 'fire', channel: 'sms', recipient: 'station-042', receipt_id: 'r-1001' }, T0 + 952);
  w.append('alert_sent', { service: 'police', channel: 'sms', recipient: 'ps-017', receipt_id: 'r-1002' }, T0 + 952);
  w.append('alert_sent', { service: 'ambulance', channel: 'api', recipient: 'ems-north', receipt_id: 'r-1003' }, T0 + 952);
  w.append('alert_ack', { service: 'fire', receipt_id: 'r-1001', by: 'station-042', latency_s: 34 }, T0 + 986);
  w.append('alert_timeout', { service: 'police', receipt_id: 'r-1002', elapsed_s: 60, action: 're_alert' }, T0 + 1012);

  return { entries: w.entries, privateKey, publicKey, deviceId, fwHash, media: [frameA, frameB, clip] };
}

function clone(entries: Entry[]): Entry[] {
  return entries.map((e) => ({
    ...e,
    device_id: Uint8Array.from(e.device_id),
    fw_hash: Uint8Array.from(e.fw_hash),
    prev_hash: Uint8Array.from(e.prev_hash),
    hash: Uint8Array.from(e.hash),
    sig: Uint8Array.from(e.sig),
    payload: { ...e.payload },
  }));
}

export function buildVectors(): Vector[] {
  const base = buildValidChain();
  const pem = publicKeyToPem(base.publicKey);
  const mediaHex = base.media.map(toHex);
  const vectors: Vector[] = [];

  const add = (name: string, description: string, expect: Vector['expect'], entries: Entry[], media = mediaHex) => {
    vectors.push({ name, description, expect, public_key_pem: pem, media, log: entries.map(entryToJson) });
  };

  add('01-valid-chain', 'A complete incident: boot, self-test, gas elevated, heat agrees, CRITICAL, photos, three alerts, one ack, one timeout.',
    { ok: true }, base.entries);

  // 02: payload edited after the fact — the classic facility-manager tamper.
  const tamperedPayload = clone(base.entries);
  tamperedPayload[3].payload = { ...tamperedPayload[3].payload, score: 12 };
  add('02-tampered-payload', 'Entry 3 has its gas score rewritten from 68 to 12; the hash no longer matches the contents.',
    { ok: false, tier: 'chain', seq: 3 }, tamperedPayload);

  // 03: the link itself is cut.
  const brokenLink = clone(base.entries);
  brokenLink[4].prev_hash = sha256(new TextEncoder().encode('not the previous hash'));
  add('03-broken-prev-link', 'Entry 4 points at a prev_hash that is not the hash of entry 3.',
    { ok: false, tier: 'chain', seq: 4 }, brokenLink);

  // 04: signature replaced.
  const badSig = clone(base.entries);
  badSig[2].sig = Uint8Array.from(badSig[2].sig.map((b, i) => (i === 0 ? b ^ 0xff : b)));
  add('04-bad-signature', 'Entry 2 keeps a valid hash chain but its signature has one flipped byte.',
    { ok: false, tier: 'signature', seq: 2 }, badSig);

  // 05: an entry is removed, leaving a gap in seq.
  const gap = clone(base.entries);
  gap.splice(4, 1);
  add('05-sequence-gap', 'Entry 4 is deleted; the sequence jumps from 3 to 5.',
    { ok: false, tier: 'sequence', seq: 5 }, gap);

  // 06: two entries swapped.
  const reordered = clone(base.entries);
  [reordered[2], reordered[3]] = [reordered[3], reordered[2]];
  add('06-reordered-entries', 'Entries 2 and 3 are swapped, so the sequence goes 0, 1, 3, 2.',
    { ok: false, tier: 'sequence', seq: 3 }, reordered);

  // 07: the middle of the log is cut out.
  const truncated = clone(base.entries);
  truncated.splice(3, 4);
  add('07-truncated-middle', 'Four entries covering the alarm are cut out of the middle of the log.',
    { ok: false, tier: 'sequence', seq: 7 }, truncated);

  // 08: an entry from another device is spliced in (device_id changed).
  const wrongDevice = clone(base.entries);
  const other = generateDeviceKey();
  const otherId = deviceIdFor(other.publicKey);
  wrongDevice[2] = signEntry({ ...wrongDevice[2], device_id: otherId }, other.privateKey);
  // keep the chain consistent so the failure is attributed to the signature
  for (let i = 3; i < wrongDevice.length; i++) {
    wrongDevice[i] = signEntry({ ...wrongDevice[i], prev_hash: wrongDevice[i - 1].hash }, base.privateKey);
  }
  add('08-wrong-device-id', 'Entry 2 was written by a different device and re-signed with that device key.',
    { ok: false, tier: 'signature', seq: 2 }, wrongDevice);

  // 09: the log is intact but an exported photo does not match its hash.
  add('09-media-hash-mismatch', 'The chain verifies, but the exported images do not include the frames the log names.',
    { ok: false, tier: 'media', seq: 7 }, base.entries, [toHex(sha256(new TextEncoder().encode('a different photo')))]);

  // 10: a genesis entry that claims seq 5.
  const badGenesis = clone(base.entries);
  badGenesis[0] = signEntry({ ...badGenesis[0], seq: 5 }, base.privateKey);
  add('10-genesis-prev-not-zero', 'The first entry has a prev_hash of zero but claims seq 5.',
    { ok: false, tier: 'sequence', seq: 5 }, [badGenesis[0]]);

  return vectors;
}

function main(): void {
  mkdirSync(OUT, { recursive: true });
  const vectors = buildVectors();
  for (const v of vectors) {
    writeFileSync(join(OUT, `${v.name}.json`), JSON.stringify(v, null, 2) + '\n', 'utf8');
  }
  writeFileSync(
    join(OUT, 'index.json'),
    JSON.stringify(vectors.map((v) => ({ name: v.name, description: v.description, expect: v.expect })), null, 2) + '\n',
    'utf8',
  );
  console.log(`wrote ${vectors.length} vectors to ledger/vectors/`);
}

if (process.argv[1] && process.argv[1].includes('vectors')) main();

export { fromHex };
