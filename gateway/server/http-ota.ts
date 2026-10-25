/**
 * The M5 routes for firmware, rollouts, traces and removing a unit, and the
 * fields they add to the device and incident views. buildApp registers them.
 *
 *   GET  /api/firmware                    viewer   images on this gateway
 *   POST /api/firmware                    admin    upload an image (application/octet-stream)
 *   POST /api/firmware/:sha/rollout       admin    {units: "all" | [ids]}
 *   GET  /api/rollouts                    viewer
 *   POST /api/rollouts/:id/stop           admin
 *   GET  /api/traces/:sha                 viewer   the raw trace file; audited
 *   DELETE /api/devices/:id               admin    remove the unit and revoke its certificate
 */
import { existsSync, mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs';

import type { FastifyInstance, FastifyReply, FastifyRequest } from 'fastify';

import type { GatewayConfig } from './config.ts';
import { audit, type Db, type Row } from './db.ts';
import { FirmwareError, inspectImage, type FirmwareImage } from './firmware.ts';
import { firmwarePath } from './fw-server.ts';
import type { LiveBus } from './live.ts';
import type { Pki } from './pki.ts';
import { removeUnit } from './removal.ts';
import { RolloutError, type RolloutManager } from './rollout.ts';
import type { SiteLedger } from './site-ledger.ts';
import { tracePath } from './traces.ts';

export const FIRMWARE_MAX_BYTES = 4 * 1024 * 1024;

/** What the M5 routes need beyond the database; absent in tests that do not exercise them. */
export interface OtaDeps {
  rollouts: RolloutManager;
  pki: Pki;
  siteLedger: SiteLedger | null;
  /** Reload the CRL in the firmware server after a removal. */
  reloadCrl?: () => void;
}

type Guard = { preHandler: (request: FastifyRequest, reply: FastifyReply) => Promise<unknown> };

function json(value: unknown): unknown {
  if (value === null || value === undefined) return null;
  try {
    return JSON.parse(String(value));
  } catch {
    return null;
  }
}

/** Check an image and keep it. Throws FirmwareError for an image a unit must never be offered. */
export function storeFirmware(db: Db, config: GatewayConfig, bytes: Buffer, by: string, now: number): FirmwareImage {
  const image = inspectImage(bytes, { trustedKeyDigest: config.fwSigningKeySha256 });
  mkdirSync(config.firmwareDir, { recursive: true });
  const path = firmwarePath(config.firmwareDir, image.sha256);
  writeFileSync(`${path}.part`, bytes);
  renameSync(`${path}.part`, path);
  db.prepare(
    `INSERT INTO firmware (sha256, version, project, size, signature, uploaded_by, uploaded_at) VALUES (?, ?, ?, ?, ?, ?, ?)
     ON CONFLICT(sha256) DO UPDATE SET signature = excluded.signature`,
  ).run(image.sha256, image.version, image.project, image.size, image.signature, by, now);
  audit(db, by, 'firmware_uploaded', { sha256: image.sha256, version: image.version, signature: image.signature }, now);
  return image;
}

/** Fields for GET /api/devices and /api/devices/:id. */
export function otaDeviceFields(row: Row): Record<string, unknown> {
  return {
    clock: row.clock ?? null,
    removed: row.removed_at !== null && row.removed_at !== undefined,
    ota: json(row.ota),
  };
}

/** The unit's firmware history and traces, for GET /api/devices/:id. */
export function otaDeviceHistory(db: Db, deviceId: string): Record<string, unknown> {
  const updates = db.prepare("SELECT seq, ts, payload FROM ledger_entries WHERE device_id = ? AND type = 'fw_update' ORDER BY epoch DESC, seq DESC LIMIT 50")
    .all(deviceId) as Row[];
  const traces = db.prepare('SELECT sha256, seq, samples, period_ms, trigger, received_at FROM traces WHERE device_id = ? ORDER BY received_at DESC LIMIT 50')
    .all(deviceId) as Row[];
  return {
    fw_updates: updates.map((u) => {
      const p = (json(u.payload) ?? {}) as Record<string, unknown>;
      return { seq: u.seq, ts: u.ts, phase: p.phase, version: p.version, from_version: p.from_version, reason: p.reason ?? '' };
    }),
    traces,
  };
}

/**
 * Traces for GET /api/incidents. A trace is closed after its episode ends, so an
 * incident's trace is the first one captured after the incident's end (or its
 * start, while it is open) and before the unit's next incident began.
 */
export function incidentTraces(db: Db, incident: Row): { sha256: string; samples: number; period_ms: number }[] {
  const next = db.prepare('SELECT start_seq FROM incidents WHERE device_id = ? AND epoch = ? AND start_seq > ? ORDER BY start_seq LIMIT 1')
    .get(incident.device_id as string, incident.epoch as number, incident.start_seq as number) as Row | undefined;
  const after = Number(incident.end_seq ?? incident.start_seq);
  return (db.prepare(
    'SELECT sha256, samples, period_ms FROM traces WHERE device_id = ? AND epoch = ? AND seq > ? AND (? IS NULL OR seq < ?) ORDER BY seq LIMIT 1',
  ).all(incident.device_id as string, incident.epoch as number, after, next ? Number(next.start_seq) : null, next ? Number(next.start_seq) : null) as Row[])
    .map((t) => ({ sha256: String(t.sha256), samples: Number(t.samples), period_ms: Number(t.period_ms) }));
}

export function registerOtaRoutes(
  app: FastifyInstance,
  deps: { db: Db; config: GatewayConfig; live: LiveBus; now: () => number; ota?: OtaDeps },
  guards: { viewer: Guard; admin: Guard },
): void {
  const { db, config, now } = deps;
  const { viewer, admin } = guards;
  const unavailable = (reply: FastifyReply) => reply.code(503).send({ error: 'firmware rollouts and unit removal are not running on this gateway' });

  app.addContentTypeParser('application/octet-stream', { parseAs: 'buffer', bodyLimit: FIRMWARE_MAX_BYTES }, (_req, body, done) => done(null, body));

  app.get('/api/firmware', viewer, async () =>
    db.prepare('SELECT sha256, version, project, size, signature, uploaded_by, uploaded_at FROM firmware ORDER BY uploaded_at DESC').all(),
  );

  app.post('/api/firmware', admin, async (request, reply) => {
    if (!Buffer.isBuffer(request.body)) return reply.code(415).send({ error: 'send the image as application/octet-stream' });
    try {
      const image = storeFirmware(db, config, request.body, request.user!.username, now());
      return { sha256: image.sha256, version: image.version, signature: image.signature };
    } catch (err) {
      if (err instanceof FirmwareError) return reply.code(400).send({ error: err.message });
      throw err;
    }
  });

  app.post('/api/firmware/:sha/rollout', admin, async (request, reply) => {
    if (!deps.ota) return unavailable(reply);
    const { sha } = request.params as { sha: string };
    const body = (request.body ?? {}) as { units?: unknown };
    const units = body.units === 'all' ? 'all' : Array.isArray(body.units) ? body.units.map(String) : null;
    if (!units) return reply.code(400).send({ error: 'units is "all" or a list of unit IDs' });
    try {
      return { id: deps.ota.rollouts.start(sha, units, request.user!.username) };
    } catch (err) {
      if (err instanceof RolloutError) return reply.code(409).send({ error: err.message });
      throw err;
    }
  });

  app.get('/api/rollouts', viewer, async (_request, reply) => (deps.ota ? deps.ota.rollouts.list() : unavailable(reply)));

  app.post('/api/rollouts/:id/stop', admin, async (request, reply) => {
    if (!deps.ota) return unavailable(reply);
    const { id } = request.params as { id: string };
    if (!deps.ota.rollouts.stop(Number(id), request.user!.username)) return reply.code(409).send({ error: 'no such rollout running' });
    return { ok: true };
  });

  app.get('/api/traces/:sha', viewer, async (request, reply) => {
    const { sha } = request.params as { sha: string };
    if (!/^[0-9a-f]{64}$/.test(sha)) return reply.code(400).send({ error: 'bad hash' });
    const row = db.prepare('SELECT device_id FROM traces WHERE sha256 = ?').get(sha) as Row | undefined;
    const path = row ? tracePath(config.tracesDir, String(row.device_id), sha) : null;
    if (!row || !path || !existsSync(path)) return reply.code(404).send({ error: 'not on this gateway' });
    audit(db, request.user!.username, 'trace_accessed', { sha256: sha, device_id: row.device_id }, now());
    return reply.type('application/octet-stream').header('content-disposition', `attachment; filename="${sha.slice(0, 16)}.fxt"`).send(readFileSync(path));
  });

  app.delete('/api/devices/:id', admin, async (request, reply) => {
    if (!deps.ota) return unavailable(reply);
    const { id } = request.params as { id: string };
    const result = await removeUnit(id, {
      db, pki: deps.ota.pki, live: deps.live, siteLedger: deps.ota.siteLedger, by: request.user!.username, now: now(), onCrlChanged: deps.ota.reloadCrl,
    });
    if (!result.ok) return reply.code(result.error === 'no such unit' ? 404 : 409).send({ error: result.error });
    return { ok: true, revoked: result.certSerial !== null, restart_broker: true };
  });
}
