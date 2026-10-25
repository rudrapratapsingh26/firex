/**
 * ModemAlert — SMS and ringing calls through a USB GSM modem on the gateway
 * (SIM800 or SIM7600 class), for the M5 pilots.
 *
 * A modem needs no internet, no account with an SMS aggregator and no bulk
 * sender registration, and it keeps working when the site's uplink does not.
 * One SIM sending a handful of messages an incident is within what an ordinary
 * SIM allows; a site that outgrows that needs an aggregator and its sender
 * registration (docs/service-registry.md).
 *
 *   SMS      AT+CMGS in text mode; delivered when the modem answers +CMGS
 *   voice    ATD<number>; rings for ringSeconds, then ATH; placed counts as delivered
 *   replies  +CMTI -> AT+CMGR -> AT+CMGD, reported to onReply listeners
 *   keys     +DTMF: or +RXDTMF: during a call, where the modem reports them
 *
 * No native module: on Linux the tty is configured with stty and read and
 * written as a file. Commands go one at a time through a queue, each with a
 * timeout, so a modem that stops answering fails a send instead of hanging
 * the escalation. Calls are placed one at a time, because a modem holds one.
 */
import { execFileSync } from 'node:child_process';
import { closeSync, createReadStream, createWriteStream, openSync } from 'node:fs';

import { alertText } from './messages.ts';
import type { Ack, Alert, AlertProvider, Receipt, Reply, ServiceContact } from './provider.ts';
import { isPhoneNumber, normalisePhone } from './registry.ts';

/** The byte stream to a modem: a tty on the gateway, or a fake in the tests. */
export interface ModemPort {
  write(data: string): void;
  onData(listener: (text: string) => void): void;
  close(): Promise<void>;
}

export function openSerialPort(device: string, baud = 115200): ModemPort {
  if (process.platform !== 'linux') {
    throw new Error(`the modem provider needs Linux, where ${device} is configured with stty; this gateway runs on ${process.platform}`);
  }
  execFileSync('stty', ['-F', device, String(baud), 'raw', '-echo', 'cs8', '-cstopb', '-parenb', 'clocal', 'cread', '-ixon', '-ixoff', '-crtscts']);
  const fd = openSync(device, 'r+');
  const input = createReadStream(device, { fd, autoClose: false, encoding: 'latin1' });
  const output = createWriteStream(device, { fd, autoClose: false });
  return {
    write: (data) => {
      output.write(Buffer.from(data, 'latin1'));
    },
    onData: (listener) => {
      input.on('data', (chunk) => listener(String(chunk)));
    },
    close: async () => {
      input.destroy();
      output.destroy();
      try {
        closeSync(fd);
      } catch {
        /* already closed */
      }
    },
  };
}

export interface InboundSms {
  index: number;
  sender: string;
  body: string;
}

interface Pending {
  command: string;
  lines: string[];
  /** Sent after the "> " prompt, then Ctrl-Z. */
  body?: string;
  /** The next line is an SMS body, whatever it says ("OK" included). */
  bodyNext: boolean;
  resolve: (lines: string[]) => void;
  reject: (err: Error) => void;
}

const CALL_FAILED = /^(NO CARRIER|BUSY|NO ANSWER|NO DIALTONE)$/;

/** The AT dialogue: one command at a time, final results, and the unsolicited lines in between. */
export class AtModem {
  private readonly port: ModemPort;
  private readonly timeoutMs: number;
  private buffer = '';
  private pending: Pending | null = null;
  private queue: Promise<unknown> = Promise.resolve();
  private readonly smsListeners: ((sms: InboundSms) => void)[] = [];
  private readonly dtmfListeners: ((key: string) => void)[] = [];

  constructor(port: ModemPort, options: { timeoutMs?: number } = {}) {
    this.port = port;
    this.timeoutMs = options.timeoutMs ?? 10_000;
    port.onData((text) => this.receive(text));
  }

  onSms(listener: (sms: InboundSms) => void): void {
    this.smsListeners.push(listener);
  }

  onDtmf(listener: (key: string) => void): void {
    this.dtmfListeners.push(listener);
  }

  /** Resolves with the lines before OK; rejects on ERROR, +CMS/+CME ERROR, a failed call, or silence. */
  command(command: string, options: { timeoutMs?: number; body?: string } = {}): Promise<string[]> {
    const timeoutMs = options.timeoutMs ?? this.timeoutMs;
    const run = () => new Promise<string[]>((resolve, reject) => {
      const timer = setTimeout(() => {
        if (this.pending?.command === command) this.pending = null;
        if (options.body !== undefined) this.port.write('\x1b'); // abandon a half-written message
        reject(new Error(`the modem did not answer ${command} within ${timeoutMs / 1000} s`));
      }, timeoutMs);
      this.pending = {
        command,
        lines: [],
        body: options.body,
        bodyNext: false,
        resolve: (lines) => {
          clearTimeout(timer);
          resolve(lines);
        },
        reject: (err) => {
          clearTimeout(timer);
          reject(err);
        },
      };
      this.port.write(`${command}\r`);
    });
    const result = this.queue.then(run, run);
    this.queue = result.catch(() => undefined);
    return result;
  }

  private receive(text: string): void {
    this.buffer += text;
    // The SMS prompt is "> " with no line ending after it.
    if (this.pending?.body !== undefined && /(^|\n)\s*>\s*$/.test(this.buffer)) {
      const body = this.pending.body;
      this.pending.body = undefined;
      this.buffer = '';
      this.port.write(`${body}\x1a`);
      return;
    }
    let newline: number;
    while ((newline = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, newline).trim();
      this.buffer = this.buffer.slice(newline + 1);
      if (line) this.line(line);
    }
  }

  private line(line: string): void {
    const pending = this.pending;
    if (pending?.bodyNext) {
      pending.bodyNext = false;
      pending.lines.push(line);
      return;
    }
    let m = line.match(/^\+CMTI:\s*"?[^",]*"?\s*,\s*(\d+)/);
    if (m) {
      void this.readSms(Number(m[1]));
      return;
    }
    m = line.match(/^\+(?:RX)?DTMF:\s*"?([0-9A-D*#])"?/i);
    if (m) {
      for (const listener of this.dtmfListeners) listener(m[1]);
      return;
    }
    if (!pending) return; // RING, a call ending, or noise between commands
    if (line === 'OK') {
      this.pending = null;
      pending.resolve(pending.lines);
    } else if (line === 'ERROR' || /^\+CM[SE] ERROR/.test(line) || (pending.command.startsWith('ATD') && CALL_FAILED.test(line))) {
      this.pending = null;
      pending.reject(new Error(`${pending.command}: ${line}`));
    } else if (line !== 'RING' && !CALL_FAILED.test(line)) {
      pending.lines.push(line);
      if (line.startsWith('+CMGR:')) pending.bodyNext = true;
    }
  }

  private async readSms(index: number): Promise<void> {
    try {
      const lines = await this.command(`AT+CMGR=${index}`);
      const header = lines.findIndex((l) => l.startsWith('+CMGR:'));
      if (header >= 0) {
        const fields = [...lines[header].matchAll(/"([^"]*)"/g)].map((f) => f[1]);
        const sms = { index, sender: fields[1] ?? '', body: lines.slice(header + 1).join('\n') };
        for (const listener of this.smsListeners) listener(sms);
      }
      await this.command(`AT+CMGD=${index}`);
    } catch {
      /* a message that cannot be read stays on the SIM; the modem announces the next one anyway */
    }
  }
}

export interface ModemAlertOptions {
  modem: AtModem;
  /** How long a voice alert rings before the gateway hangs up. */
  ringSeconds?: number;
  smsTimeoutMs?: number;
  dialTimeoutMs?: number;
  /** Unix milliseconds. */
  now?: () => number;
  log?: (line: string) => void;
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

export class ModemAlert implements AlertProvider {
  readonly name = 'modem';
  readonly ready: Promise<void>;
  private readonly modem: AtModem;
  private readonly ringMs: number;
  private readonly smsTimeoutMs: number;
  private readonly dialTimeoutMs: number;
  private readonly now: () => number;
  private readonly log: (line: string) => void;
  private readonly replyListeners: ((reply: Reply) => void)[] = [];
  private calls: Promise<unknown> = Promise.resolve();
  private calling: string | null = null;
  private counter = 0;

  constructor(options: ModemAlertOptions) {
    this.modem = options.modem;
    this.ringMs = (options.ringSeconds ?? 30) * 1000;
    this.smsTimeoutMs = options.smsTimeoutMs ?? 60_000;
    this.dialTimeoutMs = options.dialTimeoutMs ?? 30_000;
    this.now = options.now ?? Date.now;
    this.log = options.log ?? (() => {});
    this.modem.onSms((sms) => this.emit({ from: sms.sender, body: sms.body, at: this.now() / 1000 }));
    this.modem.onDtmf((key) => {
      if (this.calling) this.emit({ from: this.calling, body: key, at: this.now() / 1000, dtmf: true });
    });
    this.ready = this.init();
    this.ready.catch((err) => this.log(`the modem did not initialise: ${err instanceof Error ? err.message : String(err)}`));
  }

  private async init(): Promise<void> {
    await this.modem.command('AT');
    await this.modem.command('ATE0');
    await this.modem.command('AT+CMGF=1');
    await this.modem.command('AT+CNMI=2,1,0,0,0');
    await this.modem.command('AT+DDET=1').catch(() => undefined); // SIM800 key-press reports; other modems report them unasked, or not at all
  }

  onReply(listener: (reply: Reply) => void): void {
    this.replyListeners.push(listener);
  }

  private emit(reply: Reply): void {
    for (const listener of this.replyListeners) listener(reply);
  }

  async send(alert: Alert, contact: ServiceContact): Promise<Receipt> {
    await this.ready;
    this.counter += 1;
    const receipt: Receipt = {
      receipt_id: `m-${Math.floor(this.now()).toString(36)}-${this.counter}`,
      service: contact.service,
      channel: contact.channel,
      recipient: contact.recipient,
      sent_at: this.now() / 1000,
    };
    if (contact.channel === 'dashboard') return receipt; // answered on the dashboard; nothing to send
    if (contact.channel !== 'sms' && contact.channel !== 'voice') throw new Error(`the modem cannot deliver the ${contact.channel} channel`);
    if (!isPhoneNumber(contact.recipient)) throw new Error(`${contact.recipient} is not a phone number the modem can reach`);
    const number = normalisePhone(contact.recipient);

    if (contact.channel === 'sms') {
      const lines = await this.modem.command(`AT+CMGS="${number}"`, { body: alertText(alert, contact), timeoutMs: this.smsTimeoutMs });
      if (!lines.some((l) => l.startsWith('+CMGS:'))) throw new Error(`the modem did not confirm the SMS to ${number}`);
      return { ...receipt, delivered_at: this.now() / 1000 };
    }

    const placed = this.calls.then(async () => {
      await this.modem.command(`ATD${number};`, { timeoutMs: this.dialTimeoutMs });
      this.calling = number;
      return this.now() / 1000;
    });
    this.calls = placed
      .then(() => sleep(this.ringMs))
      .catch(() => undefined)
      .then(async () => {
        if (this.calling !== number) return;
        await this.modem.command('ATH').catch(() => undefined);
        this.calling = null;
      });
    return { ...receipt, delivered_at: await placed };
  }

  /** Answers arrive as replies, which the escalation runner matches to receipts. */
  async ack(): Promise<Ack | null> {
    return null;
  }
}
