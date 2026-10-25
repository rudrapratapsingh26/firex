/**
 * Key helpers shared by the node simulator and the tests: the same byte
 * formats the firmware uses (uncompressed P-256 points, raw r || s signatures).
 */
import { generateKeyPairSync, randomBytes, sign, type KeyObject } from 'node:crypto';

import { toHex } from '../../ledger/src/cbor.ts';
import { deviceIdFor } from '../../ledger/src/entry.ts';
import { provisionMessage, provisionProof, type ProvisionRequest } from '../server/pairing.ts';

export interface KeyPair {
  privateKey: KeyObject;
  publicKey: KeyObject;
}

export function newP256(): KeyPair {
  return generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
}

/** 04 || X || Y as lowercase hex. */
export function pointHex(publicKey: KeyObject): string {
  const jwk = publicKey.export({ format: 'jwk' });
  return '04' + Buffer.from(String(jwk.x), 'base64url').toString('hex') + Buffer.from(String(jwk.y), 'base64url').toString('hex');
}

export function signRaw(privateKey: KeyObject, message: Buffer): string {
  return sign('sha256', message, { key: privateKey, dsaEncoding: 'ieee-p1363' }).toString('hex');
}

export function buildProvisionRequest(code: string, ledger: KeyPair, tls: KeyPair, fwVersion = '0.4.0-sim'): ProvisionRequest {
  const base = {
    device_id: toHex(deviceIdFor(ledger.publicKey)),
    ledger_pub: pointHex(ledger.publicKey),
    tls_pub: pointHex(tls.publicKey),
    nonce: randomBytes(16).toString('hex'),
  };
  const message = provisionMessage(base);
  return {
    version: 1,
    ...base,
    fw_version: fwVersion,
    ledger_sig: signRaw(ledger.privateKey, message),
    tls_sig: signRaw(tls.privateKey, message),
    proof: provisionProof(code, base),
  };
}
