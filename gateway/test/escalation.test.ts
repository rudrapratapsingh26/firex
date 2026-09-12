import { beforeEach, describe as suite, expect, test } from 'vitest';

import { ConsoleAlert } from '../escalation/console-alert.ts';
import { EscalationService, type LedgerEvent } from '../escalation/escalation.ts';
import type { Alert, SiteRegistry } from '../escalation/provider.ts';

/** A site mapped at installation: one fire station, one police station, one ambulance. */
const site: SiteRegistry = {
  site_id: 'site-001',
  site_name: 'Hostel, Block B',
  address: 'Block B, example campus',
  location: { lat: 0, lon: 0 },
  contacts: [
    { service: 'site', name: 'Warden on duty', recipient: 'warden-1', channel: 'sms' },
    {
      service: 'fire', name: 'Fire station 42', recipient: 'station-042', channel: 'sms',
      backup: { name: 'Fire station 17', recipient: 'station-017', channel: 'voice' },
    },
    { service: 'police', name: 'Police station 17', recipient: 'ps-017', channel: 'sms' },
    { service: 'ambulance', name: 'EMS north', recipient: 'ems-north', channel: 'api' },
  ],
};

const alert: Alert = {
  incident_id: 'inc-001',
  site,
  node_id: 'N-06',
  where: 'Floor 2 · Room 206',
  hazard: 'gas + heat agree',
  scores: { gas: 81, heat: 64 },
  media: { frames: 3, clip_seconds: 10, hashes: ['aa', 'bb', 'cc'] },
  ledger_seq: 7,
  ledger_hash: 'c4d0a91f',
  raised_at: 1789000950,
};

let clock = 1789000950;
let events: LedgerEvent[];
let provider: ConsoleAlert;
let service: EscalationService;

beforeEach(() => {
  clock = 1789000950;
  events = [];
  provider = new ConsoleAlert(() => clock, () => {});
  service = new EscalationService(provider, {
    ackTimeout: 60,
    now: () => clock,
    onLedgerEvent: (e) => events.push(e),
  });
});

suite('escalation', () => {
  test('one confirmed alarm reaches the site and all three services', async () => {
    const outcomes = await service.raise(alert);
    expect(outcomes.map((o) => o.service)).toEqual(['site', 'fire', 'police', 'ambulance']);
    expect(events.filter((e) => e.type === 'alert_sent')).toHaveLength(4);
  });

  test('every send is written to the ledger with its receipt', async () => {
    await service.raise(alert);
    const fire = events.find((e) => e.payload.service === 'fire');
    expect(fire).toMatchObject({ type: 'alert_sent', payload: { recipient: 'station-042', channel: 'sms' } });
    expect(String(fire?.payload.receipt_id)).toMatch(/^r-\d{4}$/);
  });

  test('an acknowledgement records who answered and how long they took', async () => {
    const outcomes = await service.raise(alert);
    const fire = outcomes.find((o) => o.service === 'fire')!;
    clock += 34;
    provider.acknowledge(fire.receipt.receipt_id, 'station-042');
    await service.poll(alert, outcomes);

    expect(fire.acknowledged).toBe(true);
    expect(fire.latency_s).toBe(34);
    expect(events.find((e) => e.type === 'alert_ack')).toMatchObject({
      payload: { service: 'fire', by: 'station-042', latency_s: 34 },
    });
  });

  test('silence for 60 s triggers a re-alert, and the silence is logged', async () => {
    const outcomes = await service.raise(alert);
    clock += 61;
    await service.poll(alert, outcomes);

    const timeout = events.find((e) => e.type === 'alert_timeout' && e.payload.service === 'fire');
    expect(timeout).toMatchObject({ payload: { action: 're_alert', elapsed_s: 61 } });

    // the re-alert went to the backup station named in the site registry
    const retry = events.find((e) => e.type === 'alert_sent' && e.payload.recipient === 'station-017');
    expect(retry).toBeTruthy();
  });

  test('nothing is re-alerted before the timeout', async () => {
    const outcomes = await service.raise(alert);
    clock += 59;
    await service.poll(alert, outcomes);
    expect(events.filter((e) => e.type === 'alert_timeout')).toHaveLength(0);
  });

  test('a service that answers is not chased again', async () => {
    const outcomes = await service.raise(alert);
    const police = outcomes.find((o) => o.service === 'police')!;
    clock += 20;
    provider.acknowledge(police.receipt.receipt_id, 'ps-017');
    await service.poll(alert, outcomes);
    clock += 120;
    await service.poll(alert, outcomes);

    const chased = events.filter((e) => e.type === 'alert_timeout' && e.payload.service === 'police');
    expect(chased).toHaveLength(0);
  });

  test('after the re-alert limit the outcome escalates rather than looping', async () => {
    const outcomes = await service.raise(alert);
    clock += 61;
    await service.poll(alert, outcomes);
    clock += 61;
    await service.poll(alert, outcomes);

    const actions = events
      .filter((e) => e.type === 'alert_timeout' && e.payload.service === 'ambulance')
      .map((e) => e.payload.action);
    expect(actions).toEqual(['re_alert', 'escalate']);
  });
});
