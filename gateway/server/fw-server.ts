/**
 * The firmware download server: HTTPS on FIREX_FW_PORT for units fetching the
 * image an `ota` command named (docs/transport.md).
 *
 * Mutual TLS against the site CA, with the broker's certificate as the server's:
 * a unit already trusts the CA and knows the gateway's address. A download is
 * served only to a certificate whose CN is a registered unit that has not been
 * removed, and whose serial is the one that unit was given at provisioning, so
 * a certificate left over from an earlier provisioning cannot fetch images
 * either. The CRL is loaded too, and reloaded when a unit is removed.
 *
 * Integrity does not rest on this channel. The command carries the image's
 * SHA-256 and size, and the unit verifies the image's signature before it will
 * boot it. Every download and every refusal is written to the audit log.
 */
import { createReadStream, existsSync, readFileSync, statSync } from 'node:fs';
import { createServer, type Server } from 'node:https';
import { join } from 'node:path';
import type { TLSSocket } from 'node:tls';

import { audit, type Db, type Row } from './db.ts';
import { crlPath, type Pki } from './pki.ts';

export interface FirmwareServerOptions {
  db: Db;
  pki: Pki;
  firmwareDir: string;
  now?: () => number;
}

export type FirmwareServer = Server & { reloadCrl: () => void };

export function firmwarePath(firmwareDir: string, sha: string): string {
  return join(firmwareDir, `${sha}.bin`);
}

/** Certificate serials compare as numbers: OpenSSL prints them upper case, the CA library lower case, either may keep leading zeros. */
export function sameSerial(a: string, b: string): boolean {
  const norm = (s: string) => s.replace(/[^0-9a-f]/gi, '').replace(/^0+/, '').toLowerCase();
  return norm(a) !== '' && norm(a) === norm(b);
}

function tlsOptions(pki: Pki) {
  return {
    key: readFileSync(join(pki.dir, 'broker.key')),
    cert: readFileSync(join(pki.dir, 'broker.crt')),
    ca: pki.caCertPem,
    crl: existsSync(crlPath(pki)) ? readFileSync(crlPath(pki)) : undefined,
  };
}

export function createFirmwareServer(options: FirmwareServerOptions): FirmwareServer {
  const { db, pki, firmwareDir } = options;
  const now = options.now ?? Date.now;
  const server = createServer({ ...tlsOptions(pki), requestCert: true, rejectUnauthorized: true }, (req, res) => {
    const peer = (req.socket as TLSSocket).getPeerCertificate();
    const cn = peer?.subject?.CN ? String(peer.subject.CN) : '';
    const refuse = (status: number, message: string, detail: Record<string, unknown> = {}) => {
      audit(db, cn || 'unknown', 'firmware_download_refused', { reason: message, url: req.url, ...detail }, now());
      res.writeHead(status, { 'content-type': 'text/plain' }).end(`${message}\n`);
    };

    const match = /^\/fw\/([0-9a-f]{64})\.bin$/.exec(req.url ?? '');
    if (req.method !== 'GET' || !match) return refuse(404, 'not found');
    const device = db.prepare('SELECT id, cert_serial, removed_at FROM devices WHERE id = ?').get(cn) as Row | undefined;
    if (!device) return refuse(403, 'not a unit of this site');
    if (device.removed_at !== null) return refuse(403, 'this unit has been removed from the site');
    if (device.cert_serial && !sameSerial(String(device.cert_serial), String(peer.serialNumber ?? ''))) {
      return refuse(403, 'not the certificate this unit was given');
    }
    const sha = match[1];
    const firmware = db.prepare('SELECT size FROM firmware WHERE sha256 = ?').get(sha) as Row | undefined;
    const path = firmwarePath(firmwareDir, sha);
    if (!firmware || !existsSync(path)) return refuse(404, 'no such firmware on this gateway', { sha256: sha });

    audit(db, cn, 'firmware_downloaded', { sha256: sha, size: Number(firmware.size) }, now());
    res.writeHead(200, { 'content-type': 'application/octet-stream', 'content-length': String(statSync(path).size) });
    createReadStream(path).pipe(res);
  });
  return Object.assign(server, { reloadCrl: () => server.setSecureContext(tlsOptions(pki)) });
}
