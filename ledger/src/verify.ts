/**
 * The verifier — the part an inspector touches.
 *
 * Input: an exported log and the device public key. Output: PASS with the
 * covered time range, or TAMPERED with the first failing entry and the tier
 * that failed. No account, no server, no FireX involvement; that is the point.
 */
import type { KeyObject } from 'node:crypto';

import { toHex } from './cbor.ts';
import { preimage, sha256, verifySignature, ZERO32, type Entry } from './entry.ts';

export type Tier = 'sequence' | 'chain' | 'signature' | 'media';

export interface Failure {
  seq: number;
  index: number;
  tier: Tier;
  reason: string;
}

export interface VerifyResult {
  ok: boolean;
  entries: number;
  from?: number;
  to?: number;
  failure?: Failure;
}

function equal(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}

/**
 * @param media optional map of SHA-256 hex -> true for exported media files,
 *              used to check `media_captured` entries against the actual images.
 */
export function verifyLog(
  entries: Entry[],
  publicKey: KeyObject,
  media?: Set<string>,
): VerifyResult {
  if (entries.length === 0) {
    return { ok: false, entries: 0, failure: { seq: -1, index: -1, tier: 'sequence', reason: 'the log is empty' } };
  }

  // Annotated, so that an entry's byte fields (Uint8Array<ArrayBufferLike>)
  // assign cleanly to it under TypeScript 5.7's typed-array generics.
  let prevHash: Uint8Array = ZERO32;
  let prevSeq: number | null = null;

  for (let i = 0; i < entries.length; i++) {
    const e = entries[i];
    const fail = (tier: Tier, reason: string): VerifyResult => ({
      ok: false,
      entries: entries.length,
      failure: { seq: e.seq, index: i, tier, reason },
    });

    // 1. sequence
    if (prevSeq === null) {
      if (e.seq !== 0 && i === 0) {
        // A log may be exported from the middle of a device's life; only the
        // genesis entry of a device must be seq 0, and it is the one whose
        // prev_hash is all zeros.
        if (equal(e.prev_hash, ZERO32)) {
          return fail('sequence', `genesis entry has prev_hash of zero but seq ${e.seq}`);
        }
        prevHash = e.prev_hash; // accept the exported starting point
      }
    } else if (e.seq !== prevSeq + 1) {
      return fail('sequence', `expected seq ${prevSeq + 1}, found ${e.seq}`);
    }

    // 2. chain
    if (!equal(e.prev_hash, prevHash)) {
      return fail('chain', `prev_hash does not match the hash of entry ${e.seq - 1}`);
    }
    const recomputed = sha256(preimage(e));
    if (!equal(recomputed, e.hash)) {
      return fail('chain', `hash does not match the entry's own contents (recomputed ${toHex(recomputed).slice(0, 8)}…)`);
    }

    // 3. signature
    if (!verifySignature(e, publicKey)) {
      return fail('signature', 'signature does not verify against the device key');
    }

    // 4. media, when the exported files are present
    if (media && e.type === 'media_captured') {
      const frames = (e.payload.frames as Uint8Array[] | undefined) ?? [];
      for (const frame of frames) {
        if (!media.has(toHex(frame))) {
          return fail('media', `no exported image matches frame hash ${toHex(frame).slice(0, 8)}…`);
        }
      }
    }

    prevHash = e.hash;
    prevSeq = e.seq;
  }

  return {
    ok: true,
    entries: entries.length,
    from: entries[0].ts,
    to: entries[entries.length - 1].ts,
  };
}

export function describe(result: VerifyResult): string {
  if (result.ok) {
    const from = new Date((result.from ?? 0) * 1000).toISOString().replace('.000Z', 'Z');
    const to = new Date((result.to ?? 0) * 1000).toISOString().replace('.000Z', 'Z');
    return `PASS  ${result.entries} entries, ${from} to ${to}`;
  }
  const f = result.failure!;
  return `TAMPERED at entry ${f.seq} (${f.tier}): ${f.reason}`;
}
