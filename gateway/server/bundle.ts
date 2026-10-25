/**
 * Log bundles for inspectors: the stored entries of one device epoch, its
 * ledger public key, and the hashes of the images the gateway holds for it.
 * `firex-verify verify bundle.json --media <dir>` checks the lot.
 */
import { existsSync } from 'node:fs';

import type { EntryJson } from '../../ledger/src/entry.ts';
import { publicKeyToPem } from '../../ledger/src/entry.ts';
import type { Db, Row } from './db.ts';
import { mediaPath } from './ingest.ts';
import { p256PointToKey } from './pairing.ts';

export interface LogBundle {
  device_id: string;
  site_id: string;
  epoch: number;
  exported_at: string;
  public_key_pem: string;
  media: string[];
  log: EntryJson[];
}

export function rowToEntryJson(row: Row, deviceId: string): EntryJson {
  return {
    seq: Number(row.seq),
    ts: Number(row.ts),
    ts_src: String(row.ts_src),
    device_id: deviceId,
    type: String(row.type),
    payload: JSON.parse(String(row.payload)),
    fw_hash: String(row.fw_hash),
    prev_hash: String(row.prev_hash),
    hash: String(row.hash),
    sig: String(row.sig),
  };
}

export function buildBundle(db: Db, mediaDir: string, deviceId: string, options: { epoch?: number; fromSeq?: number; toSeq?: number } = {}): LogBundle | null {
  const device = db.prepare('SELECT id, site_id, ledger_pub, epoch FROM devices WHERE id = ?').get(deviceId) as Row | undefined;
  if (!device) return null;
  const epoch = options.epoch ?? Number(device.epoch);
  const rows = db.prepare(
    'SELECT * FROM ledger_entries WHERE device_id = ? AND epoch = ? AND seq >= ? AND seq <= ? ORDER BY seq',
  ).all(deviceId, epoch, options.fromSeq ?? 0, options.toSeq ?? Number.MAX_SAFE_INTEGER) as Row[];

  const media = db.prepare(
    `SELECT DISTINCT m.sha256 FROM media m JOIN media_refs r ON r.sha256 = m.sha256
     WHERE r.device_id = ? AND r.epoch = ? AND m.deleted_at IS NULL ORDER BY m.sha256`,
  ).all(deviceId, epoch) as Row[];

  return {
    device_id: deviceId,
    site_id: String(device.site_id),
    epoch,
    exported_at: new Date().toISOString(),
    public_key_pem: publicKeyToPem(p256PointToKey(String(device.ledger_pub))),
    media: media.map((m) => String(m.sha256)).filter((sha) => existsSync(mediaPath(mediaDir, deviceId, sha))),
    log: rows.map((row) => rowToEntryJson(row, deviceId)),
  };
}
