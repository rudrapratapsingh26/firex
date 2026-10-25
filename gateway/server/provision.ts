/**
 * The provisioning service (docs/transport.md section 1), independent of HTTP
 * so the node simulator's tests can call it directly.
 */
import { audit, transaction, type Db, type Row } from './db.ts';
import type { LiveBus } from './live.ts';
import {
  checkPossession, formatPairingCode, generatePairingCode, matchPairing, normalisePairingCode,
  parseProvisionRequest, replyMac, type ProvisionReply,
} from './pairing.ts';
import { issueCertificate, rawPointToKey, type Pki } from './pki.ts';

export interface ProvisionContext {
  db: Db;
  pki: Pki;
  live: LiveBus;
  advertisedMqttHost: string;
  advertisedMqttPort: number;
  now?: () => number;
}

const FAILURE_WINDOW_MS = 10 * 60_000;
const MAX_FAILURES = 20;
const failures: number[] = [];

export function resetProvisionRateLimit(): void {
  failures.length = 0;
}

function recordFailure(now: number): void {
  failures.push(now);
  while (failures.length && failures[0] < now - FAILURE_WINDOW_MS) failures.shift();
}

export async function handleProvision(ctx: ProvisionContext, body: unknown, peer = 'unknown'): Promise<{ status: number; body: Record<string, unknown> }> {
  const now = (ctx.now ?? Date.now)();
  while (failures.length && failures[0] < now - FAILURE_WINDOW_MS) failures.shift();
  if (failures.length >= MAX_FAILURES) {
    return { status: 429, body: { error: 'too many failed provisioning attempts; try again later' } };
  }

  const req = parseProvisionRequest(body);
  if (typeof req === 'string') {
    recordFailure(now);
    return { status: 400, body: { error: req } };
  }
  const possession = checkPossession(req);
  if (possession) {
    recordFailure(now);
    audit(ctx.db, peer, 'provision_refused', { device_id: req.device_id, reason: possession }, now);
    return { status: 400, body: { error: possession } };
  }

  const open = ctx.db.prepare('SELECT * FROM pairings WHERE used_at IS NULL AND expires_at > ?').all(now) as Row[];
  const pairing = matchPairing<Row & { code: string }>(req, open.map((row) => ({ ...row, code: String(row.code) })));
  if (!pairing) {
    recordFailure(now);
    audit(ctx.db, peer, 'provision_refused', { device_id: req.device_id, reason: 'no open pairing matches' }, now);
    return { status: 403, body: { error: 'no open pairing matches this unit' } };
  }

  const existing = ctx.db.prepare('SELECT ledger_pub FROM devices WHERE id = ?').get(req.device_id) as Row | undefined;
  if (existing && existing.ledger_pub !== req.ledger_pub) {
    audit(ctx.db, peer, 'provision_refused', { device_id: req.device_id, reason: 'registered with a different ledger key' }, now);
    return { status: 409, body: { error: 'this device is registered with a different ledger key' } };
  }

  const issued = await issueCertificate(ctx.pki, {
    commonName: req.device_id,
    publicKey: await rawPointToKey(req.tls_pub),
    usage: 'client',
  });

  transaction(ctx.db, () => {
    ctx.db.prepare(
      `INSERT INTO devices (id, site_id, label, floor_x, floor_y, ledger_pub, tls_pub, cert_serial, fw_version, provisioned_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
       ON CONFLICT(id) DO UPDATE SET site_id = excluded.site_id, label = excluded.label, floor_x = excluded.floor_x,
         floor_y = excluded.floor_y, tls_pub = excluded.tls_pub, cert_serial = excluded.cert_serial,
         fw_version = excluded.fw_version, provisioned_at = excluded.provisioned_at`,
    ).run(req.device_id, pairing.site_id as string, pairing.label as string, (pairing.floor_x as number | null) ?? null,
      (pairing.floor_y as number | null) ?? null, req.ledger_pub, req.tls_pub, issued.serial, req.fw_version, now);
    ctx.db.prepare('UPDATE pairings SET used_at = ?, device_id = ? WHERE code = ?').run(now, req.device_id, pairing.code);
    audit(ctx.db, peer, 'device_provisioned', { device_id: req.device_id, site_id: pairing.site_id, serial: issued.serial, reprovisioned: Boolean(existing) }, now);
  });

  const reply: Omit<ProvisionReply, 'mac'> = {
    version: 1,
    site_id: String(pairing.site_id),
    device_id: req.device_id,
    mqtt_host: ctx.advertisedMqttHost,
    mqtt_port: ctx.advertisedMqttPort,
    ca_pem: ctx.pki.caCertPem,
    cert_pem: issued.certPem,
  };
  ctx.live.publish({ kind: 'device', device_id: req.device_id });
  return { status: 200, body: { ...reply, mac: replyMac(pairing.code, req.nonce, reply) } };
}

export interface NewPairing {
  code: string;
  site_id: string;
  label: string;
  expires_at: number;
}

export function createPairing(
  db: Db,
  options: { code?: string; siteId: string; label?: string; floorX?: number | null; floorY?: number | null; by: string; ttlHours: number; now?: number },
): NewPairing {
  const now = options.now ?? Date.now();
  const code = normalisePairingCode(options.code ?? generatePairingCode());
  if (!code) throw new Error('a pairing code is ten characters from the label, like 7K3QD-9M2XA');
  const site = db.prepare('SELECT id FROM sites WHERE id = ?').get(options.siteId);
  if (!site) throw new Error(`unknown site ${options.siteId}`);
  const expires = now + options.ttlHours * 3600_000;
  db.prepare(
    `INSERT INTO pairings (code, site_id, label, floor_x, floor_y, created_by, created_at, expires_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
     ON CONFLICT(code) DO UPDATE SET site_id = excluded.site_id, label = excluded.label, floor_x = excluded.floor_x, floor_y = excluded.floor_y,
       created_by = excluded.created_by, created_at = excluded.created_at, expires_at = excluded.expires_at, used_at = NULL, device_id = NULL`,
  ).run(code, options.siteId, options.label ?? '', options.floorX ?? null, options.floorY ?? null, options.by, now, expires);
  audit(db, options.by, 'pairing_created', { site_id: options.siteId, label: options.label ?? '' }, now);
  return { code: formatPairingCode(code), site_id: options.siteId, label: options.label ?? '', expires_at: expires };
}
