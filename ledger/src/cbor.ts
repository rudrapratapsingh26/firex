/**
 * Canonical CBOR (RFC 8949 section 4.2), restricted to the subset the FireX
 * ledger uses: unsigned integers, text strings, byte strings, arrays, maps and
 * null. Deterministic bytes are the whole point — two implementations that
 * disagree produce two different hashes, and the verifier stops being portable.
 *
 * SPEC.md section 4 is the normative description of this subset.
 */

export type CborValue =
  | number
  | string
  | Uint8Array
  | null
  | boolean
  | CborValue[]
  | { [key: string]: CborValue };

function head(major: number, value: number): Uint8Array {
  if (value < 24) return Uint8Array.from([(major << 5) | value]);
  if (value < 0x100) return Uint8Array.from([(major << 5) | 24, value]);
  if (value < 0x10000) return Uint8Array.from([(major << 5) | 25, value >> 8, value & 0xff]);
  if (value < 0x100000000) {
    return Uint8Array.from([
      (major << 5) | 26,
      (value >>> 24) & 0xff,
      (value >>> 16) & 0xff,
      (value >>> 8) & 0xff,
      value & 0xff,
    ]);
  }
  // 64-bit: split into two 32-bit halves without BigInt round-trips.
  const hi = Math.floor(value / 0x100000000);
  const lo = value >>> 0;
  return Uint8Array.from([
    (major << 5) | 27,
    (hi >>> 24) & 0xff, (hi >>> 16) & 0xff, (hi >>> 8) & 0xff, hi & 0xff,
    (lo >>> 24) & 0xff, (lo >>> 16) & 0xff, (lo >>> 8) & 0xff, lo & 0xff,
  ]);
}

function concat(chunks: Uint8Array[]): Uint8Array {
  let total = 0;
  for (const c of chunks) total += c.length;
  const out = new Uint8Array(total);
  let at = 0;
  for (const c of chunks) {
    out.set(c, at);
    at += c.length;
  }
  return out;
}

/** Compare two byte arrays lexicographically — how canonical CBOR orders map keys. */
function compareBytes(a: Uint8Array, b: Uint8Array): number {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) {
    if (a[i] !== b[i]) return a[i] - b[i];
  }
  return a.length - b.length;
}

export function encode(value: CborValue): Uint8Array {
  if (value === null) return Uint8Array.from([0xf6]);
  if (value === true) return Uint8Array.from([0xf5]);
  if (value === false) return Uint8Array.from([0xf4]);

  if (typeof value === 'number') {
    if (!Number.isInteger(value) || value < 0) {
      throw new Error(
        `ledger entries carry unsigned integers only (got ${value}); encode scores in hundredths`,
      );
    }
    return head(0, value);
  }

  if (typeof value === 'string') {
    const bytes = new TextEncoder().encode(value);
    return concat([head(3, bytes.length), bytes]);
  }

  if (value instanceof Uint8Array) {
    return concat([head(2, value.length), value]);
  }

  if (Array.isArray(value)) {
    return concat([head(4, value.length), ...value.map(encode)]);
  }

  const keys = Object.keys(value);
  const pairs = keys.map((k) => ({ k: encode(k), v: encode(value[k]) }));
  pairs.sort((a, b) => compareBytes(a.k, b.k));
  return concat([head(5, pairs.length), ...pairs.flatMap((p) => [p.k, p.v])]);
}

export function toHex(bytes: Uint8Array): string {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

export function fromHex(hex: string): Uint8Array {
  if (hex.length % 2 !== 0) throw new Error('hex string has an odd length');
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}
