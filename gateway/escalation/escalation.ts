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
 */
import type { Ack, Alert, AlertProvider, Receipt, Service } from './provider.ts';

export interface LedgerEvent {
  type: 'alert_sent' | 'alert_ack' | 'alert_timeout';
  payload: Record<string, string | number>;
  at: number;
}

export interface EscalationOptions {
  /** Seconds to wait for an acknowledgement before re-alerting. */
  ackTimeout?: number;
  /** How many times to re-alert an unresponsive service. */
  maxReAlerts?: number;
  now?: () => number;
  onLedgerEvent?: (event: LedgerEvent) => void;
}

export interface ServiceOutcome {
  service: Service;
  receipt: Receipt;
  acknowledged: boolean;
  latency_s?: number;
  re_alerts: number;
}

export class EscalationService {
  private readonly ackTimeout: number;
  private readonly maxReAlerts: number;
  private readonly now: () => number;
  private readonly emit: (event: LedgerEvent) => void;
  private readonly provider: AlertProvider;

  constructor(provider: AlertProvider, options: EscalationOptions = {}) {
    this.provider = provider;
    this.ackTimeout = options.ackTimeout ?? 60;
    this.maxReAlerts = options.maxReAlerts ?? 1;
    this.now = options.now ?? (() => Math.floor(Date.now() / 1000));
    this.emit = options.onLedgerEvent ?? (() => {});
  }

  /** Fan out to every mapped service at once; the site desk is one of them. */
  async raise(alert: Alert): Promise<ServiceOutcome[]> {
    const receipts = await Promise.all(
      alert.site.contacts.map(async (contact) => {
        const receipt = await this.provider.send(alert, contact);
        this.emit({
          type: 'alert_sent',
          at: receipt.sent_at,
          payload: {
            service: receipt.service,
            channel: receipt.channel,
            recipient: receipt.recipient,
            receipt_id: receipt.receipt_id,
            incident_id: alert.incident_id,
          },
        });
        return receipt;
      }),
    );
    return receipts.map((receipt) => ({
      service: receipt.service,
      receipt,
      acknowledged: false,
      re_alerts: 0,
    }));
  }

  /**
   * Check for acknowledgements and act on silence. Call this on a timer; it
   * takes no wall-clock time itself, which is what makes it testable.
   */
  async poll(alert: Alert, outcomes: ServiceOutcome[]): Promise<ServiceOutcome[]> {
    for (const outcome of outcomes) {
      if (outcome.acknowledged) continue;

      const ack: Ack | null = await this.provider.ack(outcome.receipt.receipt_id);
      if (ack) {
        outcome.acknowledged = true;
        outcome.latency_s = ack.at - outcome.receipt.sent_at;
        this.emit({
          type: 'alert_ack',
          at: ack.at,
          payload: {
            service: ack.service,
            receipt_id: ack.receipt_id,
            by: ack.by,
            latency_s: outcome.latency_s,
          },
        });
        continue;
      }

      // Elapsed is measured from the receipt currently outstanding: a re-alert
      // replaces that receipt, which starts the next window from the re-alert.
      const elapsed = this.now() - outcome.receipt.sent_at;
      if (elapsed >= this.ackTimeout) {
        const action = outcome.re_alerts < this.maxReAlerts ? 're_alert' : 'escalate';
        this.emit({
          type: 'alert_timeout',
          at: this.now(),
          payload: {
            service: outcome.service,
            receipt_id: outcome.receipt.receipt_id,
            elapsed_s: elapsed,
            action,
          },
        });
        outcome.re_alerts += 1;

        if (action === 're_alert') {
          const contact = alert.site.contacts.find((c) => c.service === outcome.service);
          const target = contact?.backup
            ? { ...contact, ...contact.backup, service: contact.service }
            : contact;
          if (target) {
            const receipt = await this.provider.send(alert, target);
            this.emit({
              type: 'alert_sent',
              at: receipt.sent_at,
              payload: {
                service: receipt.service,
                channel: receipt.channel,
                recipient: receipt.recipient,
                receipt_id: receipt.receipt_id,
                incident_id: alert.incident_id,
                retry_of: outcome.receipt.receipt_id,
              },
            });
            outcome.receipt = receipt;
          }
        }
      }
    }
    return outcomes;
  }
}
