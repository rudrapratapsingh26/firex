/**
 * Removing a unit from the site: the unit is marked removed, its client
 * certificate is revoked, and the removal is a `unit_removed` entry in the site
 * ledger.
 *
 * What stops a removed unit, in order of how soon it bites: ingest ignores
 * everything it publishes at once; the firmware server refuses it at once (it
 * checks the database, and reloads the CRL); the broker refuses its certificate
 * once Mosquitto has read the new CRL, which it does when it starts, so the
 * broker is restarted after a removal (infra/README.md). The unit's ledger
 * entries, photos and traces stay on the gateway: removal is not deletion.
 */
import { audit, type Db, type Row } from './db.ts';
import type { LiveBus } from './live.ts';
import { revokeCertificate, type Pki } from './pki.ts';
import type { SiteLedger } from './site-ledger.ts';

export interface RemoveUnitOptions {
  db: Db;
  pki: Pki;
  live?: LiveBus | null;
  siteLedger?: SiteLedger | null;
  by: string;
  now?: number;
  /** Called after the CRL is rewritten, so running TLS servers can reload it. */
  onCrlChanged?: () => void;
}

export interface RemoveUnitResult {
  ok: boolean;
  error?: string;
  certSerial?: string | null;
}

export async function removeUnit(deviceId: string, options: RemoveUnitOptions): Promise<RemoveUnitResult> {
  const { db, pki } = options;
  const now = options.now ?? Date.now();
  const device = db.prepare('SELECT id, cert_serial, removed_at FROM devices WHERE id = ?').get(deviceId) as Row | undefined;
  if (!device) return { ok: false, error: 'no such unit' };
  if (device.removed_at !== null) return { ok: false, error: 'this unit was already removed' };

  const serial = device.cert_serial ? String(device.cert_serial) : null;
  db.prepare('UPDATE devices SET removed_at = ? WHERE id = ?').run(now, deviceId);
  if (serial) await revokeCertificate(pki, serial, now);
  options.siteLedger?.append('unit_removed', { unit: deviceId, by: options.by, cert_serial: serial ?? '' });
  audit(db, options.by, 'unit_removed', { device_id: deviceId, cert_serial: serial }, now);
  options.live?.publish({ kind: 'device', device_id: deviceId });
  options.onCrlChanged?.();
  return { ok: true, certSerial: serial };
}
