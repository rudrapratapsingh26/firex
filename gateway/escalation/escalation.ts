/**
 * The escalation service: one CRITICAL alarm, four recipients.
 *
 * On a confirmed alarm the gateway alerts the people on site and, in parallel,
 * the fire station, police station and ambulance mapped to that site at
 * installation. Each send, acknowledgement and timeout becomes a ledger event,
 * so "who was told, and when did they answer" is a measured number rather than
 * a claim (SPEC.md section 3, events alert_sent / alert_ack / alert_timeout).
 *
 * The clock is injected so the 60-second timeout is testable without waiting.
 * It reads fractional seconds: with whole seconds a send at 10.9 s floors to 10
 * and a poll at 70.0 s already sees 60 elapsed, a re-alert a second early.
 * Numbers that go into a ledger payload are rounded, because the ledger's CBOR
 * refuses floats.
 */
import type { Ack, Alert, AlertProvider, Receipt, Service, ServiceContact } from './provider.ts';

export interface LedgerEvent {
  type: 'alert_sent' | 'alert_ack' | 'alert_timeout';
  payload: Record<string, string | number>;
  /** Unix seconds, possibly fractional. */
  at: number;
  /** Gateway-side detail that is not part of the ledger payload, such as a delivery confirmation. */
  detail?: Record<string, unknown>;
}

/** A send the provider could not complete. Not a ledger event: the gateway records it. */
export interface SendFailure {
  incident_id: string;
  service: Service;
  recipient: string;
  channel: string;
  error: string;
  at: number;
  retry_of?: string;
}

export interface EscalationOptions {
  /** Seconds to wait for an acknowledgement before re-alerting. */
  ackTimeout?: number;
  /** How many times to re-alert an unresponsive service. */
  maxReAlerts?: number;
  /** Unix seconds, possibly fractional. */
  now?: () => number;
  onLedgerEvent?: (event: LedgerEvent) => void;
  onSendFailure?: (failure: SendFailure) => void;
}

export interface ServiceOutcome {
  service: Service;
  /** The receipt currently outstanding; null while no send to this service has succeeded. */
  receipt: Receipt | null;
  /** Receipts this one replaced: an answer to any of them still counts. */
  earlier: Receipt[];
  acknowledged: boolean;
  latency_s?: number;
  re_alerts: number;
  /** When the current acknowledgement window opened: the last successful send, or the failed attempt. */
  window_start: number;
  failed_sends: number;
}

export class EscalationService {
  private readonly ackTimeout: number;
  private readonly maxReAlerts: number;
  private readonly now: () => number;
  private readonly emit: (event: LedgerEvent) => void;
  private readonly failed: (failure: SendFailure) => void;
  private readonly provider: AlertProvider;

  constructor(provider: AlertProvider, options: EscalationOptions = {}) {
    this.provider = provider;
    this.ackTimeout = options.ackTimeout ?? 60;
    this.maxReAlerts = options.maxReAlerts ?? 1;
    this.now = options.now ?? (() => Date.now() / 1000);
    this.emit = options.onLedgerEvent ?? (() => {});
    this.failed = options.onSendFailure ?? (() => {});
  }

  /**
   * Fan out to every mapped service at once; the site desk is one of them. A
   * service whose send fails does not hold up the others: its failure is
   * reported, and it is tried again (its backup, if it has one) at the timeout.
   */
  async raise(alert: Alert): Promise<ServiceOutcome[]> {
    return Promise.all(
      alert.site.contacts.map(async (contact) => {
        const outcome: ServiceOutcome = {
          service: contact.service,
          receipt: null,
          earlier: [],
          acknowledged: false,
          re_alerts: 0,
          window_start: this.now(),
          failed_sends: 0,
        };
        await this.deliver(alert, outcome, contact);
        return outcome;
      }),
    );
  }

  /** True once every service has answered or been escalated. */
  finished(outcomes: ServiceOutcome[]): boolean {
    return outcomes.every((o) => o.acknowledged || o.re_alerts > this.maxReAlerts);
  }

  /**
   * Check for acknowledgements and act on silence. Call this on a timer; it
   * takes no wall-clock time itself, which is what makes it testable.
   */
  async poll(alert: Alert, outcomes: ServiceOutcome[]): Promise<ServiceOutcome[]> {
    for (const outcome of outcomes) {
      if (outcome.acknowledged) continue;
      if (await this.checkAck(outcome)) continue;
      if (outcome.re_alerts > this.maxReAlerts) continue; // escalated already: nothing more to send

      // Elapsed is measured from the send currently outstanding: a re-alert
      // replaces that receipt, which starts the next window from the re-alert.
      const now = this.now();
      const elapsed = now - outcome.window_start;
      if (elapsed < this.ackTimeout) continue;

      const action = outcome.re_alerts < this.maxReAlerts ? 're_alert' : 'escalate';
      this.emit({
        type: 'alert_timeout',
        at: now,
        payload: {
          service: outcome.service,
          receipt_id: outcome.receipt?.receipt_id ?? '',
          elapsed_s: Math.round(elapsed),
          action,
        },
      });
      outcome.re_alerts += 1;

      if (action === 're_alert') {
        const contact = alert.site.contacts.find((c) => c.service === outcome.service);
        const target = contact?.backup
          ? { ...contact, ...contact.backup, service: contact.service }
          : contact;
        if (target) await this.deliver(alert, outcome, target, outcome.receipt?.receipt_id);
      }
    }
    return outcomes;
  }

  private async deliver(alert: Alert, outcome: ServiceOutcome, contact: ServiceContact, retryOf?: string): Promise<void> {
    let receipt: Receipt;
    try {
      receipt = await this.provider.send(alert, contact, retryOf ? { retry_of: retryOf } : undefined);
    } catch (err) {
      outcome.failed_sends += 1;
      outcome.window_start = this.now();
      this.failed({
        incident_id: alert.incident_id,
        service: contact.service,
        recipient: contact.recipient,
        channel: contact.channel,
        error: err instanceof Error ? err.message : String(err),
        at: outcome.window_start,
        ...(retryOf ? { retry_of: retryOf } : {}),
      });
      return;
    }
    if (outcome.receipt) outcome.earlier.push(outcome.receipt);
    outcome.receipt = receipt;
    outcome.window_start = receipt.sent_at;
    this.emit({
      type: 'alert_sent',
      at: receipt.sent_at,
      payload: {
        service: receipt.service,
        channel: receipt.channel,
        recipient: receipt.recipient,
        receipt_id: receipt.receipt_id,
        incident_id: alert.incident_id,
        ...(retryOf ? { retry_of: retryOf } : {}),
      },
      ...(receipt.delivered_at !== undefined ? { detail: { delivered_at: receipt.delivered_at } } : {}),
    });
  }

  /** The newest receipt first; an unreachable endpoint counts as silence, which the timeout handles. */
  private async checkAck(outcome: ServiceOutcome): Promise<boolean> {
    const receipts = [...(outcome.receipt ? [outcome.receipt] : []), ...[...outcome.earlier].reverse()];
    for (const receipt of receipts) {
      let ack: Ack | null = null;
      try {
        ack = await this.provider.ack(receipt.receipt_id);
      } catch {
        ack = null;
      }
      if (!ack) continue;
      outcome.acknowledged = true;
      outcome.latency_s = ack.at - receipt.sent_at;
      this.emit({
        type: 'alert_ack',
        at: ack.at,
        payload: {
          service: ack.service,
          receipt_id: ack.receipt_id,
          by: ack.by,
          latency_s: Math.round(outcome.latency_s),
        },
      });
      return true;
    }
    return false;
  }
}
