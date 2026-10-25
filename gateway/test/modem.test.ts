/**
 * The GSM modem provider against a fake modem that answers AT commands the way
 * a SIM800 or SIM7600 does: an SMS sent and confirmed, a modem that goes
 * silent, a reply that arrives as +CMTI, and a call that rings, reports a key
 * press and is hung up.
 */
import { describe as suite, expect, test } from 'vitest';

import { AtModem, ModemAlert, type ModemPort } from '../escalation/modem.ts';
import type { Alert, Reply, ServiceContact, SiteRegistry } from '../escalation/provider.ts';

class FakeModem implements ModemPort {
  readonly commands: string[] = [];
  readonly bodies: string[] = [];
  silentFor = new Set<string>();
  inbox = new Map<number, { sender: string; body: string }>();
  private listener: (text: string) => void = () => {};
  private awaitingBody = false;

  write(data: string): void {
    if (this.awaitingBody) {
      if (data === '\x1b') {
        this.awaitingBody = false;
        return;
      }
      this.awaitingBody = false;
      this.bodies.push(data.replace(/\x1a$/, ''));
      this.reply('\r\n+CMGS: 17\r\n\r\nOK\r\n');
      return;
    }
    if (data === '\x1b') return;
    const command = data.replace(/\r$/, '');
    this.commands.push(command);
    if ([...this.silentFor].some((prefix) => command.startsWith(prefix))) return;
    if (command.startsWith('AT+CMGS=')) {
      this.awaitingBody = true;
      this.reply('\r\n> ');
      return;
    }
    const read = command.match(/^AT\+CMGR=(\d+)$/);
    if (read) {
      const sms = this.inbox.get(Number(read[1]));
      this.reply(sms ? `\r\n+CMGR: "REC UNREAD","${sms.sender}","","26/09/15,10:00:00+22"\r\n${sms.body}\r\n\r\nOK\r\n` : '\r\nOK\r\n');
      return;
    }
    if (command === 'AT+DDET=1') {
      this.reply('\r\nERROR\r\n'); // a SIM7600 does not know it
      return;
    }
    this.reply('\r\nOK\r\n');
  }

  onData(listener: (text: string) => void): void {
    this.listener = listener;
  }

  async close(): Promise<void> {}

  /** Something the modem says on its own. */
  unsolicited(text: string): void {
    this.reply(text);
  }

  private reply(text: string): void {
    setImmediate(() => this.listener(text));
  }
}

const site: SiteRegistry = {
  site_id: 'site-001', site_name: 'Hostel, Block B', address: '', location: { lat: 0, lon: 0 }, contacts: [],
};
const alert: Alert = {
  incident_id: '7', site, node_id: 'ab'.repeat(16), where: 'Room 206', hazard: 'gas + heat agree', scores: {},
  media: { frames: 0, clip_seconds: 0, hashes: [] }, ledger_seq: 41, ledger_hash: 'cd'.repeat(32), raised_at: 0,
};
const warden: ServiceContact = { service: 'site', name: 'Warden on duty', recipient: '+91 98000 00001', channel: 'sms', mode: 'live' };
const station: ServiceContact = { service: 'fire', name: 'Fire station 17', recipient: '+919800000017', channel: 'voice', mode: 'live' };

const until = async (what: string, predicate: () => boolean, timeoutMs = 2000) => {
  const deadline = Date.now() + timeoutMs;
  while (!predicate()) {
    if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
    await new Promise((r) => setTimeout(r, 2));
  }
};

function setup(options: { ringSeconds?: number; smsTimeoutMs?: number } = {}) {
  const fake = new FakeModem();
  const modem = new AtModem(fake, { timeoutMs: 500 });
  const provider = new ModemAlert({ modem, ringSeconds: options.ringSeconds ?? 0.05, smsTimeoutMs: options.smsTimeoutMs ?? 500 });
  const replies: Reply[] = [];
  provider.onReply((r) => replies.push(r));
  return { fake, modem, provider, replies };
}

suite('the GSM modem provider', () => {
  test('it sets the modem up for text-mode SMS and message indications', async () => {
    const { fake, provider } = setup();
    await provider.ready;
    expect(fake.commands).toEqual(['AT', 'ATE0', 'AT+CMGF=1', 'AT+CNMI=2,1,0,0,0', 'AT+DDET=1']);
  });

  test('an SMS is written after the prompt and delivered when the modem answers +CMGS', async () => {
    const { fake, provider } = setup();
    const receipt = await provider.send(alert, warden);
    expect(fake.commands).toContain('AT+CMGS="+919800000001"');
    expect(fake.bodies).toEqual(['FIREX CRITICAL Hostel, Block B: Room 206 gas + heat agree. Reply 1 to acknowledge.']);
    expect(receipt).toMatchObject({ service: 'site', channel: 'sms', recipient: '+91 98000 00001' });
    expect(receipt.receipt_id).toMatch(/^m-/);
    expect(receipt.delivered_at).toBeTypeOf('number');
  });

  test('a drill says so first, and stays within one SMS', async () => {
    const { fake, provider } = setup();
    await provider.send({ ...alert, kind: 'drill', site: { ...site, site_name: 'A very long hostel name that goes on and on, Block B, East Campus' } }, warden);
    expect(fake.bodies[0].startsWith('DRILL - NOT A REAL INCIDENT. FIREX ')).toBe(true);
    expect(fake.bodies[0].length).toBeLessThanOrEqual(160);
    expect(fake.bodies[0].endsWith('Reply 1 to acknowledge.')).toBe(true);
  });

  test('a modem that does not answer fails the send instead of hanging it, and the next command still runs', async () => {
    const { fake, provider } = setup({ smsTimeoutMs: 80 });
    await provider.ready;
    fake.silentFor.add('AT+CMGS=');
    await expect(provider.send(alert, warden)).rejects.toThrow(/did not answer AT\+CMGS="\+919800000001" within 0.08 s/);
    fake.silentFor.clear();
    await expect(provider.send(alert, warden)).resolves.toMatchObject({ channel: 'sms' });
  });

  test('a reply is read from the SIM, reported, and deleted; a body of "OK" is not mistaken for the modem', async () => {
    const { fake, provider, replies } = setup();
    await provider.ready;
    fake.inbox.set(3, { sender: '+919800000001', body: 'OK' });
    fake.unsolicited('\r\n+CMTI: "SM",3\r\n');
    await until('the reply', () => replies.length === 1);
    expect(replies[0]).toMatchObject({ from: '+919800000001', body: 'OK' });
    await until('the delete', () => fake.commands.includes('AT+CMGD=3'));
  });

  test('a voice alert dials, counts as delivered when placed, reports a key press, and hangs up', async () => {
    const { fake, provider, replies } = setup({ ringSeconds: 0.1 });
    const receipt = await provider.send(alert, station);
    expect(fake.commands).toContain('ATD+919800000017;');
    expect(receipt.delivered_at).toBeTypeOf('number');
    fake.unsolicited('\r\n+DTMF: 1\r\n');
    await until('the key press', () => replies.length === 1);
    expect(replies[0]).toMatchObject({ from: '+919800000017', body: '1', dtmf: true });
    await until('the hang-up', () => fake.commands.includes('ATH'));
  });

  test('channels the modem cannot carry, and recipients that are not numbers, are refused', async () => {
    const { provider } = setup();
    await expect(provider.send(alert, { ...warden, channel: 'api' })).rejects.toThrow(/cannot deliver the api channel/);
    await expect(provider.send(alert, { ...warden, recipient: 'warden-1' })).rejects.toThrow(/not a phone number/);
    await expect(provider.send(alert, { ...warden, channel: 'dashboard' })).resolves.toMatchObject({ channel: 'dashboard' });
  });
});
