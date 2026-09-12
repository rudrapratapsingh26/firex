/**
 * ConsoleAlert — the M0 implementation of AlertProvider.
 *
 * Prints what would be sent and lets a test or an operator acknowledge on
 * behalf of a service. No telephony account, no control-room agreement, and
 * the escalation logic above it is exactly the code that will run in M5.
 */
import type { Ack, Alert, AlertProvider, Receipt, Service, ServiceContact } from './provider.ts';

export class ConsoleAlert implements AlertProvider {
  readonly name = 'console';
  private counter = 0;
  private readonly acks = new Map<string, Ack>();
  readonly sent: Receipt[] = [];

  private readonly now: () => number;
  private readonly write: (line: string) => void;

  constructor(
    now: () => number = () => Math.floor(Date.now() / 1000),
    write: (line: string) => void = (line) => console.log(line),
  ) {
    this.now = now;
    this.write = write;
  }

  async send(alert: Alert, contact: ServiceContact): Promise<Receipt> {
    this.counter += 1;
    const receipt: Receipt = {
      receipt_id: `r-${String(this.counter).padStart(4, '0')}`,
      service: contact.service,
      channel: contact.channel,
      recipient: contact.recipient,
      sent_at: this.now(),
    };
    this.sent.push(receipt);

    const label = contact.service.toUpperCase().padEnd(9);
    this.write(`  → ${label} ${contact.name} (${contact.recipient}) via ${contact.channel}  [${receipt.receipt_id}]`);
    this.write(`      FIREX CRITICAL · ${alert.site.site_name} · ${alert.where}`);
    this.write(`      hazard: ${alert.hazard} · media: ${alert.media.frames} photos + ${alert.media.clip_seconds}s clip`);
    this.write(`      ledger #${alert.ledger_seq} ${alert.ledger_hash.slice(0, 8)}… · reply 1 to acknowledge`);
    return receipt;
  }

  async ack(receiptId: string): Promise<Ack | null> {
    return this.acks.get(receiptId) ?? null;
  }

  /** Test and demo hook: acknowledge on behalf of a service. */
  acknowledge(receiptId: string, by: string, service?: Service): Ack {
    const receipt = this.sent.find((r) => r.receipt_id === receiptId);
    const ack: Ack = {
      receipt_id: receiptId,
      service: service ?? receipt?.service ?? 'site',
      by,
      at: this.now(),
    };
    this.acks.set(receiptId, ack);
    return ack;
  }
}
