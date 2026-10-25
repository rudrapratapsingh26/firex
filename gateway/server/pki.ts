/**
 * The site PKI: one ECDSA P-256 CA per gateway, generated on first start and
 * kept in the data volume. It signs the broker's server certificate, the
 * gateway's own client certificate, and one client certificate per node at
 * provisioning. Nothing leaves the site; there is no public CA involved.
 *
 * Revocation is not implemented in M3: a device is removed by deleting it,
 * which the ingest path then ignores. Broker-side revocation by serial is M5.
 */
import 'reflect-metadata';

import { createHash, webcrypto } from 'node:crypto';

type CryptoKey = webcrypto.CryptoKey;
type CryptoKeyPair = webcrypto.CryptoKeyPair;
import { chmodSync, existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

import * as x509 from '@peculiar/x509';

x509.cryptoProvider.set(webcrypto as never);

const SIGN = { name: 'ECDSA', namedCurve: 'P-256', hash: 'SHA-256' } as const;
const IMPORT = { name: 'ECDSA', namedCurve: 'P-256' } as const;
const DAY = 86_400_000;

export interface Pki {
  dir: string;
  caCert: x509.X509Certificate;
  caCertPem: string;
  caKey: CryptoKey;
}

export interface IssuedCertificate {
  certPem: string;
  serial: string;
}

function pemArmor(label: string, der: ArrayBuffer): string {
  const b64 = Buffer.from(der).toString('base64');
  const lines = b64.match(/.{1,64}/g) ?? [];
  return `-----BEGIN ${label}-----\n${lines.join('\n')}\n-----END ${label}-----\n`;
}

function pemBody(pem: string): Buffer {
  return Buffer.from(pem.replace(/-----[A-Z ]+-----/g, '').replace(/\s+/g, ''), 'base64');
}

function writePrivate(path: string, text: string): void {
  writeFileSync(path, text, { mode: 0o600 });
  try {
    chmodSync(path, 0o600);
  } catch {
    /* not supported on every filesystem; the volume permissions still apply */
  }
}

export async function loadOrCreateCa(dir: string, siteName: string): Promise<Pki> {
  mkdirSync(dir, { recursive: true });
  const certPath = join(dir, 'ca.crt');
  const keyPath = join(dir, 'ca.key');

  if (existsSync(certPath) && existsSync(keyPath)) {
    const caCertPem = readFileSync(certPath, 'utf8');
    const caKey = await webcrypto.subtle.importKey('pkcs8', pemBody(readFileSync(keyPath, 'utf8')), IMPORT, false, ['sign']);
    return { dir, caCert: new x509.X509Certificate(caCertPem), caCertPem, caKey };
  }

  const keys = await webcrypto.subtle.generateKey(SIGN, true, ['sign', 'verify']) as CryptoKeyPair;
  const now = Date.now();
  const caCert = await x509.X509CertificateGenerator.createSelfSigned({
    name: `CN=FireX site CA ${siteName.replace(/[,=+<>#;"\\]/g, ' ')}, O=FireX`,
    keys,
    notBefore: new Date(now - DAY),
    notAfter: new Date(now + 3650 * DAY),
    signingAlgorithm: SIGN,
    extensions: [
      new x509.BasicConstraintsExtension(true, 0, true),
      new x509.KeyUsagesExtension(x509.KeyUsageFlags.keyCertSign | x509.KeyUsageFlags.cRLSign, true),
      await x509.SubjectKeyIdentifierExtension.create(keys.publicKey),
    ],
  });
  const caCertPem = caCert.toString('pem');
  writePrivate(keyPath, pemArmor('PRIVATE KEY', await webcrypto.subtle.exportKey('pkcs8', keys.privateKey)));
  writeFileSync(certPath, caCertPem);
  return { dir, caCert, caCertPem, caKey: keys.privateKey };
}

export async function rawPointToKey(pointHex: string): Promise<CryptoKey> {
  return webcrypto.subtle.importKey('raw', Buffer.from(pointHex, 'hex'), IMPORT, true, ['verify']);
}

export async function issueCertificate(
  pki: Pki,
  options: { commonName: string; publicKey: CryptoKey; usage: 'client' | 'server'; names?: string[]; days?: number },
): Promise<IssuedCertificate> {
  const now = Date.now();
  const extensions: x509.Extension[] = [
    new x509.BasicConstraintsExtension(false, undefined, true),
    new x509.KeyUsagesExtension(x509.KeyUsageFlags.digitalSignature, true),
    new x509.ExtendedKeyUsageExtension([options.usage === 'client' ? x509.ExtendedKeyUsage.clientAuth : x509.ExtendedKeyUsage.serverAuth]),
    await x509.SubjectKeyIdentifierExtension.create(options.publicKey),
    await x509.AuthorityKeyIdentifierExtension.create(pki.caCert),
  ];
  if (options.names?.length) {
    extensions.push(new x509.SubjectAlternativeNameExtension(options.names.map((name) => ({
      type: /^[\d.]+$/.test(name) || name.includes(':') ? 'ip' as const : 'dns' as const,
      value: name,
    }))));
  }
  const cert = await x509.X509CertificateGenerator.create({
    subject: `CN=${options.commonName}`,
    issuer: pki.caCert.subject,
    publicKey: options.publicKey,
    signingKey: pki.caKey,
    signingAlgorithm: SIGN,
    notBefore: new Date(now - 3600_000),
    notAfter: new Date(now + (options.days ?? 730) * DAY),
    extensions,
  });
  return { certPem: cert.toString('pem'), serial: cert.serialNumber };
}

async function issueWithNewKey(pki: Pki, base: string, commonName: string, usage: 'client' | 'server', names?: string[]): Promise<void> {
  const keys = await webcrypto.subtle.generateKey(SIGN, true, ['sign', 'verify']) as CryptoKeyPair;
  const { certPem } = await issueCertificate(pki, { commonName, publicKey: keys.publicKey, usage, names });
  writePrivate(join(pki.dir, `${base}.key`), pemArmor('PRIVATE KEY', await webcrypto.subtle.exportKey('pkcs8', keys.privateKey)));
  writeFileSync(join(pki.dir, `${base}.crt`), certPem);
}

/**
 * The broker's server certificate (names in its SAN) and the gateway's client
 * certificate. Reissued when the configured broker names change.
 */
export async function ensureServiceCertificates(pki: Pki, brokerNames: string[]): Promise<void> {
  const namesPath = join(pki.dir, 'broker.names');
  const wanted = [...new Set(brokerNames)].sort().join(',');
  const current = existsSync(namesPath) ? readFileSync(namesPath, 'utf8') : '';
  if (!existsSync(join(pki.dir, 'broker.crt')) || current !== wanted) {
    await issueWithNewKey(pki, 'broker', 'firex-broker', 'server', wanted.split(','));
    writeFileSync(namesPath, wanted);
  }
  if (!existsSync(join(pki.dir, 'server-client.crt'))) {
    await issueWithNewKey(pki, 'server-client', 'firex-server', 'client');
  }
  await ensureCrl(pki);
}

export function crlPath(pki: Pki): string {
  return join(pki.dir, 'crl.pem');
}

interface Revoked {
  serial: string;
  revoked_at: number;
}

function readRevoked(pki: Pki): Revoked[] {
  const path = join(pki.dir, 'revoked.json');
  return existsSync(path) ? JSON.parse(readFileSync(path, 'utf8')) as Revoked[] : [];
}

/**
 * The site CA's revocation list, rewritten from <pki>/revoked.json. Mosquitto
 * reads it as `crlfile` and refuses a revoked unit's certificate; the dev broker
 * and the firmware server load it too. It is valid for ten years, so an old CRL
 * never turns into "no unit may connect".
 */
export async function writeCrl(pki: Pki, now = Date.now()): Promise<string> {
  const crl = await x509.X509CrlGenerator.create({
    issuer: pki.caCert.subject,
    thisUpdate: new Date(now - 3600_000),
    nextUpdate: new Date(now + 3650 * DAY),
    entries: readRevoked(pki).map((r) => ({ serialNumber: r.serial, revocationDate: new Date(r.revoked_at) })),
    signingAlgorithm: SIGN,
    signingKey: pki.caKey,
  });
  const pem = pemArmor('X509 CRL', crl.rawData); /* the label OpenSSL, and so Mosquitto and Node's TLS, expect */
  writeFileSync(crlPath(pki), pem);
  return pem;
}

/** An empty CRL on first start: Mosquitto will not start without the file it names. */
export async function ensureCrl(pki: Pki): Promise<void> {
  if (!existsSync(crlPath(pki))) await writeCrl(pki);
}

/** Revoke one certificate by serial (hex, as issueCertificate returned it) and rewrite the CRL. */
export async function revokeCertificate(pki: Pki, serial: string, now = Date.now()): Promise<void> {
  const revoked = readRevoked(pki);
  if (!revoked.some((r) => r.serial.toLowerCase() === serial.toLowerCase())) {
    revoked.push({ serial, revoked_at: now });
    writeFileSync(join(pki.dir, 'revoked.json'), JSON.stringify(revoked, null, 2));
  }
  await writeCrl(pki, now);
}

export function readServiceCredentials(pki: Pki): { ca: string; cert: string; key: string } {
  return {
    ca: pki.caCertPem,
    cert: readFileSync(join(pki.dir, 'server-client.crt'), 'utf8'),
    key: readFileSync(join(pki.dir, 'server-client.key'), 'utf8'),
  };
}

export function fingerprint(pem: string): string {
  return createHash('sha256').update(pemBody(pem)).digest('hex');
}
