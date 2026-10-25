import { readFileSync } from 'node:fs';
import { join } from 'node:path';

import { describe as suite, expect, test } from 'vitest';

import { crc32, inspectImage, mayRollOut } from '../server/firmware.ts';

/* Synthetic application images: app-signed.bin is app-unsigned.bin signed with
 * `espsecure.py sign_data --version 2` under a throwaway RSA-3072 key whose
 * digest is key-a.digest.hex. key-b.digest.hex belongs to a key that never
 * signed anything. The private keys were not kept. */
const fixture = (name: string) => readFileSync(join(import.meta.dirname, 'fixtures', name));
const KEY_A = fixture('key-a.digest.hex').toString('utf8').trim();
const KEY_B = fixture('key-b.digest.hex').toString('utf8').trim();

suite('firmware images', () => {
  test('a signed image verifies against the configured key digest', () => {
    const image = inspectImage(fixture('app-signed.bin'), { trustedKeyDigest: KEY_A });
    expect(image).toMatchObject({ version: '0.5.0-m5', project: 'firex_node', signature: 'verified', size: 12288 });
    expect(image.keyDigests).toEqual([KEY_A]);
    expect(mayRollOut(image.signature)).toBe(true);
  });

  test('with no key configured a valid signature is only present', () => {
    expect(inspectImage(fixture('app-signed.bin')).signature).toBe('present');
  });

  test('a valid signature under another key is the wrong key, and is not rolled out', () => {
    const image = inspectImage(fixture('app-signed.bin'), { trustedKeyDigest: KEY_B });
    expect(image.signature).toBe('wrong_key');
    expect(mayRollOut(image.signature)).toBe(false);
  });

  test('an unsigned image is missing its signature', () => {
    const image = inspectImage(fixture('app-unsigned.bin'), { trustedKeyDigest: KEY_A });
    expect(image.signature).toBe('missing');
    expect(mayRollOut(image.signature)).toBe(false);
  });

  test('an image for another project is refused', () => {
    expect(() => inspectImage(fixture('app-wrong-project.bin'))).toThrow(/firex_cam/);
  });

  test('changed contents no longer match the signed digest', () => {
    const bytes = Buffer.from(fixture('app-signed.bin'));
    bytes[0x400] ^= 0x01;
    expect(() => inspectImage(bytes, { trustedKeyDigest: KEY_A })).toThrow(/different contents/);
  });

  test('a changed signature fails RSA-PSS even with its CRC made good', () => {
    const bytes = Buffer.from(fixture('app-signed.bin'));
    const block = bytes.length - 4096;
    bytes[block + 36 + 776 + 100] ^= 0x01;
    bytes.writeUInt32LE(crc32(bytes.subarray(block, block + 1196)), block + 1196);
    expect(() => inspectImage(bytes, { trustedKeyDigest: KEY_A })).toThrow(/does not verify/);
  });

  test('a damaged block is reported as damaged', () => {
    const bytes = Buffer.from(fixture('app-signed.bin'));
    bytes[bytes.length - 4096 + 36 + 776 + 5] ^= 0xff;
    expect(() => inspectImage(bytes)).toThrow(/CRC/);
  });

  test('bytes that are not an application image are refused', () => {
    expect(() => inspectImage(Buffer.alloc(8192))).toThrow(/0xE9/);
  });
});
