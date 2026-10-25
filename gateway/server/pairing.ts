/**
 * Pairing codes and the provisioning exchange (docs/transport.md section 1).
 *
 * Pure functions over node:crypto: the HTTP handler and the node simulator
 * both use them, and the firmware implements the same byte strings in C.
 */
import { createHash, createHmac, createPublicKey, randomBytes, timingSafeEqual, verify, type KeyObject } from 'node:crypto';

import { fromHex, toHex } from '../../ledger/src/cbor.ts';
import { deviceIdFor } from '../../ledger/src/entry.ts';

export const PROVISION_CONTEXT = 'firex-provision-v1';
const ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ'; // Crockford base32: no I, L, O, U

/** Ten characters, 50 bits, shown as XXXXX-XXXXX. */
export function generatePairingCode(): string {
  return pairingCodeFromBytes(randomBytes(10));
}

/** The low five bits of each of ten random bytes, as the firmware derives its code. */
export function pairingCodeFromBytes(bytes: Uint8Array): string {
  const chars = Array.from(bytes.subarray(0, 10), (b) => ALPHABET[b & 31]).join('');
  return `${chars.slice(0, 5)}-${chars.slice(5)}`;
}

/** Canonical form used as the HMAC key: upper case, no separators, look-alikes mapped. */
export function normalisePairingCode(input: string): string | null {
  const s = input.toUpperCase().replace(/[\s-]/g, '').replace(/O/g, '0').replace(/[IL]/g, '1');
  if (s.length !== 10) return null;
  for (const c of s) if (!ALPHABET.includes(c)) return null;
  return s;
}

export function formatPairingCode(normalised: string): string {
  return `${normalised.slice(0, 5)}-${normalised.slice(5)}`;
}

export interface ProvisionRequest {
  version: 1;
  device_id: string;
  ledger_pub: string;
  tls_pub: string;
  fw_version: string;
  nonce: string;
  ledger_sig: string;
  tls_sig: string;
  proof: string;
}

export interface ProvisionReply {
  version: 1;
  site_id: string;
  device_id: string;
  mqtt_host: string;
  mqtt_port: number;
  ca_pem: string;
  cert_pem: string;
  mac: string;
}

const HEX = (len: number) => new RegExp(`^[0-9a-f]{${len}}$`);

export function parseProvisionRequest(body: unknown): ProvisionRequest | string {
  if (!body || typeof body !== 'object') return 'body must be a JSON object';
  const b = body as Record<string, unknown>;
  const checks: [string, RegExp][] = [
    ['device_id', HEX(32)], ['ledger_pub', HEX(130)], ['tls_pub', HEX(130)], ['nonce', HEX(32)],
    ['ledger_sig', HEX(128)], ['tls_sig', HEX(128)], ['proof', HEX(64)],
  ];
  if (b.version !== 1) return 'unsupported version';
  for (const [field, re] of checks) {
    if (typeof b[field] !== 'string' || !re.test(b[field] as string)) return `${field} is missing or malformed`;
  }
  if (typeof b.fw_version !== 'string' || b.fw_version.length > 31) return 'fw_version is missing or malformed';
  if (!(b.ledger_pub as string).startsWith('04') || !(b.tls_pub as string).startsWith('04')) {
    return 'public keys must be uncompressed P-256 points';
  }
  if (b.ledger_pub === b.tls_pub) return 'the TLS key must differ from the ledger key';
  return b as unknown as ProvisionRequest;
}

export function provisionMessage(r: Pick<ProvisionRequest, 'device_id' | 'ledger_pub' | 'tls_pub' | 'nonce'>): Buffer {
  return Buffer.from(`${PROVISION_CONTEXT}\n${r.device_id}\n${r.ledger_pub}\n${r.tls_pub}\n${r.nonce}`, 'utf8');
}

export function provisionProof(code: string, r: Pick<ProvisionRequest, 'device_id' | 'ledger_pub' | 'tls_pub' | 'nonce'>): string {
  return createHmac('sha256', code).update(provisionMessage(r)).digest('hex');
}

function sha256hex(text: string): string {
  return createHash('sha256').update(text, 'utf8').digest('hex');
}

export function replyMac(code: string, nonce: string, reply: Omit<ProvisionReply, 'mac' | 'version' | 'device_id'>): string {
  const text = `${PROVISION_CONTEXT}-reply\n${nonce}\n${reply.site_id}\n${reply.mqtt_host}\n${reply.mqtt_port}\n` +
    `${sha256hex(reply.ca_pem)}\n${sha256hex(reply.cert_pem)}`;
  return createHmac('sha256', code).update(text, 'utf8').digest('hex');
}

export function hexEqual(a: string, b: string): boolean {
  const x = Buffer.from(a, 'hex');
  const y = Buffer.from(b, 'hex');
  return x.length === y.length && x.length > 0 && timingSafeEqual(x, y);
}

const SPKI_P256_PREFIX = Buffer.from('3059301306072a8648ce3d020106082a8648ce3d030107034200', 'hex');

export function p256PointToKey(pointHex: string): KeyObject {
  return createPublicKey({ key: Buffer.concat([SPKI_P256_PREFIX, Buffer.from(pointHex, 'hex')]), format: 'der', type: 'spki' });
}

/** Everything about a request that does not involve a pairing code. Null when sound. */
export function checkPossession(r: ProvisionRequest): string | null {
  let ledgerKey: KeyObject;
  let tlsKey: KeyObject;
  try {
    ledgerKey = p256PointToKey(r.ledger_pub);
    tlsKey = p256PointToKey(r.tls_pub);
  } catch {
    return 'a public key is not a valid P-256 point';
  }
  if (toHex(deviceIdFor(ledgerKey)) !== r.device_id) return 'device_id is not derived from ledger_pub';
  const message = provisionMessage(r);
  const opts = { dsaEncoding: 'ieee-p1363' as const };
  if (!verify('sha256', message, { key: ledgerKey, ...opts }, fromHex(r.ledger_sig))) return 'ledger_sig does not verify';
  if (!verify('sha256', message, { key: tlsKey, ...opts }, fromHex(r.tls_sig))) return 'tls_sig does not verify';
  return null;
}

/** The first open pairing whose code produces the request's proof. */
export function matchPairing<T extends { code: string }>(r: ProvisionRequest, open: T[]): T | null {
  for (const pairing of open) {
    if (hexEqual(provisionProof(pairing.code, r), r.proof)) return pairing;
  }
  return null;
}
