/**
 * M0 escalation demo: one confirmed alarm, four recipients, one silent service.
 *
 *   npm run demo:escalation
 *
 * Runs entirely against ConsoleAlert with a fake clock, so it needs no
 * telephony account and takes no wall-clock time.
 */
import { ConsoleAlert } from './console-alert.ts';
import { EscalationService, type LedgerEvent } from './escalation.ts';
import type { Alert, SiteRegistry } from './provider.ts';

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
  media: { frames: 3, clip_seconds: 10, hashes: ['3f9a', '8b21', 'c4d0'] },
  ledger_seq: 7,
  ledger_hash: 'c4d0a91f4b2e',
  raised_at: 1789000950,
};

let clock = alert.raised_at;
const events: LedgerEvent[] = [];
const provider = new ConsoleAlert(() => clock);
const service = new EscalationService(provider, {
  ackTimeout: 60,
  now: () => clock,
  onLedgerEvent: (e) => events.push(e),
});

const stamp = (t: number) => new Date(t * 1000).toISOString().slice(11, 19);

console.log('FireX escalation demo — CRITICAL at Hostel, Block B\n');
console.log(`${stamp(clock)}  CRITICAL confirmed: ${alert.hazard} at ${alert.node_id}, ${alert.where}\n`);

const outcomes = await service.raise(alert);

// The fire station answers in 34 seconds.
clock += 34;
const fire = outcomes.find((o) => o.service === 'fire')!;
provider.acknowledge(fire.receipt!.receipt_id, 'station-042');
console.log(`\n${stamp(clock)}  fire station acknowledged`);
await service.poll(alert, outcomes);

// Nobody at the police station picks up; at 60 s the backup is alerted.
clock = alert.raised_at + 61;
console.log(`\n${stamp(clock)}  60 s elapsed — chasing anyone who has not answered\n`);
await service.poll(alert, outcomes);

console.log('\nLedger events written by this incident:');
for (const e of events) {
  const detail = Object.entries(e.payload).map(([k, v]) => `${k}=${v}`).join(' ');
  console.log(`  ${stamp(e.at)}  ${e.type.padEnd(14)} ${detail}`);
}

const answered = outcomes.filter((o) => o.acknowledged).length;
console.log(`\n${answered} of ${outcomes.length} recipients acknowledged; every send, answer and silence is in the log.`);
