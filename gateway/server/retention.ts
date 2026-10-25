/**
 * Retention (docs/media-policy.md): incident images are deleted after the
 * site's window, and each deletion is written to the audit log. Ledger entries
 * are never deleted: a hash outlives its image. Telemetry older than its window
 * and expired sessions are purged.
 */
import { rmSync } from 'node:fs';

import { audit, type Db, type Row } from './db.ts';
import { mediaPath } from './ingest.ts';

export interface RetentionResult {
  mediaDeleted: number;
  telemetryRows: number;
}

export function runRetention(db: Db, mediaDir: string, windows: { mediaDays: number; telemetryDays: number }, now = Date.now()): RetentionResult {
  const mediaCutoff = now - windows.mediaDays * 86_400_000;
  const expired = db.prepare('SELECT sha256, device_id, received_at FROM media WHERE deleted_at IS NULL AND received_at < ?').all(mediaCutoff) as Row[];
  for (const row of expired) {
    rmSync(mediaPath(mediaDir, String(row.device_id), String(row.sha256)), { force: true });
    db.prepare('UPDATE media SET deleted_at = ? WHERE sha256 = ?').run(now, row.sha256 as string);
    audit(db, 'retention', 'media_deleted', {
      sha256: row.sha256, device_id: row.device_id, age_days: Math.floor((now - Number(row.received_at)) / 86_400_000),
    }, now);
  }
  const telemetryCutoff = Math.floor((now - windows.telemetryDays * 86_400_000) / 60_000);
  const telemetry = db.prepare('DELETE FROM telemetry WHERE minute < ?').run(telemetryCutoff);
  db.prepare('DELETE FROM sessions WHERE expires_at < ?').run(now);
  return { mediaDeleted: expired.length, telemetryRows: Number(telemetry.changes) };
}

export function startRetention(db: Db, mediaDir: string, windows: { mediaDays: number; telemetryDays: number }): () => void {
  const timer = setInterval(() => runRetention(db, mediaDir, windows), 3_600_000);
  timer.unref();
  runRetention(db, mediaDir, windows);
  return () => clearInterval(timer);
}
