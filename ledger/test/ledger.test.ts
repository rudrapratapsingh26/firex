import { describe as suite, expect, test } from 'vitest';

import { encode, fromHex, toHex } from '../src/cbor.ts';
import {
  deviceIdFor, entryFromJson, entryToJson, generateDeviceKey, LedgerWriter, publicKeyFromPem,
  sha256, signEntry,
} from '../src/entry.ts';
import { buildVectors } from '../src/vectors.ts';
import { verifyLog } from '../src/verify.ts';

suite('canonical CBOR', () => {
  test('map keys are sorted by their encoded bytes, not by insertion order', () => {
    const a = encode({ zz: 1, a: 2, mm: 3 });
    const b = encode({ a: 2, mm: 3, zz: 1 });
    expect(toHex(a)).toBe(toHex(b));
  });

  test('integers use the shortest form', () => {
    expect(toHex(encode(0))).toBe('00');
    expect(toHex(encode(23))).toBe('17');
    expect(toHex(encode(24))).toBe('1818');
    expect(toHex(encode(256))).toBe('190100');
  });

  test('byte strings keep their length prefix', () => {
    expect(toHex(encode(fromHex('deadbeef')))).toBe('44deadbeef');
  });

  test('floats are refused, because scores travel as hundredths', () => {
    expect(() => encode(0.62)).toThrow(/hundredths/);
  });
});

suite('entries', () => {
  test('an entry hashes and verifies against its device key', () => {
    const { privateKey, publicKey } = generateDeviceKey();
    const w = new LedgerWriter(deviceIdFor(publicKey), sha256(new Uint8Array([1, 2, 3])), privateKey);
    w.append('boot', { fw_version: '0.1.0', reset_reason: 'power_on', boot_count: 1 }, 1789000000);
    expect(verifyLog(w.entries, publicKey).ok).toBe(true);
  });

  test('JSON round-trips to identical bytes', () => {
    const { privateKey, publicKey } = generateDeviceKey();
    const w = new LedgerWriter(deviceIdFor(publicKey), sha256(new Uint8Array([9])), privateKey);
    const frame = sha256(new TextEncoder().encode('frame'));
    const original = w.append('media_captured', {
      trigger: 'CRITICAL', frames: [frame], clip: null, codec: 'mjpeg', resolution: '640x480',
    }, 1789000001);
    const round = entryFromJson(entryToJson(original));
    expect(toHex(round.hash)).toBe(toHex(original.hash));
    expect(verifyLog([round], publicKey).ok).toBe(true);
  });

  test('a chain of five entries links prev_hash to the previous hash', () => {
    const { privateKey, publicKey } = generateDeviceKey();
    const w = new LedgerWriter(deviceIdFor(publicKey), sha256(new Uint8Array([7])), privateKey);
    for (let i = 0; i < 5; i++) w.append('heartbeat_gap', { seconds: i }, 1789000000 + i);
    for (let i = 1; i < 5; i++) {
      expect(toHex(w.entries[i].prev_hash)).toBe(toHex(w.entries[i - 1].hash));
    }
    expect(verifyLog(w.entries, publicKey).ok).toBe(true);
  });
});

suite('test vectors', () => {
  const vectors = buildVectors();

  test('there are ten of them', () => {
    expect(vectors).toHaveLength(10);
  });

  for (const v of vectors) {
    test(`${v.name} — ${v.expect.ok ? 'PASS' : `TAMPERED (${v.expect.tier})`}`, () => {
      const entries = v.log.map(entryFromJson);
      const result = verifyLog(entries, publicKeyFromPem(v.public_key_pem), new Set(v.media ?? []));
      expect(result.ok).toBe(v.expect.ok);
      if (!v.expect.ok) {
        expect(result.failure?.tier).toBe(v.expect.tier);
        expect(result.failure?.seq).toBe(v.expect.seq);
      }
    });
  }
});

suite('tamper detection', () => {
  test('changing one byte of a payload breaks the chain at that entry', () => {
    const { privateKey, publicKey } = generateDeviceKey();
    const w = new LedgerWriter(deviceIdFor(publicKey), sha256(new Uint8Array([4])), privateKey);
    w.append('boot', { fw_version: '0.1.0', reset_reason: 'power_on', boot_count: 1 }, 1789000000);
    w.append('state_change', { from: 'NORMAL', to: 'CRITICAL', reason: ['gas', 'heat'] }, 1789000100);
    w.append('alert_sent', { service: 'fire', channel: 'sms', recipient: 'station-042', receipt_id: 'r-1' }, 1789000101);

    const edited = w.entries.map((e) => ({ ...e, payload: { ...e.payload } }));
    edited[1].payload = { ...edited[1].payload, to: 'ELEVATED' };

    const result = verifyLog(edited, publicKey);
    expect(result.ok).toBe(false);
    expect(result.failure).toMatchObject({ seq: 1, tier: 'chain' });
  });

  test('re-signing a tampered entry still breaks the next link', () => {
    const { privateKey, publicKey } = generateDeviceKey();
    const w = new LedgerWriter(deviceIdFor(publicKey), sha256(new Uint8Array([5])), privateKey);
    w.append('boot', { boot_count: 1 }, 1789000000);
    w.append('threshold_cross', { modality: 'gas', score: 70, direction: 'up' }, 1789000050);
    w.append('state_change', { from: 'NORMAL', to: 'ELEVATED', reason: ['gas'] }, 1789000055);

    const edited = w.entries.map((e) => ({ ...e, payload: { ...e.payload } }));
    edited[1] = signEntry({ ...edited[1], payload: { modality: 'gas', score: 10, direction: 'up' } }, privateKey);

    const result = verifyLog(edited, publicKey);
    expect(result.ok).toBe(false);
    expect(result.failure).toMatchObject({ seq: 2, tier: 'chain' });
  });
});
