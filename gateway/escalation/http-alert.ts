/**
 * HttpAlert — AlertProvider over plain HTTP.
 *
 * In M4 its only target is the mock endpoints in mock-endpoints.ts: the
 * escalation dry runs measure the alert path against them. It is not a way to
 * reach a real control room; real services are connected in M5, through
 * providers that have agreements behind them.
 *
 *   POST {base}/alerts/{recipient}   {alert, contact, sent_at_ms, ledger_received_at_ms, retry_of}
 *                                    -> 200 {receipt_id, received_at_ms}
 *   GET  {base}/acks/{receipt_id}    -> 200 {receipt_id, service, by, at_ms} | 204 not yet
 */
import type { Ack, Alert, AlertProvider, Receipt, SendContext, Service, ServiceContact } from './provider.ts';

export interface HttpAlertOptions {
  baseUrl: string;
  /** Per request. */
  timeoutMs?: number;
  /** Attempts per send before it is reported as failed. */
  attempts?: number;
  /** Wait before attempt n+1 is n times this. */
  backoffMs?: number;
  /** Unix milliseconds. */
  now?: () => number;
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

export class HttpAlert implements AlertProvider {
  readonly name = 'http';
  private readonly base: string;
  private readonly timeoutMs: number;
  private readonly attempts: number;
  private readonly backoffMs: number;
  private readonly now: () => number;

  constructor(options: HttpAlertOptions) {
    this.base = options.baseUrl.replace(/\/+$/, '');
    this.timeoutMs = options.timeoutMs ?? 5000;
    this.attempts = Math.max(1, options.attempts ?? 3);
    this.backoffMs = options.backoffMs ?? 250;
    this.now = options.now ?? Date.now;
  }

  async send(alert: Alert, contact: ServiceContact, context?: SendContext): Promise<Receipt> {
    const url = `${this.base}/alerts/${encodeURIComponent(contact.recipient)}`;
    let lastError = 'no attempt made';
    for (let attempt = 1; attempt <= this.attempts; attempt++) {
      const sentMs = this.now();
      try {
        const res = await fetch(url, {
          method: 'POST',
          headers: { 'content-type': 'application/json' },
          body: JSON.stringify({
            alert,
            contact,
            sent_at_ms: sentMs,
            ledger_received_at_ms: alert.ledger_received_at_ms ?? null,
            retry_of: context?.retry_of ?? null,
          }),
          signal: AbortSignal.timeout(this.timeoutMs),
        });
        if (res.ok) {
          const body = (await res.json()) as { receipt_id?: unknown; received_at_ms?: unknown };
          if (typeof body.receipt_id !== 'string' || !body.receipt_id) throw new Error('reply carried no receipt_id');
          return {
            receipt_id: body.receipt_id,
            service: contact.service,
            channel: contact.channel,
            recipient: contact.recipient,
            sent_at: sentMs / 1000,
            ...(typeof body.received_at_ms === 'number' ? { delivered_at: body.received_at_ms / 1000 } : {}),
          };
        }
        lastError = `HTTP ${res.status}`;
        await res.body?.cancel();
      } catch (err) {
        lastError = err instanceof Error ? err.message : String(err);
      }
      if (attempt < this.attempts) await sleep(this.backoffMs * attempt);
    }
    throw new Error(`${contact.service} alert to ${contact.recipient} not delivered after ${this.attempts} attempts: ${lastError}`);
  }

  async ack(receiptId: string): Promise<Ack | null> {
    const res = await fetch(`${this.base}/acks/${encodeURIComponent(receiptId)}`, { signal: AbortSignal.timeout(this.timeoutMs) });
    if (res.status === 204 || res.status === 404) {
      await res.body?.cancel();
      return null;
    }
    if (!res.ok) {
      await res.body?.cancel();
      throw new Error(`ack lookup for ${receiptId}: HTTP ${res.status}`);
    }
    const body = (await res.json()) as { receipt_id: string; service: Service; by: string; at_ms: number };
    return { receipt_id: body.receipt_id, service: body.service, by: body.by, at: body.at_ms / 1000 };
  }
}
