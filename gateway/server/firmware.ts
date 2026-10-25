/**
 * Node firmware images, checked before the gateway will offer one to a unit.
 *
 * An image is an Espressif application image: the image header (magic 0xE9),
 * the first segment header, then esp_app_desc_t at offset 0x20 with its magic
 * word, the version and the project name. A signed image (secure boot v2, RSA
 * scheme) has its contents padded to a 4096-byte boundary and one 4096-byte
 * signature sector after them, holding up to three 1216-byte blocks:
 *
 *   u8 0xE7 | u8 0x02 | 2 reserved | image digest[32] |
 *   n[384] e u32 rinv[384] m' u32  (the public key, little-endian bignums) |
 *   RSA-PSS signature[384] (little-endian) | CRC-32 of the 1196 bytes before it | 16 padding
 *
 * The digest is SHA-256 over the padded contents. The signature is RSA-PSS
 * with SHA-256, MGF1-SHA-256 and a 32-byte salt over that digest. The key
 * digest a unit trusts is SHA-256 over the 776 key bytes, the same value
 * `espsecure.py digest_sbv2_public_key` prints.
 *
 * The unit checks the signature itself before it boots an update; this check
 * exists so an image the unit would refuse is never offered.
 *
 *   verified    a block verifies, and its key digest is the one configured (FIREX_FW_SIGNING_KEY_SHA256)
 *   present     a block verifies, but no key digest is configured to compare it with
 *   wrong_key   a block verifies, but under a key other than the configured one
 *   missing     no signature sector
 * An image whose signature sector is damaged, or does not verify, is refused.
 */
import { createHash } from 'node:crypto';

/** The node's ESP-IDF project name (firmware/CMakeLists.txt). */
export const NODE_PROJECT = 'firex_node';

export type SignatureStatus = 'verified' | 'present' | 'missing' | 'wrong_key';

export interface FirmwareImage {
  sha256: string;
  size: number;
  version: string;
  project: string;
  idfVersion: string;
  signature: SignatureStatus;
  /** Key digests of the blocks that verified. */
  keyDigests: string[];
}

export class FirmwareError extends Error {}

const SECTOR = 4096;
const BLOCK = 1216;
const SIGNED_LEN = 1196;
const APP_DESC_OFFSET = 0x20;
const APP_DESC_MAGIC = 0xabcd5432;

function cString(bytes: Buffer, offset: number, length: number): string {
  const slice = bytes.subarray(offset, offset + length);
  const end = slice.indexOf(0);
  return slice.subarray(0, end < 0 ? length : end).toString('latin1');
}

let crcTable: Uint32Array | null = null;

/** zlib's CRC-32, as the signature block uses it. Exported for the tests. */
export function crc32(bytes: Uint8Array): number {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  let crc = 0xffffffff;
  for (const b of bytes) crc = crcTable[(crc ^ b) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

function bigFromLittle(bytes: Uint8Array): bigint {
  let v = 0n;
  for (let i = bytes.length - 1; i >= 0; i--) v = (v << 8n) | BigInt(bytes[i]);
  return v;
}

function bigToBig(v: bigint, length: number): Buffer {
  const out = Buffer.alloc(length);
  for (let i = length - 1; i >= 0; i--) {
    out[i] = Number(v & 0xffn);
    v >>= 8n;
  }
  return out;
}

function modPow(base: bigint, exp: bigint, mod: bigint): bigint {
  let result = 1n;
  base %= mod;
  while (exp > 0n) {
    if (exp & 1n) result = (result * base) % mod;
    base = (base * base) % mod;
    exp >>= 1n;
  }
  return result;
}

function sha256(...parts: Uint8Array[]): Buffer {
  const h = createHash('sha256');
  for (const p of parts) h.update(p);
  return h.digest();
}

function mgf1(seed: Uint8Array, length: number): Buffer {
  const out: Buffer[] = [];
  for (let counter = 0; out.length * 32 < length; counter++) {
    const c = Buffer.alloc(4);
    c.writeUInt32BE(counter);
    out.push(sha256(seed, c));
  }
  return Buffer.concat(out).subarray(0, length);
}

/** EMSA-PSS-VERIFY (RFC 8017 9.1.2) with SHA-256 and a 32-byte salt, over an already hashed message. */
function pssVerify(digest: Uint8Array, signature: bigint, n: bigint, e: bigint): boolean {
  const modBits = n.toString(2).length;
  const emBits = modBits - 1;
  const emLen = Math.ceil(emBits / 8);
  if (signature >= n) return false;
  const em = bigToBig(modPow(signature, e, n), emLen);
  if (em[emLen - 1] !== 0xbc) return false;
  const dbLen = emLen - 32 - 1;
  const masked = em.subarray(0, dbLen);
  const h = em.subarray(dbLen, dbLen + 32);
  const unusedBits = 8 * emLen - emBits;
  if (unusedBits && masked[0] & (0xff << (8 - unusedBits))) return false;
  const db = Buffer.from(masked.map((b, i) => b ^ mgf1(h, dbLen)[i]));
  if (unusedBits) db[0] &= 0xff >> unusedBits;
  const saltStart = dbLen - 32;
  for (let i = 0; i < saltStart - 1; i++) if (db[i] !== 0) return false;
  if (db[saltStart - 1] !== 0x01) return false;
  const salt = db.subarray(saltStart);
  return sha256(Buffer.alloc(8), digest, salt).equals(h);
}

interface BlockResult {
  keyDigest: string;
}

/** The blocks in a signature sector that verify against `contents`; throws on a damaged or failing block. */
function checkSignatureSector(contents: Buffer, sector: Buffer): BlockResult[] {
  const digest = sha256(contents);
  const results: BlockResult[] = [];
  for (let i = 0; i < 3; i++) {
    const block = sector.subarray(i * BLOCK, (i + 1) * BLOCK);
    if (block[0] !== 0xe7) continue; /* an unused slot */
    if (block[1] !== 0x02) throw new FirmwareError(`signature block ${i} is not an RSA secure boot v2 block (version ${block[1]})`);
    if (crc32(block.subarray(0, SIGNED_LEN)) !== block.readUInt32LE(SIGNED_LEN)) {
      throw new FirmwareError(`signature block ${i} is damaged (CRC does not match)`);
    }
    if (!block.subarray(4, 36).equals(digest)) {
      throw new FirmwareError(`signature block ${i} was made for different contents`);
    }
    const keyBytes = block.subarray(36, 36 + 776);
    const n = bigFromLittle(block.subarray(36, 36 + 384));
    const e = BigInt(block.readUInt32LE(36 + 384));
    const sig = bigFromLittle(block.subarray(36 + 776, 36 + 776 + 384));
    if (!pssVerify(digest, sig, n, e)) throw new FirmwareError(`signature block ${i} does not verify`);
    results.push({ keyDigest: sha256(keyBytes).toString('hex') });
  }
  return results;
}

/**
 * Parse and check one image. Throws FirmwareError when it is not a node
 * application image, or when its signature is damaged or does not verify.
 */
export function inspectImage(bytes: Buffer, options: { project?: string; trustedKeyDigest?: string | null } = {}): FirmwareImage {
  const project = options.project ?? NODE_PROJECT;
  if (bytes.length < APP_DESC_OFFSET + 0xb0) throw new FirmwareError('too short to be an application image');
  if (bytes[0] !== 0xe9) throw new FirmwareError('not an ESP application image (no 0xE9 header)');
  if (bytes.readUInt32LE(APP_DESC_OFFSET) !== APP_DESC_MAGIC) throw new FirmwareError('no application description at offset 0x20');
  const desc = {
    version: cString(bytes, APP_DESC_OFFSET + 0x10, 32),
    project: cString(bytes, APP_DESC_OFFSET + 0x30, 32),
    idfVersion: cString(bytes, APP_DESC_OFFSET + 0x70, 32),
  };
  if (!desc.version) throw new FirmwareError('the image carries no version');
  if (desc.project !== project) throw new FirmwareError(`the image is for project "${desc.project}", not ${project}`);

  let signature: SignatureStatus = 'missing';
  let keyDigests: string[] = [];
  const signed = bytes.length >= 2 * SECTOR && bytes.length % SECTOR === 0 && bytes[bytes.length - SECTOR] === 0xe7;
  if (signed) {
    const contents = bytes.subarray(0, bytes.length - SECTOR);
    const blocks = checkSignatureSector(contents, bytes.subarray(bytes.length - SECTOR));
    if (!blocks.length) throw new FirmwareError('the signature sector holds no usable block');
    keyDigests = blocks.map((b) => b.keyDigest);
    const trusted = options.trustedKeyDigest?.trim().toLowerCase() || null;
    signature = !trusted ? 'present' : keyDigests.includes(trusted) ? 'verified' : 'wrong_key';
  }

  return {
    sha256: sha256(bytes).toString('hex'),
    size: bytes.length,
    version: desc.version,
    project: desc.project,
    idfVersion: desc.idfVersion,
    signature,
    keyDigests,
  };
}

/** Images a rollout may offer: the unit refuses unsigned ones, and ones signed by another key. */
export function mayRollOut(signature: string): boolean {
  return signature === 'verified' || signature === 'present';
}
