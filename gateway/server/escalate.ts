/**
 * The escalation runner: when ingest opens an incident, alert the site and the
 * services the site's registry connects to real incidents, and follow up until
 * each one has answered or been escalated (docs/service-registry.md).
 *
 * It starts from the verified ledger entry, not the unsigned state message:
 * the alert names the CRITICAL entry's sequence number and hash, and a
 * CRITICAL entry whose hash or signature failed on arrival (`chain`,
 * `signature`) never alerts anyone. It runs beside ingest, never inside it:
 * ingest has stored and acknowledged the entry before the first send.
 *
 * Who is alerted (M5):
 *   a CRITICAL as it happens   the site, and every service whose mode is live
 *   a late CRITICAL            the site only: it came in from a unit's backlog, already over
 *   a drill                    the contacts the drill names, whatever their mode, marked DRILL
 *
 * Every send, acknowledgement and timeout goes into alert_events (drill_events
 * for a drill) with millisecond times, and into the site ledger as a signed
 * alert_sent, alert_ack or alert_timeout entry that names the unit's CRITICAL
 * entry by seq and hash. A failed send is recorded in the database only.
 *
 * Escalations survive a gateway restart: at start the runner picks up every
 * incident still marked running and every unfinished drill from the last hour,
 * and carries on from the rows it wrote. Acknowledgements arrive three ways:
 * the provider's own check, a reply the provider hears (an SMS saying 1, OK or
 * ACK, or a key pressed during a call), and the site's operator on the dashboard.
 */
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';

import { fromHex, type CborValue } from '../../ledger/src/cbor.ts';
import type { EventType } from '../../ledger/src/entry.ts';
import { ConsoleAlert } from '../escalation/console-alert.ts';
import { EscalationService, type LedgerEvent, type SendFailure, type ServiceOutcome } from '../escalation/escalation.ts';
import { HttpAlert } from '../escalation/http-alert.ts';
import { AtModem, ModemAlert, openSerialPort, type ModemPort } from '../escalation/modem.ts';
import type { Ack, Alert, AlertProvider, Receipt, Reply, SendContext, Service, ServiceContact, SiteRegistry } from '../escalation/provider.ts';
import { isPhoneNumber, loadRegistry, maskRecipient, phoneKey, SERVICES, siteContact } from '../escalation/registry.ts';
import type { GatewayConfig } from './config.ts';
import { audit, type Db, type Row } from './db.ts';
import type { LiveBus } from './live.ts';
import type { SiteLedger } from './site-ledger.ts';

export type AlertEventType = 'alert_sent' | 'alert_ack' | 'alert_timeout' | 'alert_failed';
export type EscalationState = 'none' | 'running' | 'finished' | 'withheld';

interface Subject {
  kind: 'incident' | 'drill';
  id: number;
}

/** What the site ledger needs to tie an alert to the unit entry it is about. */
interface Tie {
  unit: string;
  incident: number;
  critical_seq: number;
  critical_hash: string;
  drill: boolean;
  incident_id: string;
}

interface EventRow {
  id: number;
  at_ms: number;
  type: string;
  service: string;
  receipt_id: string | null;
  payload: Record<string, unknown>;
}

interface Session {
  subject: Subject;
  outcomes: ServiceOutcome[];
}

const ZERO_HASH = '0'.repeat(64);
/** Verification results that may alert: a chain first seen mid-way, or after a gap, is still genuine. */
const MAY_ESCALATE = new Set(['ok', 'start', 'gap']);
const ACK_WORDS = /^(1|ok|ack)$/i;
const DRILL_PICKUP_MS = 2000;

const keyOf = (s: Subject) => `${s.kind}:${s.id}`;
const whole = (value: unknown) => Math.max(0, Math.round(Number(value) || 0));
const errorText = (err: unknown) => (err instanceof Error ? err.message : String(err));

/** Acknowledgements the runner learned itself (the dashboard, replies), checked before the provider's own. */
class BookedAcks implements AlertProvider {
  readonly name: string;
  private readonly inner: AlertProvider;
  private readonly booked = new Map<string, Ack>();

  constructor(inner: AlertProvider) {
    this.inner = inner;
    this.name = inner.name;
  }

  send(alert: Alert, contact: ServiceContact, context?: SendContext): Promise<Receipt> {
    return this.inner.send(alert, contact, context);
  }

  async ack(receiptId: string): Promise<Ack | null> {
    return this.booked.get(receiptId) ?? this.inner.ack(receiptId);
  }

  onReply(listener: (reply: Reply) => void): void {
    this.inner.onReply?.(listener);
  }

  book(ack: Ack): void {
    this.booked.set(ack.receipt_id, ack);
  }
}

/** Queue a drill for whichever gateway process runs escalation; it starts within a few seconds. */
export function queueDrill(db: Db, services: Service[], by: string, now = Date.now()): number {
  const chosen = [...new Set(services)];
  if (!chosen.length) throw new Error('a drill names at least one service');
  for (const s of chosen) if (!SERVICES.includes(s)) throw new Error(`${s} is not a service; use fire, police, ambulance or site`);
  const info = db.prepare('INSERT INTO drills (started_at_ms, by, services) VALUES (?, ?, ?)').run(now, by, JSON.stringify(chosen));
  return Number(info.lastInsertRowid);
}

export interface EscalationRunnerOptions {
  db: Db;
  live: LiveBus;
  registry: SiteRegistry;
  provider: AlertProvider;
  /** Where alert_* and escalation_withheld entries are signed; without one only the database records them. */
  siteLedger?: SiteLedger | null;
  ackTimeoutS?: number;
  maxReAlerts?: number;
  pollMs?: number;
  /** Unix milliseconds. */
  now?: () => number;
  log?: (line: string) => void;
  /** How long to wait for a unit's backlog to arrive before deciding whether its alarm is late. */
  drainWaitMs?: number;
  /** A CRITICAL older than this, already over when it arrived, is late. */
  lateAfterS?: number;
  /** Escalations and drills older than this are not resumed after a restart. */
  resumeWindowMs?: number;
}

export class EscalationRunner {
  private readonly db: Db;
  private readonly live: LiveBus;
  private readonly registry: SiteRegistry;
  private readonly provider: BookedAcks;
  private readonly siteLedger: SiteLedger | null;
  private readonly ackTimeoutS: number;
  private readonly maxReAlerts: number;
  private readonly pollMs: number;
  private readonly now: () => number;
  private readonly log: (line: string) => void;
  private readonly drainWaitMs: number;
  private readonly lateAfterS: number;
  private readonly resumeWindowMs: number;
  private readonly active = new Map<string, Promise<void>>();
  private readonly handled = new Set<string>();
  private readonly sessions = new Map<string, Session>();
  private readonly sleepers = new Map<NodeJS.Timeout, () => void>();
  private unsubscribe: (() => void) | null = null;
  private drillTimer: NodeJS.Timeout | null = null;
  private listening = false;
  private stopped = false;

  constructor(options: EscalationRunnerOptions) {
    this.db = options.db;
    this.live = options.live;
    this.registry = options.registry;
    this.provider = new BookedAcks(options.provider);
    this.siteLedger = options.siteLedger ?? null;
    this.ackTimeoutS = options.ackTimeoutS ?? options.registry.ack_timeout_s ?? 60;
    this.maxReAlerts = options.maxReAlerts ?? options.registry.max_realerts ?? 1;
    this.pollMs = options.pollMs ?? 1000;
    this.now = options.now ?? Date.now;
    this.log = options.log ?? (() => {});
    this.drainWaitMs = options.drainWaitMs ?? 10_000;
    this.lateAfterS = options.lateAfterS ?? 120;
    this.resumeWindowMs = options.resumeWindowMs ?? 3_600_000;
  }

  start(): void {
    this.stopped = false;
    this.unsubscribe ??= this.live.subscribe((event) => {
      if (event.kind === 'incident' && event.open) void this.escalate(event.incident_id);
    });
    if (!this.listening) {
      this.listening = true;
      this.provider.onReply((reply) => this.onReply(reply));
    }
    this.resume();
    this.drillTimer ??= setInterval(() => this.pickUpDrills(), DRILL_PICKUP_MS);
    this.drillTimer.unref?.();
  }

  /** Stop listening and leave escalations in flight marked running, for the next start to resume. */
  stop(): void {
    this.stopped = true;
    this.unsubscribe?.();
    this.unsubscribe = null;
    if (this.drillTimer) clearInterval(this.drillTimer);
    this.drillTimer = null;
    for (const [timer, wake] of this.sleepers) {
      clearTimeout(timer);
      wake();
    }
    this.sleepers.clear();
  }

  /** Resolves when no escalation or drill is running. For tests and the dry run. */
  async idle(): Promise<void> {
    while (this.active.size) await Promise.allSettled([...this.active.values()]);
  }

  get running(): number {
    return this.active.size;
  }

  /** Escalate one incident, once. Returns immediately; the work starts on the next turn of the event loop. */
  escalate(incidentId: number): Promise<void> {
    return this.track({ kind: 'incident', id: incidentId }, () => this.runIncident(incidentId));
  }

  /** Start a drill now. Throws for an empty or unknown service list. */
  startDrill(services: Service[], by: string): number {
    const id = queueDrill(this.db, services, by, this.now());
    void this.track({ kind: 'drill', id }, () => this.runDrill(id));
    return id;
  }

  /**
   * The site's operator acknowledges on the dashboard. `booked` when an escalation is
   * running (the next check records it), `recorded` when it had already finished,
   * null when the site was never alerted or has already answered.
   */
  acknowledgeSite(incidentId: number, by: string): 'booked' | 'recorded' | null {
    const session = this.sessions.get(keyOf({ kind: 'incident', id: incidentId }));
    if (session) {
      const site = session.outcomes.find((o) => o.service === 'site' && !o.acknowledged && o.receipt);
      if (!site?.receipt) return null;
      this.provider.book({ receipt_id: site.receipt.receipt_id, service: 'site', by, at: this.now() / 1000 });
      return 'booked';
    }
    const events = this.eventsFor({ kind: 'incident', id: incidentId });
    if (events.some((e) => e.type === 'alert_ack' && e.service === 'site')) return null;
    const last = events.filter((e) => e.type === 'alert_sent' && e.service === 'site').at(-1);
    const tie = this.tieFor(incidentId);
    if (!last || !tie) return null;
    const at = this.now() / 1000;
    this.recordEvent({ kind: 'incident', id: incidentId }, tie, {
      type: 'alert_ack',
      at,
      payload: { service: 'site', receipt_id: String(last.receipt_id), by, latency_s: whole(at - last.at_ms / 1000) },
    });
    return 'recorded';
  }

  /** A message to the site that needs no answer, such as a unit gone offline. */
  async notifySite(text: string): Promise<boolean> {
    const contact = siteContact(this.registry);
    const notice: Alert = {
      incident_id: 'notice', site: this.registry, node_id: '', where: '', hazard: '', scores: {},
      media: { frames: 0, clip_seconds: 0, hashes: [] }, ledger_seq: 0, ledger_hash: '', raised_at: this.now() / 1000,
      kind: 'notice', notice_text: text,
    };
    try {
      await this.provider.send(notice, contact);
      audit(this.db, 'escalation', 'site_notice_sent', { text, recipient: maskRecipient(contact.recipient) }, this.now());
      return true;
    } catch (err) {
      audit(this.db, 'escalation', 'site_notice_failed', { text, error: errorText(err) }, this.now());
      this.log(`notice to the site not sent: ${errorText(err)}`);
      return false;
    }
  }

  private track(subject: Subject, work: () => Promise<void>): Promise<void> {
    const key = keyOf(subject);
    const existing = this.active.get(key);
    if (existing) return existing;
    if (this.handled.has(key)) return Promise.resolve();
    this.handled.add(key);
    const run = work()
      .catch((err) => this.log(`escalation for ${subject.kind} ${subject.id} stopped: ${errorText(err)}`))
      .finally(() => this.active.delete(key));
    this.active.set(key, run);
    return run;
  }

  private sleep(ms: number): Promise<void> {
    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        this.sleepers.delete(timer);
        resolve();
      }, ms);
      this.sleepers.set(timer, resolve);
    });
  }

  /* --- incidents ------------------------------------------------------------ */

  private async runIncident(incidentId: number, resumed?: EventRow[]): Promise<void> {
    await new Promise((resolve) => setImmediate(resolve)); // never inside ingest's call stack
    if (this.stopped) return;
    const found = this.findIncident(incidentId, Boolean(resumed));
    if (!found) return;
    const { alert, tie } = found;
    const subject: Subject = { kind: 'incident', id: incidentId };

    let contacts: ServiceContact[];
    let withheld: string | null;
    let late = false;
    if (resumed) {
      const services = new Set(resumed.filter((e) => e.type === 'alert_sent' || e.type === 'alert_failed').map((e) => e.service));
      contacts = this.registry.contacts.filter((c) => services.has(c.service));
      withheld = found.reason;
      late = found.late;
    } else {
      late = await this.isLate(incidentId, tie, found.ts, found.tsSrc);
      if (this.stopped) return;
      const live = this.registry.contacts.filter((c) => c.service !== 'site' && c.mode === 'live');
      contacts = late ? [siteContact(this.registry)] : this.registry.contacts.filter((c) => c.service === 'site' || c.mode === 'live');
      withheld = late ? 'late' : live.length ? null : 'drill_mode';
      this.db.prepare('UPDATE incidents SET late = ?, escalation_state = ?, escalation_reason = ? WHERE id = ?')
        .run(late ? 1 : 0, 'running', withheld, incidentId);
    }

    const scoped: Alert = { ...alert, kind: late ? 'late' : 'incident', site: { ...this.registry, contacts } };
    this.log(`incident ${incidentId}: ${resumed ? 'resuming' : 'alerting'} ${contacts.map((c) => c.service).join(', ')}${withheld ? ` (${withheld})` : ''} (${alert.hazard} at ${alert.where})`);
    const outcomes = await this.drive(subject, scoped, tie, resumed ? this.rebuild(resumed, contacts) : undefined, () => {
      if (withheld && !resumed) {
        this.ledger('escalation_withheld', { unit: tie.unit, incident: tie.incident, critical_seq: tie.critical_seq, reason: withheld });
      }
    });
    if (this.stopped) return; // still marked running: the next start resumes it
    this.db.prepare('UPDATE incidents SET escalation_state = ?, escalation_reason = ? WHERE id = ?')
      .run(withheld ? 'withheld' : 'finished', withheld, incidentId);
    const answered = outcomes.filter((o) => o.acknowledged).map((o) => o.service);
    this.log(`incident ${incidentId}: escalation finished; acknowledged by ${answered.join(', ') || 'nobody'}`);
  }

  private tieFor(incidentId: number): (Tie & { ts: number; tsSrc: string; verify: string; received: number; payload: string }) | null {
    const row = this.db.prepare(
      `SELECT i.device_id, e.seq, e.hash, e.ts, e.ts_src, e.verify_result, e.received_at, e.payload FROM incidents i
       JOIN ledger_entries e ON e.device_id = i.device_id AND e.epoch = i.epoch AND e.seq = i.start_seq WHERE i.id = ?`,
    ).get(incidentId) as Row | undefined;
    if (!row) return null;
    return {
      unit: String(row.device_id), incident: incidentId, critical_seq: Number(row.seq), critical_hash: String(row.hash),
      drill: false, incident_id: String(incidentId), ts: Number(row.ts), tsSrc: String(row.ts_src),
      verify: String(row.verify_result), received: Number(row.received_at), payload: String(row.payload),
    };
  }

  private findIncident(incidentId: number, resuming: boolean): { alert: Alert; tie: Tie; ts: number; tsSrc: string; reason: string | null; late: boolean } | null {
    const incident = this.db.prepare('SELECT * FROM incidents WHERE id = ?').get(incidentId) as Row | undefined;
    if (!incident) return null;
    if (!resuming) {
      const prior = this.db.prepare('SELECT COUNT(*) AS n FROM alert_events WHERE incident_id = ?').get(incidentId) as Row;
      if (Number(prior.n) > 0) return null;
    }
    const found = this.tieFor(incidentId);
    if (!found) return null;
    const { ts, tsSrc, verify, received, payload: payloadText, ...tie } = found;
    if (!MAY_ESCALATE.has(verify)) {
      audit(this.db, 'escalation', 'escalation_withheld', { incident: incidentId, device_id: tie.unit, verify_result: verify }, this.now());
      this.log(`incident ${incidentId}: not escalated, its CRITICAL entry failed verification (${verify})`);
      this.ledger('escalation_withheld', { unit: tie.unit, incident: incidentId, critical_seq: tie.critical_seq, reason: 'verification' });
      this.db.prepare("UPDATE incidents SET escalation_state = 'withheld', escalation_reason = 'verification' WHERE id = ?").run(incidentId);
      return null;
    }

    const device = this.db.prepare('SELECT label FROM devices WHERE id = ?').get(tie.unit) as Row | undefined;
    const payload = JSON.parse(payloadText) as { reason?: unknown; scores?: Record<string, number> };
    const reasons = Array.isArray(payload.reason) ? payload.reason.map(String) : [];
    const hashes = (this.db.prepare('SELECT sha256 FROM media_refs WHERE device_id = ? AND epoch = ? AND seq >= ? ORDER BY seq, idx')
      .all(tie.unit, incident.epoch as number, incident.start_seq as number) as Row[]).map((r) => String(r.sha256));

    const alert: Alert = {
      incident_id: String(incidentId),
      site: this.registry,
      node_id: tie.unit,
      where: String(device?.label || `unit ${tie.unit.slice(0, 8)}`),
      hazard: reasons.length > 1 ? `${reasons.join(' + ')} agree` : (reasons[0] ?? 'CRITICAL'),
      scores: payload.scores ?? {},
      media: { frames: hashes.length, clip_seconds: 0, hashes },
      ledger_seq: tie.critical_seq,
      ledger_hash: tie.critical_hash,
      raised_at: this.now() / 1000,
      ledger_received_at_ms: received,
    };
    return {
      alert, tie, ts, tsSrc,
      reason: incident.escalation_reason === null || incident.escalation_reason === undefined ? null : String(incident.escalation_reason),
      late: Boolean(incident.late),
    };
  }

  /**
   * Late means the CRITICAL was already on the unit when it last connected, the
   * incident had closed by the time that backlog arrived, and the alarm is old
   * (or its time cannot be trusted). An alarm written after the unit's latest
   * connection is never late.
   */
  private async isLate(incidentId: number, tie: Tie, ts: number, tsSrc: string): Promise<boolean> {
    const device = () => this.db.prepare('SELECT * FROM devices WHERE id = ?').get(tie.unit) as Row | undefined;
    const announced = (row: Row | undefined) =>
      row?.hello_next_seq === null || row?.hello_next_seq === undefined ? null : Number(row.hello_next_seq);
    const first = announced(device());
    if (first === null || tie.critical_seq >= first) return false;

    const deadline = Date.now() + this.drainWaitMs;
    for (;;) {
      const row = device();
      const next = announced(row);
      if (next === null || (row?.last_seq !== null && row?.last_seq !== undefined && Number(row.last_seq) >= next - 1)) break;
      if (Date.now() >= deadline || this.stopped) break;
      await this.sleep(Math.min(100, this.drainWaitMs));
    }
    const incident = this.db.prepare('SELECT end_seq FROM incidents WHERE id = ?').get(incidentId) as Row | undefined;
    if (!incident || incident.end_seq === null) return false;
    if (tsSrc === 'gateway' || tsSrc === 'ntp') return this.now() / 1000 - ts > this.lateAfterS;
    return true;
  }

  /* --- drills --------------------------------------------------------------- */

  private pickUpDrills(): void {
    if (this.stopped) return;
    const cutoff = this.now() - this.resumeWindowMs;
    for (const row of this.db.prepare('SELECT id, started_at_ms FROM drills WHERE finished_at_ms IS NULL ORDER BY id').all() as Row[]) {
      const id = Number(row.id);
      const key = keyOf({ kind: 'drill', id });
      if (this.handled.has(key)) continue;
      if (Number(row.started_at_ms) < cutoff) {
        this.handled.add(key);
        this.db.prepare('UPDATE drills SET finished_at_ms = ? WHERE id = ?').run(this.now(), id);
        audit(this.db, 'escalation', 'drill_abandoned', { drill: id }, this.now());
        continue;
      }
      void this.track({ kind: 'drill', id }, () => this.runDrill(id));
    }
  }

  private async runDrill(id: number): Promise<void> {
    await new Promise((resolve) => setImmediate(resolve));
    if (this.stopped) return;
    const row = this.db.prepare('SELECT * FROM drills WHERE id = ?').get(id) as Row | undefined;
    if (!row || row.finished_at_ms !== null) return;
    const services = JSON.parse(String(row.services)) as Service[];
    const by = String(row.by);
    const contacts = this.registry.contacts.filter((c) => services.includes(c.service));
    const events = this.eventsFor({ kind: 'drill', id });
    const tie: Tie = { unit: '', incident: 0, critical_seq: 0, critical_hash: ZERO_HASH, drill: true, incident_id: `drill-${id}` };
    const alert: Alert = {
      incident_id: tie.incident_id, site: { ...this.registry, contacts }, node_id: '', where: this.registry.site_name, hazard: 'drill',
      scores: {}, media: { frames: 0, clip_seconds: 0, hashes: [] }, ledger_seq: 0, ledger_hash: '', raised_at: this.now() / 1000, kind: 'drill',
    };
    if (!events.length) {
      this.ledger('drill', { drill_id: id, by, services, phase: 'started' });
      this.live.publish({ kind: 'drill', id });
    }
    this.log(`drill ${id}: ${events.length ? 'resuming' : 'alerting'} ${contacts.map((c) => c.service).join(', ')}`);
    await this.drive({ kind: 'drill', id }, alert, tie, events.length ? this.rebuild(events, contacts) : undefined);
    if (this.stopped) return;
    this.db.prepare('UPDATE drills SET finished_at_ms = ? WHERE id = ?').run(this.now(), id);
    this.ledger('drill', { drill_id: id, by, services, phase: 'finished' });
    this.live.publish({ kind: 'drill', id });
  }

  /* --- the loop shared by incidents and drills --------------------------------- */

  private async drive(subject: Subject, alert: Alert, tie: Tie, resumed: ServiceOutcome[] | undefined, afterRaise?: () => void): Promise<ServiceOutcome[]> {
    const service = new EscalationService(this.provider, {
      ackTimeout: this.ackTimeoutS,
      maxReAlerts: this.maxReAlerts,
      now: () => this.now() / 1000,
      onLedgerEvent: (e) => this.recordEvent(subject, tie, e),
      onSendFailure: (f) => this.recordFailure(subject, tie, f),
    });
    const key = keyOf(subject);
    const outcomes: ServiceOutcome[] = resumed ?? [];
    this.sessions.set(key, { subject, outcomes });
    try {
      if (!resumed) outcomes.push(...(await service.raise(alert)));
      afterRaise?.();
      while (!this.stopped && !service.finished(outcomes)) {
        await this.sleep(this.pollMs);
        if (this.stopped) break;
        await service.poll(alert, outcomes);
      }
    } finally {
      this.sessions.delete(key);
    }
    return outcomes;
  }

  /** Outcomes as they stood when the gateway stopped, from the rows it wrote. */
  private rebuild(events: EventRow[], contacts: ServiceContact[]): ServiceOutcome[] {
    return contacts.map((contact) => {
      const mine = events.filter((e) => e.service === contact.service);
      const receipts: Receipt[] = mine.filter((e) => e.type === 'alert_sent').map((e) => ({
        receipt_id: String(e.receipt_id),
        service: contact.service,
        channel: String(e.payload.channel ?? contact.channel) as Receipt['channel'],
        recipient: String(e.payload.recipient ?? contact.recipient),
        sent_at: e.at_ms / 1000,
      }));
      const failures = mine.filter((e) => e.type === 'alert_failed');
      const current = receipts.at(-1) ?? null;
      const lastFailure = failures.at(-1);
      return {
        service: contact.service,
        receipt: current,
        earlier: receipts.slice(0, -1),
        acknowledged: mine.some((e) => e.type === 'alert_ack'),
        re_alerts: mine.filter((e) => e.type === 'alert_timeout').length,
        window_start: Math.max(current?.sent_at ?? 0, lastFailure ? lastFailure.at_ms / 1000 : 0),
        failed_sends: failures.length,
      };
    });
  }

  private eventsFor(subject: Subject): EventRow[] {
    const rows = subject.kind === 'incident'
      ? this.db.prepare('SELECT id, at_ms, type, service, receipt_id, payload FROM alert_events WHERE incident_id = ? ORDER BY at_ms, id').all(subject.id)
      : this.db.prepare('SELECT id, at_ms, type, service, receipt_id, payload FROM drill_events WHERE drill_id = ? ORDER BY at_ms, id').all(subject.id);
    return (rows as Row[]).map((r) => ({
      id: Number(r.id), at_ms: Number(r.at_ms), type: String(r.type), service: String(r.service),
      receipt_id: r.receipt_id === null ? null : String(r.receipt_id), payload: JSON.parse(String(r.payload)) as Record<string, unknown>,
    }));
  }

  /** An escalation still marked running, and every unfinished drill, from the last hour. */
  private resume(): void {
    const cutoff = this.now() - this.resumeWindowMs;
    for (const row of this.db.prepare("SELECT id FROM incidents WHERE escalation_state = 'running' ORDER BY id").all() as Row[]) {
      const id = Number(row.id);
      if (this.handled.has(keyOf({ kind: 'incident', id }))) continue;
      const events = this.eventsFor({ kind: 'incident', id });
      const started = events.length ? events[0].at_ms : this.tieFor(id)?.received ?? null;
      if (started === null || started < cutoff) {
        this.db.prepare("UPDATE incidents SET escalation_state = 'finished', escalation_reason = COALESCE(escalation_reason, 'not resumed after a gateway restart') WHERE id = ?").run(id);
        continue;
      }
      void this.track({ kind: 'incident', id }, () => this.runIncident(id, events.length ? events : undefined));
    }
    this.pickUpDrills();
  }

  /* --- replies ---------------------------------------------------------------- */

  private onReply(reply: Reply): void {
    if (this.stopped) return;
    let matched: string | null = null;
    if (ACK_WORDS.test(reply.body.trim())) {
      const sender = phoneKey(reply.from);
      let best: { receipt: Receipt; service: Service } | null = null;
      for (const session of this.sessions.values()) {
        for (const outcome of session.outcomes) {
          if (outcome.acknowledged) continue;
          for (const receipt of [outcome.receipt, ...outcome.earlier]) {
            if (receipt && sender && phoneKey(receipt.recipient) === sender && (!best || receipt.sent_at > best.receipt.sent_at)) {
              best = { receipt, service: outcome.service };
            }
          }
        }
      }
      if (best) {
        this.provider.book({ receipt_id: best.receipt.receipt_id, service: best.service, by: reply.from, at: reply.at });
        matched = best.receipt.receipt_id;
      }
    }
    if (!reply.dtmf) {
      this.db.prepare('INSERT INTO sms_inbox (received_at_ms, sender, body, matched_receipt) VALUES (?, ?, ?, ?)')
        .run(Math.round(reply.at * 1000), reply.from, reply.body.slice(0, 500), matched);
    }
    this.log(`${reply.dtmf ? 'key' : 'SMS'} from ${maskRecipient(reply.from)}${matched ? `: acknowledges ${matched}` : ': matched no open alert'}`);
  }

  /* --- recording ----------------------------------------------------------------- */

  private insert(subject: Subject, tie: Tie, type: AlertEventType, atMs: number, service: string, receiptId: string | null, payload: Record<string, unknown>): void {
    if (this.stopped) return;
    if (subject.kind === 'incident') {
      this.db.prepare('INSERT INTO alert_events (incident_id, at_ms, type, service, receipt_id, payload) VALUES (?, ?, ?, ?, ?, ?)')
        .run(subject.id, Math.round(atMs), type, service, receiptId, JSON.stringify(payload));
      this.live.publish({ kind: 'alert', device_id: tie.unit, incident_id: subject.id, type, service });
    } else {
      this.db.prepare('INSERT INTO drill_events (drill_id, at_ms, type, service, receipt_id, payload) VALUES (?, ?, ?, ?, ?, ?)')
        .run(subject.id, Math.round(atMs), type, service, receiptId, JSON.stringify(payload));
      this.live.publish({ kind: 'alert', drill_id: subject.id, type, service });
    }
  }

  private recordEvent(subject: Subject, tie: Tie, e: LedgerEvent): void {
    if (this.stopped) return;
    const p = e.payload;
    const receipt = p.receipt_id ? String(p.receipt_id) : null;
    const detail = e.detail?.delivered_at !== undefined ? { delivered_at_ms: Math.round(Number(e.detail.delivered_at) * 1000) } : {};
    this.insert(subject, tie, e.type, e.at * 1000, String(p.service), receipt, { ...p, ...detail });

    const common = { unit: tie.unit, incident: tie.incident, drill: tie.drill };
    if (e.type === 'alert_sent') {
      this.ledger('alert_sent', {
        service: String(p.service), channel: String(p.channel), recipient: String(p.recipient), receipt_id: String(p.receipt_id),
        incident_id: tie.incident_id, critical_seq: tie.critical_seq, critical_hash: fromHex(tie.critical_hash), ...common,
        ...(p.retry_of ? { retry_of: String(p.retry_of) } : {}),
      });
    } else if (e.type === 'alert_ack') {
      this.ledger('alert_ack', { service: String(p.service), receipt_id: String(p.receipt_id), by: String(p.by), latency_s: whole(p.latency_s), ...common });
    } else {
      this.ledger('alert_timeout', {
        service: String(p.service), receipt_id: String(p.receipt_id ?? ''), elapsed_s: whole(p.elapsed_s), action: String(p.action), ...common,
      });
    }
  }

  private recordFailure(subject: Subject, tie: Tie, f: SendFailure): void {
    this.log(`${subject.kind} ${subject.id}: ${f.service} not alerted: ${f.error}`);
    this.insert(subject, tie, 'alert_failed', f.at * 1000, f.service, null, {
      recipient: f.recipient, channel: f.channel, error: f.error, ...(f.retry_of ? { retry_of: f.retry_of } : {}),
    });
  }

  /** The site ledger never holds up an escalation: a write that fails is logged and audited. */
  private ledger(type: EventType, payload: Record<string, CborValue>): void {
    if (!this.siteLedger || this.stopped) return;
    try {
      this.siteLedger.append(type, payload);
    } catch (err) {
      this.log(`site ledger: ${type} not written: ${errorText(err)}`);
      audit(this.db, 'escalation', 'site_ledger_write_failed', { type, error: errorText(err) }, this.now());
    }
  }
}

/** Write registry_change when the registry file differs from the one the site ledger last recorded. */
export function recordRegistryChange(siteLedger: SiteLedger, sha256: string, by = 'startup'): boolean {
  const newest = siteLedger.entries({ type: 'registry_change', limit: 1 })[0];
  const previous = newest && typeof newest.payload.new_registry === 'string' ? newest.payload.new_registry : null;
  if (previous === sha256) return false;
  siteLedger.append('registry_change', { old_registry: previous ? fromHex(previous) : null, new_registry: fromHex(sha256), by });
  return true;
}

export interface EscalationSetup {
  runner: EscalationRunner | null;
  /** The loaded registry, even when escalation is off, for the dashboard. */
  registry: SiteRegistry | null;
  registrySha256: string | null;
  message: string;
}

/** The runner the configuration asks for, or null with the reason. The gateway runs either way. */
export function escalationFromConfig(
  config: GatewayConfig,
  deps: { db: Db; live: LiveBus; log?: (line: string) => void; siteLedger?: SiteLedger | null; modemPort?: ModemPort },
): EscalationSetup {
  let registry: SiteRegistry | null = null;
  let sha: string | null = null;
  let problem: string | null = null;
  if (config.serviceRegistryPath) {
    try {
      registry = loadRegistry(config.serviceRegistryPath);
      sha = createHash('sha256').update(readFileSync(config.serviceRegistryPath)).digest('hex');
      if (registry.site_id !== config.siteId) {
        problem = `the registry is for site ${registry.site_id}, this gateway is ${config.siteId}`;
        registry = null;
        sha = null;
      }
    } catch (err) {
      problem = `service registry ${errorText(err)}`;
      registry = null;
    }
  }
  if (registry && sha && deps.siteLedger) {
    try {
      if (recordRegistryChange(deps.siteLedger, sha)) deps.log?.(`registry ${sha.slice(0, 12)}… recorded in the site ledger`);
    } catch (err) {
      deps.log?.(`registry change not recorded in the site ledger: ${errorText(err)}`);
    }
  }

  const provider = config.alertProvider;
  const none = (message: string) => ({ runner: null, registry, registrySha256: sha, message });
  if (provider === 'off') return none('escalation off (FIREX_ALERT_PROVIDER=off)');
  if (provider !== 'console' && provider !== 'http' && provider !== 'modem') {
    return none(`escalation NOT started: FIREX_ALERT_PROVIDER "${provider}" is not off, console, http or modem`);
  }
  if (!config.serviceRegistryPath) {
    return none(`escalation NOT started: FIREX_ALERT_PROVIDER is ${provider} but FIREX_SERVICE_REGISTRY is not set`);
  }
  if (!registry) return none(`escalation NOT started: ${problem}`);

  let alertProvider: AlertProvider;
  let message: string;
  if (provider === 'http') {
    if (!config.alertHttpUrl) {
      return none('escalation NOT started: FIREX_ALERT_PROVIDER is http but FIREX_ALERT_HTTP_URL is not set');
    }
    alertProvider = new HttpAlert({ baseUrl: config.alertHttpUrl });
    message = `escalation ON: http to ${config.alertHttpUrl} (mock endpoints for a dry run, or a gateway with an agreement behind every live contact)`;
  } else if (provider === 'modem') {
    const reachable = registry.contacts.flatMap((c) => [c, ...(c.backup ? [{ ...c, ...c.backup }] : [])]);
    const wrong = reachable.filter((c) => c.channel === 'api' || c.channel === 'console' || ((c.channel === 'sms' || c.channel === 'voice') && !isPhoneNumber(c.recipient)));
    if (wrong.length) {
      return none(`escalation NOT started: the modem sends SMS and calls phone numbers; ${wrong.map((c) => `${c.service} ${c.channel} ${c.recipient}`).join(', ')} cannot be reached that way`);
    }
    try {
      const port = deps.modemPort ?? openSerialPort(config.modemDevice, config.modemBaud);
      alertProvider = new ModemAlert({ modem: new AtModem(port), ringSeconds: config.modemRingS, log: deps.log });
    } catch (err) {
      return none(`escalation NOT started: modem ${config.modemDevice}: ${errorText(err)}`);
    }
    message = `escalation ON: GSM modem on ${config.modemDevice}`;
  } else {
    alertProvider = new ConsoleAlert(undefined, (line) => (deps.log ?? console.log)(line));
    message = 'escalation ON: console (alerts are printed to this log; nothing acknowledges, so every service times out)';
  }

  const ackTimeoutS = registry.ack_timeout_s ?? config.alertAckTimeoutS;
  const maxReAlerts = registry.max_realerts ?? config.alertMaxReAlerts;
  const liveServices = registry.contacts.filter((c) => c.service !== 'site' && c.mode === 'live').map((c) => c.service);
  const runner = new EscalationRunner({
    db: deps.db,
    live: deps.live,
    registry,
    provider: alertProvider,
    siteLedger: deps.siteLedger,
    ackTimeoutS,
    maxReAlerts,
    pollMs: config.alertPollMs,
    log: deps.log,
  });
  return {
    runner,
    registry,
    registrySha256: sha,
    message: `${message}; ${registry.site_name}, live services: ${liveServices.join(', ') || 'none, drills only'}; ack window ${ackTimeoutS} s, ${maxReAlerts} re-alert(s)`,
  };
}
