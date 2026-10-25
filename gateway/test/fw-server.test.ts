import { generateKeyPairSync, webcrypto } from 'node:crypto';
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { request } from 'node:https';
import type { AddressInfo } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { afterAll, beforeAll, describe as suite, expect, test } from 'vitest';

import { openDb, type Db, type Row } from '../server/db.ts';
import { createFirmwareServer, firmwarePath, sameSerial, type FirmwareServer } from '../server/fw-server.ts';
import { ensureServiceCertificates, issueCertificate, loadOrCreateCa, revokeCertificate, type Pki } from '../server/pki.ts';

const SITE = 'site-001';
const IMAGE = readFileSync(join(import.meta.dirname, 'fixtures', 'app-signed.bin'));
const SHA = 'f'.repeat(64);

let dir: string;
let db: Db;
let pki: Pki;
let server: FirmwareServer;
let base: string;

interface Client {
  id: string;
  cert: string;
  key: string;
  serial: string;
}

async function client(ca: Pki, id: string): Promise<Client> {
  const keys = generateKeyPairSync('ec', { namedCurve: 'prime256v1' });
  const publicKey = await webcrypto.subtle.importKey('spki', keys.publicKey.export({ type: 'spki', format: 'der' }), { name: 'ECDSA', namedCurve: 'P-256' }, true, ['verify']);
  const issued = await issueCertificate(ca, { commonName: id, publicKey, usage: 'client' });
  return { id, cert: issued.certPem, key: keys.privateKey.export({ type: 'pkcs8', format: 'pem' }).toString(), serial: issued.serial };
}

function register(c: Client): void {
  db.prepare('INSERT INTO devices (id, site_id, ledger_pub, tls_pub, cert_serial, provisioned_at) VALUES (?, ?, ?, ?, ?, 0)').run(c.id, SITE, '04', '04', c.serial);
}

function get(path: string, c: Client): Promise<{ status: number; body: Buffer }> {
  return new Promise((resolve, reject) => {
    const req = request(`${base}${path}`, { ca: pki.caCertPem, cert: c.cert, key: c.key, servername: 'localhost', agent: false }, (res) => {
      const chunks: Buffer[] = [];
      res.on('data', (d: Buffer) => chunks.push(d));
      res.on('end', () => resolve({ status: res.statusCode ?? 0, body: Buffer.concat(chunks) }));
      res.on('error', reject);
    });
    req.on('error', reject);
    req.end();
  });
}

beforeAll(async () => {
  dir = mkdtempSync(join(tmpdir(), 'firex-fw-'));
  db = openDb(':memory:');
  db.prepare("INSERT INTO sites (id, name) VALUES (?, 'Test')").run(SITE);
  pki = await loadOrCreateCa(join(dir, 'pki'), 'Test');
  await ensureServiceCertificates(pki, ['localhost', '127.0.0.1']);
  mkdirSync(join(dir, 'firmware'));
  writeFileSync(firmwarePath(join(dir, 'firmware'), SHA), IMAGE);
  db.prepare("INSERT INTO firmware (sha256, version, project, size, signature, uploaded_by, uploaded_at) VALUES (?, '0.5.0-m5', 'firex_node', ?, 'verified', 'admin', 0)").run(SHA, IMAGE.length);
  server = createFirmwareServer({ db, pki, firmwareDir: join(dir, 'firmware') });
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  base = `https://127.0.0.1:${(server.address() as AddressInfo).port}`;
}, 30_000);

afterAll(async () => {
  await new Promise<void>((resolve) => server.close(() => resolve()));
  db.close();
  rmSync(dir, { recursive: true, force: true });
});

suite('firmware downloads', () => {
  test('a registered unit downloads the image, and the download is audited', async () => {
    const unit = await client(pki, 'a1'.repeat(16));
    register(unit);
    const res = await get(`/fw/${SHA}.bin`, unit);
    expect(res.status).toBe(200);
    expect(res.body.equals(IMAGE)).toBe(true);
    const audit = db.prepare("SELECT actor FROM audit WHERE action = 'firmware_downloaded'").get() as Row;
    expect(audit.actor).toBe(unit.id);
    expect((await get(`/fw/${'0'.repeat(64)}.bin`, unit)).status).toBe(404);
  });

  test('a certificate whose CN is not a unit of the site is refused', async () => {
    const stranger = await client(pki, 'b2'.repeat(16));
    expect((await get(`/fw/${SHA}.bin`, stranger)).status).toBe(403);
  });

  test('a removed unit is refused, and an old certificate of a re-provisioned unit too', async () => {
    const unit = await client(pki, 'c3'.repeat(16));
    register(unit);
    const replacement = await client(pki, unit.id);
    db.prepare('UPDATE devices SET cert_serial = ? WHERE id = ?').run(replacement.serial, unit.id);
    expect((await get(`/fw/${SHA}.bin`, unit)).status).toBe(403);
    expect((await get(`/fw/${SHA}.bin`, replacement)).status).toBe(200);
    db.prepare('UPDATE devices SET removed_at = 1 WHERE id = ?').run(unit.id);
    expect((await get(`/fw/${SHA}.bin`, replacement)).status).toBe(403);
  });

  test('a revoked certificate does not get through the TLS handshake once the CRL is reloaded', async () => {
    const unit = await client(pki, 'd4'.repeat(16));
    register(unit);
    expect((await get(`/fw/${SHA}.bin`, unit)).status).toBe(200);
    await revokeCertificate(pki, unit.serial);
    server.reloadCrl();
    const outcome = await get(`/fw/${SHA}.bin`, unit).then((r) => `status ${r.status}`, (err: Error) => `refused: ${err.message}`);
    expect(outcome).toMatch(/^refused/);
  });

  test('a certificate from another CA is refused', async () => {
    const rogue = await loadOrCreateCa(join(dir, 'rogue'), 'rogue');
    const unit = await client(rogue, 'e5'.repeat(16));
    register(unit);
    const outcome = await get(`/fw/${SHA}.bin`, unit).then((r) => `status ${r.status}`, (err: Error) => `refused: ${err.message}`);
    expect(outcome).toMatch(/^refused/);
  });

  test('serials compare as numbers', () => {
    expect(sameSerial('00AB12', 'ab12')).toBe(true);
    expect(sameSerial('ab12', 'ab13')).toBe(false);
    expect(sameSerial('', '')).toBe(false);
  });
});
