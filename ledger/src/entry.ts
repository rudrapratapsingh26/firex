/**
 * Entry construction, hashing and signing — SPEC.md sections 1 and 2.
 *
 * Runtime dependencies: none. SHA-256 and ECDSA P-256 come from node:crypto,
 * which keeps the reference implementation something an evaluator can read
 * without auditing a dependency tree.
 */
import { createHash, createPrivateKey, createPublicKey, generateKeyPairSync, sign, verify, KeyObject } from 'node:crypto';

import { encode, fromHex, toHex, type CborValue } from './cbor.ts';

export const ZERO32 = new Uint8Array(32);

export type EventType =
  | 'boot' | 'self_test' | 'threshold_cross' | 'state_change' | 'config_change'
  | 'calibration' | 'silence' | 'heartbeat_gap' | 'clock_sync' | 'anchor_ack'
  // new in v0.2
  | 'media_captured' | 'alert_sent' | 'alert_ack' | 'alert_timeout' | 'fan_state'
  // new in v0.3
  | 'fault'
  // new in v0.4
  | 'log_overwrite'
  // new in v0.5: fw_update and trace_captured from a unit; the rest only in a site chain
  | 'fw_update' | 'trace_captured'
  | 'incident_label' | 'drill' | 'registry_change' | 'fw_offered' | 'unit_removed' | 'escalation_withheld';

export interface EntryBody {
  seq: number;
  ts: number;
  /** `gateway` (v0.5): a unit's clock set from its site gateway over the authenticated MQTT link. */
  ts_src: 'ntp' | 'rtc' | 'uncertain' | 'gateway';
  device_id: Uint8Array;
  type: EventType;
  payload: Record<string, CborValue>;
  fw_hash: Uint8Array;
  prev_hash: Uint8Array;
}

export interface Entry extends EntryBody {
  hash: Uint8Array;
  sig: Uint8Array;
}

/** The bytes that both the hash and the signature cover. */
export function preimage(body: EntryBody): Uint8Array {
  const encoded = encode({
    seq: body.seq,
    ts: body.ts,
    ts_src: body.ts_src,
    device_id: body.device_id,
    type: body.type,
    payload: body.payload,
    fw_hash: body.fw_hash,
    prev_hash: body.prev_hash,
  });
  const out = new Uint8Array(32 + encoded.length);
  out.set(body.prev_hash, 0);
  out.set(encoded, 32);
  return out;
}

export function sha256(bytes: Uint8Array): Uint8Array {
  return new Uint8Array(createHash('sha256').update(bytes).digest());
}

export function signEntry(body: EntryBody, privateKey: KeyObject): Entry {
  const pre = preimage(body);
  return {
    ...body,
    hash: sha256(pre),
    sig: new Uint8Array(sign('sha256', pre, { key: privateKey, dsaEncoding: 'ieee-p1363' })),
  };
}

export function verifySignature(entry: Entry, publicKey: KeyObject): boolean {
  return verify('sha256', preimage(entry), { key: publicKey, dsaEncoding: 'ieee-p1363' }, entry.sig);
}

/** device_id is the first 16 bytes of SHA-256 over the public key in SPKI DER form. */
export function deviceIdFor(publicKey: KeyObject): Uint8Array {
  const spki = publicKey.export({ type: 'spki', format: 'der' });
  return sha256(new Uint8Array(spki)).slice(0, 16);
}

export function generateDeviceKey(): { privateKey: KeyObject; publicKey: KeyObject } {
  // On a node this pair is generated on first boot and never exported; from M6
  // it lives in the ATECC608B. Here it is a development key.
  return generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
}

export function publicKeyToPem(publicKey: KeyObject): string {
  return publicKey.export({ type: 'spki', format: 'pem' }).toString();
}

export function publicKeyFromPem(pem: string): KeyObject {
  return createPublicKey(pem);
}

export function privateKeyFromPem(pem: string): KeyObject {
  return createPrivateKey(pem);
}

/* ---- JSON transport -------------------------------------------------- */
/* The log on the wire and on disk is JSON with hex-encoded byte strings, so an
 * inspector can open it in a text editor. The hashes are always computed over
 * the CBOR form, never over this JSON. */

export interface EntryJson {
  seq: number;
  ts: number;
  ts_src: string;
  device_id: string;
  type: string;
  payload: Record<string, unknown>;
  fw_hash: string;
  prev_hash: string;
  hash: string;
  sig: string;
}

function payloadToJson(payload: Record<string, CborValue>): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(payload)) {
    out[k] = v instanceof Uint8Array
      ? toHex(v)
      : Array.isArray(v)
        ? v.map((item) => (item instanceof Uint8Array ? toHex(item) : item))
        : v;
  }
  return out;
}

/** Byte-string payload fields, so JSON round-trips back to identical CBOR. */
const BSTR_FIELDS = new Set(['root', 'clip', 'image', 'trace', 'critical_hash', 'old_registry', 'new_registry']);
const BSTR_ARRAY_FIELDS = new Set(['frames']);

function payloadFromJson(payload: Record<string, unknown>): Record<string, CborValue> {
  const out: Record<string, CborValue> = {};
  for (const [k, v] of Object.entries(payload)) {
    if (BSTR_FIELDS.has(k) && typeof v === 'string') out[k] = fromHex(v);
    else if (BSTR_ARRAY_FIELDS.has(k) && Array.isArray(v)) out[k] = v.map((h) => fromHex(String(h)));
    else out[k] = v as CborValue;
  }
  return out;
}

export function entryToJson(e: Entry): EntryJson {
  return {
    seq: e.seq,
    ts: e.ts,
    ts_src: e.ts_src,
    device_id: toHex(e.device_id),
    type: e.type,
    payload: payloadToJson(e.payload),
    fw_hash: toHex(e.fw_hash),
    prev_hash: toHex(e.prev_hash),
    hash: toHex(e.hash),
    sig: toHex(e.sig),
  };
}

export function entryFromJson(j: EntryJson): Entry {
  return {
    seq: j.seq,
    ts: j.ts,
    ts_src: j.ts_src as EntryBody['ts_src'],
    device_id: fromHex(j.device_id),
    type: j.type as EventType,
    payload: payloadFromJson(j.payload),
    fw_hash: fromHex(j.fw_hash),
    prev_hash: fromHex(j.prev_hash),
    hash: fromHex(j.hash),
    sig: fromHex(j.sig),
  };
}

/** Appends entries to a chain, keeping seq and prev_hash consistent. */
export class LedgerWriter {
  private prev: Uint8Array = ZERO32;
  private seq = 0;
  readonly entries: Entry[] = [];
  // Written as explicit fields rather than constructor parameter properties, so
  // that `node --experimental-strip-types` can run this file directly.
  private readonly deviceId: Uint8Array;
  private readonly fwHash: Uint8Array;
  private readonly privateKey: KeyObject;

  constructor(deviceId: Uint8Array, fwHash: Uint8Array, privateKey: KeyObject) {
    this.deviceId = deviceId;
    this.fwHash = fwHash;
    this.privateKey = privateKey;
  }

  /** Continue a chain whose newest entry is already stored elsewhere (a restarted writer). */
  resume(nextSeq: number, prevHash: Uint8Array): void {
    this.seq = nextSeq;
    this.prev = prevHash;
  }

  append(type: EventType, payload: Record<string, CborValue>, ts: number, tsSrc: EntryBody['ts_src'] = 'ntp'): Entry {
    const entry = signEntry(
      {
        seq: this.seq,
        ts,
        ts_src: tsSrc,
        device_id: this.deviceId,
        type,
        payload,
        fw_hash: this.fwHash,
        prev_hash: this.prev,
      },
      this.privateKey,
    );
    this.entries.push(entry);
    this.prev = entry.hash;
    this.seq += 1;
    return entry;
  }
}
