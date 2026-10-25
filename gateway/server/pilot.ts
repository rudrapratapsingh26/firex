/**
 * The pilot export: one site's record over a period, in files an evaluator can
 * check without the gateway, for tools/pilot to report on.
 *
 *   <out>/pilot.json                 units, incidents with their labels and alerts, drills, gaps, updates
 *   <out>/site-ledger.json           the site chain, whole (a chain verifies only whole)
 *   <out>/bundles/<unit>.json        each unit's chain for its current epoch, whole
 *   <out>/traces/<sha>.csv, .events.jsonl   episode traces in the M4 capture format, for replay
 *
 * The period selects what pilot.json counts: unit entries by when the gateway
 * received them (a unit's own ts may be `uncertain`), gateway rows by their own
 * millisecond times. The chains are exported whole regardless, so the counts can
 * be re-derived from signed records. No photos are exported, only their hashes
 * inside the unit bundles (docs/media-policy.md). Phone numbers keep their last
 * four digits.
 */
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

import { entryFromJson, publicKeyFromPem, type EntryJson } from '../../ledger/src/entry.ts';
import { describe, verifyLog } from '../../ledger/src/verify.ts';
import { maskRecipient } from '../escalation/registry.ts';
import { buildBundle } from './bundle.ts';
import type { Db, Row } from './db.ts';
import type { SiteLedger } from './site-ledger.ts';
import { traceToCapture } from './traces.ts';

export interface PilotExportOptions {
  db: Db;
  siteId: string;
  siteLedger: SiteLedger;
  mediaDir: string;
  tracesDir: string;
  registryPath: string | null;
  fromMs: number;
  toMs: number;
  out: string;
  now?: () => number;
}

export interface AlertSummary {
  service: string;
  channel: string | null;
  recipient_masked: string | null;
  drill: boolean;
  sent_at_ms: number | null;
  delivered_at_ms: number | null;
  acked_at_ms: number | null;
  acked_by: string | null;
  latency_s: number | null;
  timeouts: number;
  re_alerts: number;
  failed: number;
}

function parse(row: Row, field = 'payload'): Record<string, unknown> {
  try {
    return JSON.parse(String(row[field] ?? '{}')) as Record<string, unknown>;
  } catch {
    return {};
  }
}

function verifyText(log: EntryJson[], pem: string): string {
  if (!log.length) return 'empty';
  return describe(verifyLog(log.map(entryFromJson), publicKeyFromPem(pem)));
}

/** One summary per service, in the order the services were first alerted. */
export function summariseAlerts(rows: Row[], drill: boolean): AlertSummary[] {
  const services = [...new Set(rows.map((r) => String(r.service)))];
  return services.map((service) => {
    const of = (type: string) => rows.filter((r) => r.service === service && r.type === type);
    const sent = of('alert_sent');
    const first = sent[0];
    const firstPayload = first ? parse(first) : {};
    const failed = of('alert_failed');
    const ack = of('alert_ack')[0];
    const ackPayload = ack ? parse(ack) : {};
    const recipient = firstPayload.recipient ?? (failed[0] ? parse(failed[0]).recipient : null);
    return {
      service,
      channel: String(firstPayload.channel ?? (failed[0] ? parse(failed[0]).channel : '')) || null,
      recipient_masked: recipient ? maskRecipient(String(recipient)) : null,
      drill,
      sent_at_ms: first ? Number(first.at_ms) : null,
      delivered_at_ms: typeof firstPayload.delivered_at_ms === 'number' ? firstPayload.delivered_at_ms : null,
      acked_at_ms: ack ? Number(ack.at_ms) : null,
      acked_by: ack ? String(ackPayload.by ?? '') || null : null,
      latency_s: typeof ackPayload.latency_s === 'number' ? ackPayload.latency_s : null,
      timeouts: of('alert_timeout').length,
      re_alerts: sent.filter((r) => parse(r).retry_of).length,
      failed: failed.length,
    };
  });
}

function registrySummary(path: string | null): { sha256: string | null; services: Record<string, { mode: string; agreement: unknown }> } {
  if (!path || !existsSync(path)) return { sha256: null, services: {} };
  const bytes = readFileSync(path);
  const doc = JSON.parse(bytes.toString('utf8')) as { contacts?: { service: string; mode?: string; agreement?: unknown }[] };
  const services: Record<string, { mode: string; agreement: unknown }> = {};
  for (const c of doc.contacts ?? []) services[c.service] = { mode: c.mode ?? 'drill', agreement: c.agreement ?? null };
  return { sha256: createHash('sha256').update(bytes).digest('hex'), services };
}

function hasTable(db: Db, name: string): boolean {
  return Boolean(db.prepare("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?").get(name));
}

export function exportPilot(options: PilotExportOptions): Record<string, unknown> {
  const { db, fromMs, toMs, out } = options;
  const now = options.now ?? Date.now;
  mkdirSync(join(out, 'bundles'), { recursive: true });
  mkdirSync(join(out, 'traces'), { recursive: true });

  const site = db.prepare('SELECT id, name FROM sites WHERE id = ?').get(options.siteId) as Row | undefined;
  const inPeriod = 'received_at >= ? AND received_at <= ?';

  /* Traces first: their conversion finds the incident each one belongs to. */
  const traceRows = db.prepare(`SELECT * FROM traces WHERE ${inPeriod} ORDER BY received_at`).all(fromMs, toMs) as Row[];
  const traces = traceRows.map((t) => {
    const sha = String(t.sha256);
    const capture = traceToCapture(db, options.tracesDir, sha);
    writeFileSync(join(out, 'traces', `${sha}.csv`), capture.csv);
    writeFileSync(join(out, 'traces', `${sha}.events.jsonl`), capture.events);
    return {
      sha256: sha, unit: String(t.device_id), incident: capture.incidentId,
      csv: `traces/${sha}.csv`, events: `traces/${sha}.events.jsonl`,
      samples: Number(t.samples), period_ms: Number(t.period_ms), trigger: String(t.trigger),
      transitions: capture.transitions, matched: capture.matched,
    };
  });

  const devices = db.prepare('SELECT * FROM devices ORDER BY label, id').all() as Row[];
  const gapsTable = hasTable(db, 'heartbeat_gaps');
  const units = devices.map((d) => {
    const id = String(d.id);
    const entries = (type: string) => db.prepare(`SELECT seq, ts, payload FROM ledger_entries WHERE device_id = ? AND type = ? AND ${inPeriod} ORDER BY epoch, seq`)
      .all(id, type, fromMs, toMs) as Row[];
    const start = Math.max(fromMs, Number(d.provisioned_at));
    const end = Math.min(toMs, d.removed_at === null || d.removed_at === undefined ? toMs : Number(d.removed_at));
    const gaps = gapsTable
      ? (db.prepare('SELECT start_ms, end_ms FROM heartbeat_gaps WHERE device_id = ? AND start_ms < ? AND COALESCE(end_ms, ?) > ? ORDER BY start_ms')
        .all(id, end, end, start) as Row[]).map((g) => {
        const s = Math.max(Number(g.start_ms), start);
        const e = Math.min(g.end_ms === null ? end : Number(g.end_ms), end);
        return { start_ms: s, end_ms: e, seconds: Math.max(0, Math.round((e - s) / 1000)) };
      })
      : [];
    const selfTests = entries('self_test').map((r) => parse(r).result);
    const boots = entries('boot').map((r) => String(parse(r).fw_version ?? ''));
    return {
      id,
      label: String(d.label ?? ''),
      provisioned_at: Number(d.provisioned_at),
      removed_at: d.removed_at === null || d.removed_at === undefined ? null : Number(d.removed_at),
      fw_versions: [...new Set([...boots, String(d.fw_version ?? '')].filter(Boolean))],
      monitored_s: Math.max(0, Math.round((end - start) / 1000)),
      gap_s: gaps.reduce((sum, g) => sum + g.seconds, 0),
      gaps,
      self_tests: { pass: selfTests.filter((r) => r === 'pass').length, fail: selfTests.filter((r) => r !== 'pass').length },
      faults_raised: entries('fault').filter((r) => parse(r).state === 'raised').length,
      fw_updates: entries('fw_update').map((r) => {
        const p = parse(r);
        return { ts: Number(r.ts), phase: p.phase, version: p.version, from_version: p.from_version, reason: p.reason ?? '' };
      }),
      elevated_episodes: entries('state_change').filter((r) => parse(r).from === 'NORMAL' && parse(r).to === 'ELEVATED').length,
    };
  });

  const incidentRows = db.prepare(
    `SELECT i.*, e.ts_src FROM incidents i JOIN ledger_entries e ON e.device_id = i.device_id AND e.epoch = i.epoch AND e.seq = i.start_seq
     WHERE e.received_at >= ? AND e.received_at <= ? ORDER BY i.id`,
  ).all(fromMs, toMs) as Row[];
  const incidents = incidentRows.map((i) => {
    const alerts = hasTable(db, 'alert_events')
      ? db.prepare('SELECT * FROM alert_events WHERE incident_id = ? ORDER BY at_ms, id').all(Number(i.id)) as Row[]
      : [];
    return {
      id: Number(i.id),
      unit: String(i.device_id),
      start_ts: Number(i.start_ts),
      end_ts: i.end_ts === null ? null : Number(i.end_ts),
      ts_src: String(i.ts_src),
      reason: JSON.parse(String(i.reason ?? '[]')),
      label: i.label ?? null,
      labelled_by: i.labelled_by ?? null,
      labelled_at_ms: i.labelled_at === null || i.labelled_at === undefined ? null : Number(i.labelled_at),
      late: Boolean(i.late),
      escalation_state: i.escalation_state ?? null,
      traces: traces.filter((t) => t.incident === Number(i.id)).map((t) => t.sha256),
      alerts: summariseAlerts(alerts, false),
    };
  });

  const drills = hasTable(db, 'drills')
    ? (db.prepare('SELECT * FROM drills WHERE started_at_ms >= ? AND started_at_ms <= ? ORDER BY id').all(fromMs, toMs) as Row[]).map((d) => ({
      id: Number(d.id),
      started_at_ms: Number(d.started_at_ms),
      finished_at_ms: d.finished_at_ms === null ? null : Number(d.finished_at_ms),
      by: String(d.by),
      services: JSON.parse(String(d.services ?? '[]')),
      alerts: summariseAlerts(db.prepare('SELECT * FROM drill_events WHERE drill_id = ? ORDER BY at_ms, id').all(Number(d.id)) as Row[], true),
    }))
    : [];

  const siteBundle = options.siteLedger.bundle();
  writeFileSync(join(out, 'site-ledger.json'), `${JSON.stringify(siteBundle, null, 2)}\n`);
  const unitBundles = devices.map((d) => {
    const bundle = buildBundle(db, options.mediaDir, String(d.id))!;
    const file = `bundles/${d.id}.json`;
    writeFileSync(join(out, file), `${JSON.stringify(bundle, null, 2)}\n`);
    return { unit: String(d.id), file, entries: bundle.log.length, verify: verifyText(bundle.log, bundle.public_key_pem) };
  });

  const doc = {
    kind: 'pilot_site',
    synthetic: false,
    generated_at: new Date(now()).toISOString(),
    site: { id: options.siteId, name: String(site?.name ?? '') },
    period: { from_ms: fromMs, to_ms: toMs },
    registry: registrySummary(options.registryPath),
    units,
    incidents,
    drills,
    site_ledger: { entries: siteBundle.log.length, verify: verifyText(siteBundle.log, siteBundle.public_key_pem), device_id: siteBundle.device_id, file: 'site-ledger.json' },
    unit_bundles: unitBundles,
    traces,
  };
  writeFileSync(join(out, 'pilot.json'), `${JSON.stringify(doc, null, 2)}\n`);
  return doc;
}
