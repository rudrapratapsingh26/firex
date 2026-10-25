import type { Device, Entry, SiteEntry } from './api.ts';

export type Condition = 'critical' | 'silenced' | 'elevated' | 'fault' | 'selftest' | 'warmup' | 'safe' | 'offline' | 'unknown';

/** Most urgent first. */
export const SEVERITY: Condition[] = ['critical', 'silenced', 'elevated', 'fault', 'offline', 'selftest', 'warmup', 'unknown', 'safe'];

export const CONDITION_TEXT: Record<Condition, string> = {
  critical: 'Critical',
  silenced: 'Critical, silenced',
  elevated: 'Elevated',
  fault: 'Fault',
  selftest: 'Self-test',
  warmup: 'Warming up',
  safe: 'Safe',
  offline: 'Offline',
  unknown: 'No report yet',
};

const ALARM: Record<string, Condition> = {
  CRITICAL: 'critical',
  SILENCED: 'silenced',
  ELEVATED: 'elevated',
  FAULT: 'fault',
  SELF_TEST: 'selftest',
  WARMUP: 'warmup',
  BOOT: 'warmup',
  NORMAL: 'safe',
};

export function condition(device: Device): Condition {
  if (!device.last_seen) return 'unknown';
  if (!device.online || device.offline_since) return 'offline';
  const c = ALARM[device.alarm ?? ''] ?? ALARM[device.state ?? ''] ?? 'unknown';
  if (c === 'safe' && Object.values(device.faults ?? {}).some(Boolean)) return 'fault';
  return c;
}

export function worst(devices: Device[]): Condition {
  let best = SEVERITY.length - 1;
  for (const d of devices) best = Math.min(best, SEVERITY.indexOf(condition(d)));
  return devices.length ? SEVERITY[best] : 'unknown';
}

export const MODALITY: Record<string, string> = { gas: 'Gas', smoke: 'Smoke', flame: 'Flame', heat: 'Heat', humidity: 'Humidity' };

export function modalityList(keys: unknown): string {
  const names = (Array.isArray(keys) ? keys : []).map((k) => (MODALITY[String(k)] ?? String(k)).toLowerCase());
  if (names.length === 0) return '';
  if (names.length === 1) return names[0];
  return `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}`;
}

const STATE_WORD: Record<string, string> = {
  WARMUP: 'warming up', NORMAL: 'safe', ELEVATED: 'elevated', CRITICAL: 'critical',
};

const RESET: Record<string, string> = {
  power_on: 'power on', watchdog: 'watchdog reset', brownout: 'brownout', panic: 'crash', software: 'restart', external: 'reset button',
};

const s = (v: unknown) => String(v ?? '');
const cap = (text: string) => (text ? text[0].toUpperCase() + text.slice(1) : text);

/** A firmware update step in words, from a fw_update entry or the unit's firmware history. */
export function fwUpdateText(u: { phase?: unknown; version?: unknown; from_version?: unknown; reason?: unknown }): string {
  const version = s(u.version);
  const from = s(u.from_version);
  const why = s(u.reason);
  switch (u.phase) {
    case 'accepted':
      return `Accepted firmware ${version}${from ? ` to replace ${from}` : ''}`;
    case 'downloaded':
      return `Downloaded firmware ${version} and checked its signature`;
    case 'confirmed':
      return `Running firmware ${version}, confirmed`;
    case 'failed':
      return `Firmware ${version} was not installed${why ? `: ${why}` : ''}`;
    case 'rolled_back':
      return `Firmware ${version} was not confirmed; the unit went back to its previous firmware${why ? ` (${why})` : ''}`;
    default:
      return `Firmware ${version}: ${s(u.phase)}`;
  }
}

/** A ledger entry in words, for the timeline and node pages. */
export function describeEntry(e: Entry): string {
  const p = e.payload ?? {};
  switch (e.type) {
    case 'boot':
      return `Started after ${RESET[s(p.reset_reason)] ?? s(p.reset_reason)}, firmware ${s(p.fw_version)}`;
    case 'self_test': {
      const failed = Object.entries((p.checks ?? {}) as Record<string, boolean>).filter(([, ok]) => !ok).map(([name]) => name.replace('_', ' '));
      return p.result === 'pass' ? 'Self-test passed' : `Self-test failed: ${failed.join(', ') || 'see entry'}`;
    }
    case 'threshold_cross':
      return p.direction === 'up'
        ? `${MODALITY[s(p.modality)] ?? s(p.modality)} rose past its threshold, score ${s(p.score)}`
        : `${MODALITY[s(p.modality)] ?? s(p.modality)} fell back below its threshold`;
    case 'state_change': {
      const reason = modalityList(p.reason);
      const to = STATE_WORD[s(p.to)] ?? s(p.to).toLowerCase();
      return `Changed from ${STATE_WORD[s(p.from)] ?? s(p.from).toLowerCase()} to ${to}${reason ? ` on ${reason}` : ''}`;
    }
    case 'media_captured': {
      const n = Array.isArray(p.frames) ? p.frames.length : 0;
      return n ? `${n} ${n === 1 ? 'photo' : 'photos'} taken on the critical alarm` : 'The camera was asked for photos and produced none';
    }
    case 'silence':
      return `Silenced with the button for ${Math.round(Number(p.seconds ?? 0) / 60)} minutes`;
    case 'calibration':
      return `${MODALITY[s(p.modality)] ?? s(p.modality)} clean-air baseline set to ${s(p.r0)} Ω`;
    case 'config_change':
      return `Setting ${s(p.key)} changed from ${s(p.old)} to ${s(p.new)}`;
    case 'fan_state':
      return p.state === 'failed' ? 'Fan stopped while it should be running' : `Fan ${s(p.state)}, ${s(p.duty)}% duty`;
    case 'fault':
      return p.state === 'raised'
        ? `${MODALITY[s(p.source)] ?? s(p.source)} sensor stopped reporting`
        : `${MODALITY[s(p.source)] ?? s(p.source)} sensor reporting again`;
    case 'log_overwrite':
      return `Entries ${s(p.first_seq)} to ${s(p.last_seq)} were overwritten on the unit before reaching the gateway`;
    case 'fw_update':
      return fwUpdateText(p);
    case 'trace_captured':
      return `Recorded a score trace of ${s(p.samples)} samples around the change to ${STATE_WORD[s(p.trigger)] ?? s(p.trigger).toLowerCase()}`;
    case 'clock_sync':
      return `Clock set from the gateway, ${p.direction === 'back' ? 'held back' : 'moved forward'} ${duration(Number(p.step_ms ?? 0) / 1000)}`;
    default:
      return e.type.replace(/_/g, ' ');
  }
}

export const VERIFY_TEXT: Record<string, string> = {
  ok: 'Verified',
  start: 'Verified, first entry received',
  gap: 'Verified, after missing entries',
  chain: 'Contents do not match their hash',
  signature: 'Signature does not match this unit',
};

export const CHAIN_TEXT: Record<string, string> = {
  empty: 'Nothing received yet',
  ok: 'Complete and verified',
  partial: 'Verified from the first entry the gateway received',
  gap: 'Verified, with entries missing',
  broken: 'Tampering detected',
};

export const shortHash = (hash: string) => hash.slice(0, 10);

const dateTime = new Intl.DateTimeFormat(undefined, { day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit', second: '2-digit' });
const timeOnly = new Intl.DateTimeFormat(undefined, { hour: '2-digit', minute: '2-digit', second: '2-digit' });

export const atMs = (ms: number | null | undefined) => (ms ? dateTime.format(new Date(ms)) : 'never');
export const atSeconds = (sec: number | null | undefined) => (sec ? dateTime.format(new Date(sec * 1000)) : 'never');
export const clockMs = (ms: number) => timeOnly.format(new Date(ms));

export function ago(ms: number | null | undefined): string {
  if (!ms) return 'never';
  const seconds = Math.max(0, Math.round((Date.now() - ms) / 1000));
  if (seconds < 60) return `${seconds} s ago`;
  if (seconds < 3600) return `${Math.round(seconds / 60)} min ago`;
  if (seconds < 86_400) return `${Math.round(seconds / 3600)} h ago`;
  return `${Math.round(seconds / 86_400)} days ago`;
}

/** 34 s, 2 min 5 s, 1 h 3 min. */
export function duration(seconds: number | null | undefined): string {
  if (seconds === null || seconds === undefined || !Number.isFinite(seconds)) return '–';
  const t = Math.max(0, Math.round(seconds));
  if (t < 60) return `${t} s`;
  if (t < 3600) {
    const rest = t % 60;
    return rest ? `${Math.floor(t / 60)} min ${rest} s` : `${t / 60} min`;
  }
  const minutes = Math.round((t % 3600) / 60);
  return minutes ? `${Math.floor(t / 3600)} h ${minutes} min` : `${Math.floor(t / 3600)} h`;
}

export function bytes(n: number): string {
  if (n >= 1_048_576) return `${(n / 1_048_576).toFixed(2)} MB`;
  if (n >= 1024) return `${Math.round(n / 1024)} KB`;
  return `${n} B`;
}

export const unitName = (d: { label?: string | null; id: string }) => d.label || `Unit ${d.id.slice(0, 6)}`;

/* ---- escalation ------------------------------------------------------------------ */

/** The four recipients of a critical alarm, in the order the dashboard lists them. */
export const SERVICES: string[] = ['site', 'fire', 'police', 'ambulance'];

export const SERVICE_TEXT: Record<string, string> = {
  site: 'Site contact',
  fire: 'Fire station',
  police: 'Police station',
  ambulance: 'Ambulance',
};

export const CHANNEL_TEXT: Record<string, string> = {
  sms: 'SMS',
  voice: 'voice call',
  api: 'API',
  console: 'gateway console',
  dashboard: 'dashboard',
};

export const LABEL_TEXT: Record<string, string> = { true: 'real hazard', nuisance: 'nuisance', test: 'test' };

/** Phone numbers show their last four digits only; station codes and endpoint names show as they are. */
export function maskRecipient(recipient: string): string {
  if (!recipient || recipient.includes('•')) return recipient;
  if (!/^\+?[\d\s-]{6,}$/.test(recipient)) return recipient;
  const digits = recipient.replace(/\D/g, '');
  return `••••${digits.slice(-4)}`;
}

export const TS_SRC_TEXT: Record<string, string> = {
  ntp: 'Synchronised',
  gateway: 'Set from the gateway',
  rtc: 'Real-time clock',
  uncertain: 'Uncertain',
};

/* ---- firmware -------------------------------------------------------------------- */

export const SIGNATURE_TEXT: Record<string, string> = {
  verified: 'Signature verified',
  present: 'Signed, key not checked',
  missing: 'Not signed',
  wrong_key: 'Signed with an untrusted key',
};

export const ROLLOUT_STATE_TEXT: Record<string, string> = {
  running: 'In progress',
  done: 'Finished',
  failed: 'Stopped on a failure',
  stopped: 'Stopped',
};

export const ROLLOUT_UNIT_TEXT: Record<string, string> = {
  pending: 'Waiting its turn',
  offered: 'Offered to the unit',
  downloaded: 'Downloaded, restarting when safe',
  confirmed: 'Running and confirmed',
  rolled_back: 'Not confirmed, back on its previous firmware',
  failed: 'Failed',
  skipped: 'Skipped',
};

/* ---- site record ------------------------------------------------------------------- */

/** A site-chain entry in words. `names` maps unit IDs to their labels. */
export function describeSiteEntry(e: SiteEntry, names: Record<string, string> = {}): string {
  const p = e.payload ?? {};
  const unit = p.unit ? (names[s(p.unit)] ?? `Unit ${s(p.unit).slice(0, 6)}`) : 'a unit';
  const service = SERVICE_TEXT[s(p.service)] ?? s(p.service);
  const drill = p.drill === true ? 'Drill: ' : '';
  const incident = p.incident !== undefined && p.incident !== null ? `incident ${s(p.incident)}` : 'an incident';
  const channel = CHANNEL_TEXT[s(p.channel)] ?? s(p.channel);
  switch (e.type) {
    case 'alert_sent':
      return `${drill}${service} ${p.retry_of ? 'alerted again' : 'alerted'}${channel ? ` by ${channel}` : ''}${p.unit ? ` about ${incident} at ${unit}` : ''}`;
    case 'alert_ack':
      return `${drill}${service} acknowledged${p.by ? ` (${s(p.by)})` : ''} after ${duration(Number(p.latency_s))}`;
    case 'alert_timeout':
      return `${drill}${service} did not answer within ${duration(Number(p.elapsed_s))}; ${p.action === 're_alert' ? 'alerted again' : 'no more re-alerts, raised to the site'}`;
    case 'heartbeat_gap':
      return `${cap(unit)} did not report for ${duration(Number(p.seconds))}`;
    case 'incident_label':
      return `${cap(incident)} at ${unit} labelled ${LABEL_TEXT[s(p.label)] ?? s(p.label)} by ${s(p.by)}`;
    case 'drill': {
      const who = Array.isArray(p.services) ? `: ${p.services.map((x) => SERVICE_TEXT[String(x)] ?? String(x)).join(', ')}` : '';
      return p.phase === 'finished' ? `Drill ${s(p.drill_id)} finished` : `Drill ${s(p.drill_id)} started by ${s(p.by)}${who}`;
    }
    case 'registry_change':
      return `Service registry changed by ${s(p.by)}`;
    case 'fw_offered':
      return `Firmware ${s(p.version)} offered to ${unit} by ${s(p.by)}`;
    case 'unit_removed':
      return `${cap(unit)} removed and its certificate revoked by ${s(p.by)}`;
    case 'escalation_withheld': {
      const why = p.reason === 'late'
        ? 'the alarm reached the gateway late, so only the site was alerted'
        : p.reason === 'verification'
          ? 'its alarm entry failed verification'
          : s(p.reason);
      return `Services not alerted for ${incident} at ${unit}: ${why}`;
    }
    default:
      return e.type.replace(/_/g, ' ');
  }
}
