/**
 * The gateway's HTTP side.
 *
 *   buildApp          dashboard API, live WebSocket and the dashboard itself (behind Caddy)
 *   buildProvisionApp POST /api/provision only, plain HTTP on the LAN for nodes
 *
 * Every /api route except health and login needs a session; roles are
 * enforced per route. Every image fetch is written to the audit log, because
 * the media policy says access to incident images is logged.
 */
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

import fastifyStatic from '@fastify/static';
import websocket from '@fastify/websocket';
import Fastify, { type FastifyInstance, type FastifyReply, type FastifyRequest } from 'fastify';

import { fromHex } from '../../ledger/src/cbor.ts';
import type { Service, SiteRegistry } from '../escalation/provider.ts';
import { maskRecipient, SERVICES } from '../escalation/registry.ts';
import { checkLogin, createSession, createUser, endSession, roleAllows, ROLES, sessionUser, type Role } from './auth.ts';
import { buildBundle } from './bundle.ts';
import type { GatewayConfig } from './config.ts';
import { audit, type Db, type Row } from './db.ts';
import type { EscalationRunner } from './escalate.ts';
import { incidentTraces, otaDeviceFields, otaDeviceHistory, registerOtaRoutes, type OtaDeps } from './http-ota.ts';
import { mediaPath } from './ingest.ts';
import type { LiveBus } from './live.ts';
import { createPairing, handleProvision, type ProvisionContext } from './provision.ts';
import type { SiteLedger } from './site-ledger.ts';

declare module 'fastify' {
  interface FastifyRequest {
    user?: { username: string; role: Role };
  }
}

export interface AppDeps {
  db: Db;
  config: GatewayConfig;
  live: LiveBus;
  mqttConnected: () => boolean;
  now?: () => number;
  /** The running escalation; null or absent when FIREX_ALERT_PROVIDER is off. */
  escalation?: EscalationRunner | null;
  /** The site's service registry as loaded at start, even when escalation is off. */
  registry?: SiteRegistry | null;
  registrySha256?: string | null;
  siteLedger?: SiteLedger | null;
  /** Firmware rollouts and unit removal (M5); absent in tests that do not use them. */
  ota?: OtaDeps;
}

const COOKIE = 'firex_session';
const ONLINE_MS = 90_000;
const LABELS = ['true', 'nuisance', 'test'];

function cookieToken(request: FastifyRequest): string | undefined {
  const header = request.headers.cookie ?? '';
  for (const part of header.split(';')) {
    const [name, ...rest] = part.trim().split('=');
    if (name === COOKIE) return rest.join('=');
  }
  return undefined;
}

function json(row: Row, field: string): unknown {
  const value = row[field];
  if (value === null || value === undefined) return null;
  try {
    return JSON.parse(String(value));
  } catch {
    return null;
  }
}

function deviceView(row: Row, now: number): Record<string, unknown> {
  const lastSeen = row.last_seen === null ? null : Number(row.last_seen);
  return {
    id: row.id,
    site_id: row.site_id,
    label: row.label,
    floor_x: row.floor_x,
    floor_y: row.floor_y,
    fw_version: row.fw_version,
    boot_count: row.boot_count,
    provisioned_at: row.provisioned_at,
    last_seen: lastSeen,
    online: lastSeen !== null && now - lastSeen < ONLINE_MS,
    state: row.last_state,
    alarm: row.last_alarm,
    faults: json(row, 'faults') ?? {},
    epoch: row.epoch,
    chain_status: row.chain_status,
    last_seq: row.last_seq,
    last_ts: row.last_ts,
    last_self_test: json(row, 'last_self_test'),
    last_calibration_ts: row.last_calibration_ts,
    ...otaDeviceFields(row),
    open_incident: row.open_incident ?? null,
  };
}

function entryView(row: Row): Record<string, unknown> {
  return {
    device_id: row.device_id,
    label: row.label,
    epoch: row.epoch,
    seq: row.seq,
    ts: row.ts,
    ts_src: row.ts_src,
    type: row.type,
    payload: json(row, 'payload'),
    hash: row.hash,
    prev_hash: row.prev_hash,
    received_at: row.received_at,
    verify_result: row.verify_result,
  };
}

function clampInt(value: unknown, fallback: number, min: number, max: number): number {
  const n = Number(value);
  return Number.isFinite(n) ? Math.min(max, Math.max(min, Math.trunc(n))) : fallback;
}

export async function buildApp(deps: AppDeps, options: { serveDashboard?: boolean } = {}): Promise<FastifyInstance> {
  const { db, config, live } = deps;
  const now = deps.now ?? Date.now;
  const app = Fastify({ logger: false, bodyLimit: 256 * 1024, trustProxy: true });
  await app.register(websocket);

  app.addContentTypeParser(/^image\/(png|jpeg|svg\+xml|webp)$/, { parseAs: 'buffer', bodyLimit: 5 * 1024 * 1024 }, (_req, body, done) => done(null, body));

  const need = (role: Role) => async (request: FastifyRequest, reply: FastifyReply) => {
    const user = sessionUser(db, cookieToken(request), now());
    if (!user) return reply.code(401).send({ error: 'sign in first' });
    if (!roleAllows(user.role, role)) return reply.code(403).send({ error: `needs the ${role} role` });
    request.user = user;
  };
  const viewer = { preHandler: need('viewer') };
  const operator = { preHandler: need('operator') };
  const admin = { preHandler: need('admin') };

  app.get('/api/healthz', async () => ({ ok: true, mqtt: deps.mqttConnected() }));

  app.post('/api/login', async (request, reply) => {
    const body = (request.body ?? {}) as { username?: string; password?: string };
    const username = String(body.username ?? '');
    const role = checkLogin(db, username, String(body.password ?? ''));
    if (!role) {
      audit(db, username || 'unknown', 'login_failed', { ip: request.ip }, now());
      return reply.code(401).send({ error: 'wrong username or password' });
    }
    const token = createSession(db, username, now());
    audit(db, username, 'login', { ip: request.ip }, now());
    reply.header('set-cookie', `${COOKIE}=${token}; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=43200`);
    return { username, role };
  });

  app.post('/api/logout', async (request, reply) => {
    endSession(db, cookieToken(request));
    reply.header('set-cookie', `${COOKIE}=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0`);
    return { ok: true };
  });

  app.get('/api/me', viewer, async (request) => request.user);

  app.get('/api/site', viewer, async () => {
    const site = db.prepare('SELECT id, name, address, timezone, floor_plan_type FROM sites WHERE id = ?').get(config.siteId) as Row;
    return { ...site, has_floor_plan: Boolean(site?.floor_plan_type), mqtt_connected: deps.mqttConnected() };
  });

  app.get('/api/site/floor-plan', viewer, async (_request, reply) => {
    const row = db.prepare('SELECT floor_plan, floor_plan_type FROM sites WHERE id = ?').get(config.siteId) as Row | undefined;
    if (!row?.floor_plan) return reply.code(404).send({ error: 'no floor plan uploaded' });
    return reply.type(String(row.floor_plan_type)).send(Buffer.from(row.floor_plan as Uint8Array));
  });

  app.put('/api/site/floor-plan', operator, async (request, reply) => {
    if (!Buffer.isBuffer(request.body)) return reply.code(415).send({ error: 'send a PNG, JPEG, WebP or SVG image' });
    db.prepare('UPDATE sites SET floor_plan = ?, floor_plan_type = ? WHERE id = ?')
      .run(request.body, String(request.headers['content-type']), config.siteId);
    audit(db, request.user!.username, 'floor_plan_updated', { bytes: request.body.length }, now());
    return { ok: true };
  });

  /* When an open heartbeat gap started (heartbeat.ts): the unit has been silent since then. */
  const OFFLINE_SINCE = '(SELECT g.start_ms FROM heartbeat_gaps g WHERE g.device_id = d.id AND g.end_ms IS NULL ORDER BY g.id DESC LIMIT 1) AS offline_since';

  app.get('/api/devices', viewer, async () => {
    const rows = db.prepare(
      `SELECT d.*, (SELECT id FROM incidents i WHERE i.device_id = d.id AND i.end_seq IS NULL ORDER BY id DESC LIMIT 1) AS open_incident, ${OFFLINE_SINCE}
       FROM devices d ORDER BY d.label, d.id`,
    ).all() as Row[];
    const t = now();
    return rows.map((row) => ({ ...deviceView(row, t), offline_since: row.offline_since ?? null }));
  });

  app.get('/api/devices/:id', viewer, async (request, reply) => {
    const { id } = request.params as { id: string };
    const row = db.prepare(
      `SELECT d.*, (SELECT id FROM incidents i WHERE i.device_id = d.id AND i.end_seq IS NULL ORDER BY id DESC LIMIT 1) AS open_incident, ${OFFLINE_SINCE}
       FROM devices d WHERE d.id = ?`,
    ).get(id) as Row | undefined;
    if (!row) return reply.code(404).send({ error: 'no such device' });
    const chainEvents = db.prepare('SELECT * FROM chain_events WHERE device_id = ? ORDER BY id DESC LIMIT 50').all(id);
    const counts = db.prepare('SELECT verify_result, COUNT(*) AS n FROM ledger_entries WHERE device_id = ? AND epoch = ? GROUP BY verify_result')
      .all(id, row.epoch as number) as Row[];
    const t = now();
    const gaps = db.prepare('SELECT start_ms, end_ms FROM heartbeat_gaps WHERE device_id = ? ORDER BY id DESC LIMIT 50').all(id) as Row[];
    return {
      ...deviceView(row, t),
      offline_since: row.offline_since ?? null,
      ...otaDeviceHistory(db, id),
      chain_events: chainEvents,
      verify_counts: Object.fromEntries(counts.map((c) => [c.verify_result, c.n])),
      gaps: gaps.map((g) => ({
        start_ms: Number(g.start_ms),
        end_ms: g.end_ms === null ? null : Number(g.end_ms),
        seconds: Math.round(((g.end_ms === null ? t : Number(g.end_ms)) - Number(g.start_ms)) / 1000),
      })),
    };
  });

  app.patch('/api/devices/:id', operator, async (request, reply) => {
    const { id } = request.params as { id: string };
    const body = (request.body ?? {}) as { label?: string; floor_x?: number | null; floor_y?: number | null };
    const inRange = (v: unknown) => v === null || v === undefined || (typeof v === 'number' && v >= 0 && v <= 1);
    if (!inRange(body.floor_x) || !inRange(body.floor_y) || (body.label !== undefined && String(body.label).length > 64)) {
      return reply.code(400).send({ error: 'label is at most 64 characters; floor_x and floor_y are 0-1' });
    }
    const info = db.prepare(
      'UPDATE devices SET label = COALESCE(?, label), floor_x = COALESCE(?, floor_x), floor_y = COALESCE(?, floor_y) WHERE id = ?',
    ).run(body.label ?? null, body.floor_x ?? null, body.floor_y ?? null, id);
    if (!info.changes) return reply.code(404).send({ error: 'no such device' });
    audit(db, request.user!.username, 'device_updated', { device_id: id, ...body }, now());
    live.publish({ kind: 'device', device_id: id });
    return { ok: true };
  });

  app.get('/api/devices/:id/telemetry', viewer, async (request) => {
    const { id } = request.params as { id: string };
    const minutes = clampInt((request.query as Row).minutes, 60, 1, 60 * 24 * 7);
    const from = Math.floor(now() / 60000) - minutes;
    const rows = db.prepare('SELECT minute, scores, fan FROM telemetry WHERE device_id = ? AND minute >= ? ORDER BY minute').all(id, from) as Row[];
    return rows.map((r) => ({ at: Number(r.minute) * 60000, scores: json(r, 'scores'), fan: json(r, 'fan') }));
  });

  app.get('/api/devices/:id/ledger', viewer, async (request, reply) => {
    const { id } = request.params as { id: string };
    const q = request.query as Row;
    const device = db.prepare('SELECT epoch FROM devices WHERE id = ?').get(id) as Row | undefined;
    if (!device) return reply.code(404).send({ error: 'no such device' });
    const epoch = clampInt(q.epoch, Number(device.epoch), 0, 1_000_000);
    const before = clampInt(q.before_seq, Number.MAX_SAFE_INTEGER, 0, Number.MAX_SAFE_INTEGER);
    const limit = clampInt(q.limit, 100, 1, 500);
    const rows = db.prepare(
      `SELECT e.*, d.label FROM ledger_entries e JOIN devices d ON d.id = e.device_id
       WHERE e.device_id = ? AND e.epoch = ? AND e.seq < ? ORDER BY e.seq DESC LIMIT ?`,
    ).all(id, epoch, before, limit) as Row[];
    return rows.map(entryView);
  });

  app.get('/api/devices/:id/bundle', viewer, async (request, reply) => {
    const { id } = request.params as { id: string };
    const q = request.query as Row;
    const bundle = buildBundle(db, config.mediaDir, id, {
      epoch: q.epoch === undefined ? undefined : clampInt(q.epoch, 0, 0, 1_000_000),
      fromSeq: q.from_seq === undefined ? undefined : clampInt(q.from_seq, 0, 0, Number.MAX_SAFE_INTEGER),
      toSeq: q.to_seq === undefined ? undefined : clampInt(q.to_seq, 0, 0, Number.MAX_SAFE_INTEGER),
    });
    if (!bundle) return reply.code(404).send({ error: 'no such device' });
    audit(db, request.user!.username, 'bundle_exported', { device_id: id, entries: bundle.log.length }, now());
    reply.header('content-disposition', `attachment; filename="firex-${id.slice(0, 8)}-epoch${bundle.epoch}.json"`);
    return bundle;
  });

  app.get('/api/timeline', viewer, async (request) => {
    const q = request.query as Row;
    const limit = clampInt(q.limit, 100, 1, 500);
    const before = clampInt(q.before, Number.MAX_SAFE_INTEGER, 0, Number.MAX_SAFE_INTEGER);
    const types = typeof q.types === 'string' && q.types ? String(q.types).split(',').slice(0, 20) : null;
    const rows = db.prepare(
      `SELECT e.*, d.label FROM ledger_entries e JOIN devices d ON d.id = e.device_id
       WHERE e.received_at < ? ${types ? `AND e.type IN (${types.map(() => '?').join(',')})` : ''}
       ORDER BY e.received_at DESC, e.seq DESC LIMIT ?`,
    ).all(before, ...(types ?? []), limit) as Row[];
    return rows.map(entryView);
  });

  app.get('/api/incidents', viewer, async (request) => {
    const limit = clampInt((request.query as Row).limit, 50, 1, 200);
    const rows = db.prepare(
      'SELECT i.*, d.label AS device_label FROM incidents i JOIN devices d ON d.id = i.device_id ORDER BY i.id DESC LIMIT ?',
    ).all(limit) as Row[];
    return rows.map((row) => {
      const frames = db.prepare(
        `SELECT r.sha256, r.seq, m.size, m.received_at, m.deleted_at FROM media_refs r
         JOIN ledger_entries e ON e.device_id = r.device_id AND e.epoch = r.epoch AND e.seq = r.seq
         LEFT JOIN media m ON m.sha256 = r.sha256
         WHERE r.device_id = ? AND r.epoch = ? AND r.seq >= ? AND (? IS NULL OR r.seq <= ?)
         ORDER BY r.seq, r.idx`,
      ).all(row.device_id as string, row.epoch as number, row.start_seq as number, row.end_seq as number | null, row.end_seq as number | null) as Row[];
      const alerts = db.prepare('SELECT id, at_ms, type, service, receipt_id, payload FROM alert_events WHERE incident_id = ? ORDER BY at_ms, id')
        .all(row.id as number) as Row[];
      return {
        ...row,
        reason: json(row, 'reason'),
        late: Boolean(row.late),
        escalation: {
          state: row.escalation_state ?? 'none',
          ...(row.escalation_reason ? { reason: row.escalation_reason } : {}),
        },
        traces: incidentTraces(db, row),
        alerts: alerts.map((a) => ({ ...a, payload: json(a, 'payload') })),
        frames: frames.map((f) => ({
          sha256: f.sha256,
          seq: f.seq,
          /* on the gateway, hash checked on arrival, not yet deleted by retention */
          available: f.size !== null && f.deleted_at === null,
          deleted: f.deleted_at !== null,
        })),
      };
    });
  });

  app.post('/api/incidents/:id/label', operator, async (request, reply) => {
    const { id } = request.params as { id: string };
    const body = (request.body ?? {}) as { label?: string; note?: string };
    if (!LABELS.includes(String(body.label))) return reply.code(400).send({ error: 'label is true, nuisance or test' });
    const info = db.prepare('UPDATE incidents SET label = ?, note = ?, labelled_by = ?, labelled_at = ? WHERE id = ?')
      .run(String(body.label), String(body.note ?? '').slice(0, 500), request.user!.username, now(), Number(id));
    if (!info.changes) return reply.code(404).send({ error: 'no such incident' });
    audit(db, request.user!.username, 'incident_labelled', { incident: Number(id), label: body.label }, now());
    if (deps.siteLedger) {
      const row = db.prepare(
        `SELECT i.device_id, i.start_seq, e.hash FROM incidents i
         LEFT JOIN ledger_entries e ON e.device_id = i.device_id AND e.epoch = i.epoch AND e.seq = i.start_seq WHERE i.id = ?`,
      ).get(Number(id)) as Row;
      try {
        deps.siteLedger.append('incident_label', {
          unit: String(row.device_id), incident: Number(id), critical_seq: Number(row.start_seq),
          critical_hash: fromHex(row.hash ? String(row.hash) : '0'.repeat(64)), label: String(body.label), by: request.user!.username,
        });
      } catch (err) {
        audit(db, 'gateway', 'site_ledger_write_failed', { type: 'incident_label', error: String(err) }, now());
      }
    }
    return { ok: true };
  });

  app.post('/api/incidents/:id/ack', operator, async (request, reply) => {
    const id = Number((request.params as { id: string }).id);
    if (!db.prepare('SELECT id FROM incidents WHERE id = ?').get(id)) return reply.code(404).send({ error: 'no such incident' });
    if (!deps.escalation) return reply.code(409).send({ error: 'escalation is off on this gateway' });
    const recorded = deps.escalation.acknowledgeSite(id, request.user!.username);
    if (!recorded) return reply.code(409).send({ error: 'the site was not alerted for this incident, or has already acknowledged' });
    audit(db, request.user!.username, 'incident_acknowledged', { incident: id }, now());
    return { ok: true, recorded };
  });

  const drillView = (row: Row) => ({
    id: row.id,
    started_at_ms: row.started_at_ms,
    finished_at_ms: row.finished_at_ms,
    by: row.by,
    services: json(row, 'services') ?? [],
    alerts: (db.prepare('SELECT id, at_ms, type, service, receipt_id, payload FROM drill_events WHERE drill_id = ? ORDER BY at_ms, id').all(row.id as number) as Row[])
      .map((a) => ({ ...a, payload: json(a, 'payload') })),
  });

  app.get('/api/drills', viewer, async (request) => {
    const limit = clampInt((request.query as Row).limit, 50, 1, 200);
    return (db.prepare('SELECT * FROM drills ORDER BY id DESC LIMIT ?').all(limit) as Row[]).map(drillView);
  });

  app.post('/api/drills', admin, async (request, reply) => {
    const body = (request.body ?? {}) as { services?: unknown };
    const services = Array.isArray(body.services) ? body.services.map(String) : [];
    if (!services.length || services.some((s) => !SERVICES.includes(s as Service))) {
      return reply.code(400).send({ error: 'services is a list drawn from fire, police, ambulance and site' });
    }
    if (!deps.escalation) return reply.code(409).send({ error: 'escalation is off on this gateway (FIREX_ALERT_PROVIDER)' });
    const id = deps.escalation.startDrill(services as Service[], request.user!.username);
    audit(db, request.user!.username, 'drill_started', { drill: id, services }, now());
    return { id };
  });

  app.get('/api/registry', viewer, async (_request, reply) => {
    const registry = deps.registry;
    if (!registry) return reply.code(404).send({ error: 'no service registry is loaded (FIREX_SERVICE_REGISTRY)' });
    return {
      site_id: registry.site_id,
      site_name: registry.site_name,
      version: registry.version ?? 1,
      ack_timeout_s: registry.ack_timeout_s ?? config.alertAckTimeoutS,
      max_realerts: registry.max_realerts ?? config.alertMaxReAlerts,
      offline_notify_min: registry.offline_notify_min ?? 10,
      sha256: deps.registrySha256 ?? null,
      contacts: registry.contacts.map((c) => ({
        service: c.service,
        name: c.name,
        channel: c.channel,
        recipient_masked: maskRecipient(c.recipient),
        mode: c.mode ?? 'drill',
        agreement: c.agreement ?? null,
        ...(c.backup ? { backup: { name: c.backup.name, channel: c.backup.channel, recipient_masked: maskRecipient(c.backup.recipient) } } : {}),
      })),
    };
  });

  app.get('/api/site/ledger', viewer, async (request, reply) => {
    if (!deps.siteLedger) return reply.code(404).send({ error: 'no site ledger on this gateway' });
    const q = request.query as Row;
    return deps.siteLedger.entries({
      limit: clampInt(q.limit, 100, 1, 500),
      beforeSeq: q.before_seq === undefined ? undefined : clampInt(q.before_seq, 0, 0, Number.MAX_SAFE_INTEGER),
    });
  });

  app.get('/api/site/ledger/bundle', viewer, async (request, reply) => {
    if (!deps.siteLedger) return reply.code(404).send({ error: 'no site ledger on this gateway' });
    const bundle = deps.siteLedger.bundle();
    audit(db, request.user!.username, 'site_ledger_exported', { entries: bundle.log.length }, now());
    reply.header('content-disposition', `attachment; filename="firex-site-${config.siteId}.json"`);
    return bundle;
  });

  app.get('/api/media/:sha', viewer, async (request, reply) => {
    const { sha } = request.params as { sha: string };
    if (!/^[0-9a-f]{64}$/.test(sha)) return reply.code(400).send({ error: 'bad hash' });
    const row = db.prepare('SELECT device_id, deleted_at FROM media WHERE sha256 = ?').get(sha) as Row | undefined;
    if (!row || row.deleted_at !== null) return reply.code(404).send({ error: 'not on this gateway' });
    const path = mediaPath(config.mediaDir, String(row.device_id), sha);
    if (!existsSync(path)) return reply.code(404).send({ error: 'not on this gateway' });
    audit(db, request.user!.username, 'media_accessed', { sha256: sha, device_id: row.device_id }, now());
    return reply.type('image/jpeg').header('cache-control', 'private, max-age=300').send(readFileSync(path));
  });

  app.get('/api/pairings', operator, async () =>
    db.prepare('SELECT code, site_id, label, floor_x, floor_y, created_by, created_at, expires_at, used_at, device_id FROM pairings ORDER BY created_at DESC LIMIT 100').all(),
  );

  app.post('/api/pairings', operator, async (request, reply) => {
    const body = (request.body ?? {}) as { code?: string; label?: string; floor_x?: number; floor_y?: number };
    try {
      return createPairing(db, {
        code: body.code, siteId: config.siteId, label: String(body.label ?? '').slice(0, 64),
        floorX: body.floor_x ?? null, floorY: body.floor_y ?? null, by: request.user!.username, ttlHours: config.pairingTtlHours, now: now(),
      });
    } catch (err) {
      return reply.code(400).send({ error: (err as Error).message });
    }
  });

  app.get('/api/audit', admin, async (request) => {
    const limit = clampInt((request.query as Row).limit, 200, 1, 1000);
    return (db.prepare('SELECT * FROM audit ORDER BY id DESC LIMIT ?').all(limit) as Row[]).map((r) => ({ ...r, detail: json(r, 'detail') }));
  });

  app.get('/api/users', admin, async () => db.prepare('SELECT username, role, created_at FROM users ORDER BY username').all());

  app.post('/api/users', admin, async (request, reply) => {
    const body = (request.body ?? {}) as { username?: string; password?: string; role?: string };
    if (!ROLES.includes(body.role as Role)) return reply.code(400).send({ error: 'role is viewer, operator or admin' });
    try {
      createUser(db, String(body.username ?? ''), String(body.password ?? ''), body.role as Role, now());
    } catch (err) {
      return reply.code(400).send({ error: (err as Error).message });
    }
    audit(db, request.user!.username, 'user_created', { username: body.username, role: body.role }, now());
    return { ok: true };
  });

  registerOtaRoutes(app, { db, config, live, now, ota: deps.ota }, { viewer, admin });

  app.get('/api/live', { websocket: true }, (socket, request) => {
    const user = sessionUser(db, cookieToken(request), now());
    if (!user) {
      socket.close(4401, 'sign in first');
      return;
    }
    const unsubscribe = live.subscribe((event) => {
      if (socket.readyState === socket.OPEN) socket.send(JSON.stringify(event));
    });
    socket.on('close', unsubscribe);
  });

  if (options.serveDashboard && existsSync(join(config.dashboardDir, 'index.html'))) {
    await app.register(fastifyStatic, { root: config.dashboardDir, wildcard: false });
    app.setNotFoundHandler((request, reply) => {
      if (request.method === 'GET' && !request.url.startsWith('/api/')) return reply.sendFile('index.html');
      return reply.code(404).send({ error: 'not found' });
    });
  }

  return app;
}

export async function buildProvisionApp(ctx: ProvisionContext): Promise<FastifyInstance> {
  const app = Fastify({ logger: false, bodyLimit: 16 * 1024 });
  app.post('/api/provision', async (request, reply) => {
    const result = await handleProvision(ctx, request.body, request.ip);
    return reply.code(result.status).send(result.body);
  });
  app.get('/api/healthz', async () => ({ ok: true }));
  return app;
}
