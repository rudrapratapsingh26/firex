import { describe as suite, expect, test } from 'vitest';

import {
  checkPossession, formatPairingCode, generatePairingCode, matchPairing, normalisePairingCode,
  parseProvisionRequest, replyMac,
} from '../server/pairing.ts';
import { buildProvisionRequest, newP256 } from '../sim/keys.ts';

suite('pairing codes', () => {
  test('are ten Crockford base32 characters in two groups', () => {
    for (let i = 0; i < 50; i++) {
      const code = generatePairingCode();
      expect(code).toMatch(/^[0-9A-HJKMNP-TV-Z]{5}-[0-9A-HJKMNP-TV-Z]{5}$/);
      expect(normalisePairingCode(code)).toBe(code.replace('-', ''));
    }
  });

  test('normalise look-alikes, case and separators', () => {
    expect(normalisePairingCode(' abcde-fgh1o ')).toBe('ABCDEFGH10');
    expect(normalisePairingCode('ILOIL-01234')).toBe('11011' + '01234');
    expect(normalisePairingCode('ABCDE-FGHU0')).toBeNull(); // U is not in the alphabet
    expect(normalisePairingCode('ABCD')).toBeNull();
    expect(formatPairingCode('ABCDEFGH10')).toBe('ABCDE-FGH10');
  });
});

suite('provisioning requests', () => {
  const code = 'ABCDEFGH10';
  const ledger = newP256();
  const tls = newP256();

  test('a well-formed request proves both keys and the code', () => {
    const req = buildProvisionRequest(code, ledger, tls);
    const parsed = parseProvisionRequest(JSON.parse(JSON.stringify(req)));
    expect(typeof parsed).toBe('object');
    expect(checkPossession(req)).toBeNull();
    const open = [{ code: 'ZZZZZZZZZZ' }, { code }];
    expect(matchPairing(req, open)).toEqual({ code });
    expect(matchPairing(req, [{ code: 'ZZZZZZZZZZ' }])).toBeNull();
  });

  test('tampering with any signed field breaks a signature', () => {
    const req = buildProvisionRequest(code, ledger, tls);
    expect(checkPossession({ ...req, nonce: '00'.repeat(16) })).toBe('ledger_sig does not verify');
    const other = newP256();
    const swapped = buildProvisionRequest(code, other, tls);
    expect(checkPossession({ ...req, ledger_sig: swapped.ledger_sig })).toBe('ledger_sig does not verify');
    expect(checkPossession({ ...req, device_id: 'ab'.repeat(16) })).toBe('device_id is not derived from ledger_pub');
  });

  test('malformed requests are refused before any crypto', () => {
    const req = buildProvisionRequest(code, ledger, tls);
    expect(parseProvisionRequest({ ...req, version: 2 })).toBe('unsupported version');
    expect(parseProvisionRequest({ ...req, proof: 'xyz' })).toBe('proof is missing or malformed');
    expect(parseProvisionRequest({ ...req, tls_pub: req.ledger_pub })).toBe('the TLS key must differ from the ledger key');
    expect(parseProvisionRequest(null)).toBe('body must be a JSON object');
  });

  test('the reply MAC binds the nonce, the endpoint and both certificates', () => {
    const reply = { site_id: 's', mqtt_host: 'h', mqtt_port: 8883, ca_pem: 'CA', cert_pem: 'CERT' };
    const mac = replyMac(code, 'aa', reply);
    expect(mac).toMatch(/^[0-9a-f]{64}$/);
    expect(replyMac(code, 'aa', reply)).toBe(mac);
    expect(replyMac(code, 'ab', reply)).not.toBe(mac);
    expect(replyMac(code, 'aa', { ...reply, ca_pem: 'EVIL CA' })).not.toBe(mac);
    expect(replyMac('ZZZZZZZZZZ', 'aa', reply)).not.toBe(mac);
  });
});
