/**
 * Heartbeat gaps: when a unit went quiet, and for how long.
 *
 * A unit publishes telemetry every 10 s and its state every 60 s, so 90 s with
 * no message from it opens a gap (the dashboard's online flag uses the same
 * 90 s). A gap longer than the registry's offline_notify_min sends the site one
 * notice. When the unit is heard again the gap closes and becomes a signed
 * heartbeat_gap entry in the site ledger, from the last message before the
 * silence to the first one after it.
 *
 * The unit keeps alarming through a gap; what a gap records is how long the
 * site could not see it. Limits, stated: gaps are measured by the gateway, so
 * the gateway's own downtime shows as a gap for every unit, and the opening of
 * a gap is found by a check every 10 s.
 */
import type { Db, Row } from './db.ts';
import type { LiveBus } from './live.ts';
import type { SiteLedger } from './site-ledger.ts';

export interface HeartbeatOptions {
  db: Db;
  live: LiveBus;
  siteLedger?: SiteLedger | null;
  /** Sends the site a notice; absent when escalation is off. */
  notify?: (text: string) => Promise<unknown>;
  notifyAfterMin?: number;
  silentMs?: number;
  checkMs?: number;
  /** Unix milliseconds. */
  now?: () => number;
  log?: (line: string) => void;
}

/** Live events that mean a message from the unit itself arrived. */
const FROM_UNIT = new Set(['state', 'telemetry', 'entry', 'media']);

interface Gap {
  id: number;
  start_ms: number;
  notified_at_ms: number | null;
}

export class HeartbeatMonitor {
  private readonly db: Db;
  private readonly live: LiveBus;
  private readonly siteLedger: SiteLedger | null;
  private readonly notify: ((text: string) => Promise<unknown>) | null;
  private readonly notifyAfterMs: number;
  private readonly silentMs: number;
  private readonly checkMs: number;
  private readonly now: () => number;
  private readonly log: (line: string) => void;
  private timer: NodeJS.Timeout | null = null;
  private unsubscribe: (() => void) | null = null;

  constructor(options: HeartbeatOptions) {
    this.db = options.db;
    this.live = options.live;
    this.siteLedger = options.siteLedger ?? null;
    this.notify = options.notify ?? null;
    this.notifyAfterMs = (options.notifyAfterMin ?? 10) * 60_000;
    this.silentMs = options.silentMs ?? 90_000;
    this.checkMs = options.checkMs ?? 10_000;
    this.now = options.now ?? Date.now;
    this.log = options.log ?? (() => {});
  }

  start(): void {
    this.unsubscribe ??= this.live.subscribe((event) => {
      if (FROM_UNIT.has(event.kind) && 'device_id' in event && event.device_id) this.heard(event.device_id);
    });
    this.timer ??= setInterval(() => this.check(), this.checkMs);
    this.timer.unref?.();
  }

  stop(): void {
    this.unsubscribe?.();
    this.unsubscribe = null;
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }

  /** Open gaps for units silent too long, notify the site about long ones, close gaps for units heard since. */
  check(): void {
    const now = this.now();
    for (const device of this.db.prepare('SELECT * FROM devices').all() as Row[]) {
      if (device.removed_at !== undefined && device.removed_at !== null) continue;
      if (device.last_seen === null || device.last_seen === undefined) continue;
      const id = String(device.id);
      const lastSeen = Number(device.last_seen);
      const gap = this.openGap(id);
      if (gap) {
        if (lastSeen > gap.start_ms) this.close(gap, id, lastSeen);
        else if (gap.notified_at_ms === null && now - gap.start_ms >= this.notifyAfterMs) this.notice(gap, device);
      } else if (now - lastSeen >= this.silentMs) {
        this.db.prepare('INSERT INTO heartbeat_gaps (device_id, start_ms) VALUES (?, ?)').run(id, lastSeen);
        this.live.publish({ kind: 'gap', device_id: id, open: true });
        this.log(`unit ${String(device.label || id.slice(0, 8))} silent since ${new Date(lastSeen).toISOString()}`);
      }
    }
  }

  /** A message from the unit: any open gap ends now. */
  heard(deviceId: string): void {
    const gap = this.openGap(deviceId);
    if (gap) this.close(gap, deviceId, this.now());
  }

  private openGap(deviceId: string): Gap | null {
    const row = this.db.prepare('SELECT id, start_ms, notified_at_ms FROM heartbeat_gaps WHERE device_id = ? AND end_ms IS NULL ORDER BY id DESC LIMIT 1')
      .get(deviceId) as Row | undefined;
    return row ? { id: Number(row.id), start_ms: Number(row.start_ms), notified_at_ms: row.notified_at_ms === null ? null : Number(row.notified_at_ms) } : null;
  }

  private close(gap: Gap, deviceId: string, endMs: number): void {
    const end = Math.max(endMs, gap.start_ms);
    const info = this.db.prepare('UPDATE heartbeat_gaps SET end_ms = ? WHERE id = ? AND end_ms IS NULL').run(end, gap.id);
    if (!info.changes) return;
    if (this.siteLedger) {
      try {
        this.siteLedger.append('heartbeat_gap', {
          unit: deviceId,
          start_ts: Math.floor(gap.start_ms / 1000),
          end_ts: Math.floor(end / 1000),
          seconds: Math.round((end - gap.start_ms) / 1000),
        });
      } catch (err) {
        this.log(`site ledger: heartbeat_gap not written: ${err instanceof Error ? err.message : String(err)}`);
      }
    }
    this.live.publish({ kind: 'gap', device_id: deviceId, open: false });
    this.log(`unit ${deviceId.slice(0, 8)} heard again after ${Math.round((end - gap.start_ms) / 1000)} s`);
  }

  private notice(gap: Gap, device: Row): void {
    this.db.prepare('UPDATE heartbeat_gaps SET notified_at_ms = ? WHERE id = ?').run(this.now(), gap.id);
    if (!this.notify) return;
    const site = this.db.prepare('SELECT timezone FROM sites LIMIT 1').get() as Row | undefined;
    let since: string;
    try {
      since = new Date(gap.start_ms).toLocaleString('en-IN', {
        timeZone: String(site?.timezone ?? 'Asia/Kolkata'), day: 'numeric', month: 'short', hour: '2-digit', minute: '2-digit', hour12: false,
      });
    } catch {
      since = new Date(gap.start_ms).toISOString().slice(0, 16).replace('T', ' ');
    }
    const label = String(device.label || String(device.id).slice(0, 8));
    void this.notify(`FireX: unit ${label} offline since ${since}`);
  }
}
