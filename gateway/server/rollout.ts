/**
 * Firmware rollouts: one unit at a time, each confirmed before the next is
 * offered the image.
 *
 * The gateway sends a unit an `ota` command and then follows only what the unit
 * writes in its own ledger: `fw_update` accepted, downloaded, then confirmed by
 * the new image within 60 seconds of booting it, or failed, and rolled_back once
 * the old image is running again. A confirmed unit lets the rollout move on. A
 * failed or rolled-back unit stops the rollout, and the units still waiting are
 * skipped, so one bad image reaches one unit. A unit that makes no progress for
 * 15 minutes counts as failed too.
 *
 * A removed unit, an offline one (nothing heard for 90 s) and one already running
 * the version are skipped with the reason. Each offer is a `fw_offered` entry in
 * the site ledger. State lives in the rollouts and rollout_units tables, so a
 * restarted gateway picks up where it was.
 */
import { fromHex } from '../../ledger/src/cbor.ts';
import { audit, type Db, type Row } from './db.ts';
import { mayRollOut } from './firmware.ts';
import { cmdTopic, type Publish } from './ingest.ts';
import type { LiveBus } from './live.ts';
import type { SiteLedger } from './site-ledger.ts';

export type UnitState = 'pending' | 'offered' | 'downloaded' | 'confirmed' | 'rolled_back' | 'failed' | 'skipped';

export const UNIT_TIMEOUT_MS = 15 * 60_000;
const ONLINE_MS = 90_000;

export interface RolloutOptions {
  db: Db;
  live: LiveBus;
  publish: Publish;
  siteLedger?: SiteLedger | null;
  /** The host and port units download from: the advertised gateway address and FIREX_FW_PORT. */
  fwHost: string;
  fwPort: number;
  now?: () => number;
  unitTimeoutMs?: number;
  log?: (line: string) => void;
}

export interface RolloutView {
  id: number;
  sha256: string;
  version: string;
  state: string;
  started_at: number;
  finished_at: number | null;
  by: string;
  units: { id: string; label: string; state: UnitState; detail: string; updated_at: number }[];
}

export class RolloutError extends Error {}

export class RolloutManager {
  private readonly db: Db;
  private readonly live: LiveBus;
  private readonly publish: Publish;
  private readonly siteLedger: SiteLedger | null;
  private readonly fwHost: string;
  private readonly fwPort: number;
  private readonly now: () => number;
  private readonly unitTimeoutMs: number;
  private readonly log: (line: string) => void;

  constructor(options: RolloutOptions) {
    this.db = options.db;
    this.live = options.live;
    this.publish = options.publish;
    this.siteLedger = options.siteLedger ?? null;
    this.fwHost = options.fwHost;
    this.fwPort = options.fwPort;
    this.now = options.now ?? Date.now;
    this.unitTimeoutMs = options.unitTimeoutMs ?? UNIT_TIMEOUT_MS;
    this.log = options.log ?? (() => {});
  }

  /** Start a rollout. With advance false (the CLI, which has no broker connection) the running gateway sends the first offer at its next tick. */
  start(sha: string, units: 'all' | string[], by: string, advance = true): number {
    const firmware = this.db.prepare('SELECT * FROM firmware WHERE sha256 = ?').get(sha) as Row | undefined;
    if (!firmware) throw new RolloutError(`no firmware ${sha} on this gateway`);
    if (!mayRollOut(String(firmware.signature))) {
      throw new RolloutError(`the image's signature is ${firmware.signature}: a unit would refuse it`);
    }
    const running = this.db.prepare("SELECT id FROM rollouts WHERE state = 'running'").get() as Row | undefined;
    if (running) throw new RolloutError(`rollout ${running.id} is still running; stop it first`);

    let ids: string[];
    if (units === 'all') {
      ids = (this.db.prepare('SELECT id FROM devices WHERE removed_at IS NULL ORDER BY label, id').all() as Row[]).map((r) => String(r.id));
    } else {
      ids = [...new Set(units)];
      const unknown = ids.filter((id) => !this.db.prepare('SELECT 1 FROM devices WHERE id = ?').get(id));
      if (unknown.length) throw new RolloutError(`no such unit: ${unknown.join(', ')}`);
    }
    if (!ids.length) throw new RolloutError('no units to update');

    const now = this.now();
    const info = this.db.prepare("INSERT INTO rollouts (sha256, state, started_at, by) VALUES (?, 'running', ?, ?)").run(sha, now, by);
    const id = Number(info.lastInsertRowid);
    ids.forEach((device, position) => {
      this.db.prepare("INSERT INTO rollout_units (rollout_id, device_id, position, state, detail, updated_at) VALUES (?, ?, ?, 'pending', '', ?)")
        .run(id, device, position, now);
    });
    audit(this.db, by, 'rollout_started', { rollout: id, sha256: sha, version: firmware.version, units: ids.length }, now);
    this.log(`rollout ${id}: ${firmware.version} to ${ids.length} unit(s), one at a time`);
    if (advance) this.advance(id);
    return id;
  }

  stop(id: number, by: string): boolean {
    const rollout = this.db.prepare('SELECT state FROM rollouts WHERE id = ?').get(id) as Row | undefined;
    if (!rollout || rollout.state !== 'running') return false;
    this.finish(id, 'stopped', `rollout stopped by ${by}`);
    audit(this.db, by, 'rollout_stopped', { rollout: id }, this.now());
    return true;
  }

  /** A unit's fw_update entry was stored. Units are updated whatever the rollout's state, so the record stays true. */
  onFwUpdate(deviceId: string, payload: Record<string, unknown>): void {
    const phase = String(payload.phase ?? '');
    const rows = this.db.prepare(
      `SELECT u.rollout_id, u.state, r.sha256, r.state AS rollout_state, f.version FROM rollout_units u
       JOIN rollouts r ON r.id = u.rollout_id JOIN firmware f ON f.sha256 = r.sha256
       WHERE u.device_id = ? AND u.state IN ('offered', 'downloaded', 'failed') ORDER BY u.rollout_id DESC`,
    ).all(deviceId) as Row[];
    for (const row of rows) {
      const image = typeof payload.image === 'string' ? payload.image : null;
      if (image ? image !== row.sha256 : String(payload.version ?? '') !== row.version) continue;
      const rolloutId = Number(row.rollout_id);
      const reason = String(payload.reason ?? '');
      if (row.state === 'failed') {
        if (phase === 'rolled_back') this.setUnit(rolloutId, deviceId, 'rolled_back', reason || 'the old image is running again');
        return;
      }
      if (phase === 'accepted') {
        this.setUnit(rolloutId, deviceId, row.state as UnitState, 'accepted');
      } else if (phase === 'downloaded') {
        this.setUnit(rolloutId, deviceId, 'downloaded', 'downloaded and checked; installs when the unit is quiet');
      } else if (phase === 'confirmed') {
        this.setUnit(rolloutId, deviceId, 'confirmed', `running ${row.version}`);
        if (row.rollout_state === 'running') this.advance(rolloutId);
      } else if (phase === 'failed' || phase === 'rolled_back') {
        this.setUnit(rolloutId, deviceId, phase === 'failed' ? 'failed' : 'rolled_back', reason || phase);
        if (row.rollout_state === 'running') this.finish(rolloutId, 'failed', `rollout stopped: ${deviceId.slice(0, 8)} ${phase}`);
      }
      return;
    }
  }

  /** Timeouts and progress. Call on a timer, and once at start to resume. */
  tick(): void {
    for (const row of this.db.prepare("SELECT id FROM rollouts WHERE state = 'running'").all() as Row[]) this.advance(Number(row.id));
  }

  list(limit = 20): RolloutView[] {
    const rollouts = this.db.prepare(
      'SELECT r.*, f.version FROM rollouts r LEFT JOIN firmware f ON f.sha256 = r.sha256 ORDER BY r.id DESC LIMIT ?',
    ).all(limit) as Row[];
    return rollouts.map((r) => ({
      id: Number(r.id),
      sha256: String(r.sha256),
      version: String(r.version ?? ''),
      state: String(r.state),
      started_at: Number(r.started_at),
      finished_at: r.finished_at === null ? null : Number(r.finished_at),
      by: String(r.by),
      units: (this.db.prepare(
        'SELECT u.device_id, u.state, u.detail, u.updated_at, d.label FROM rollout_units u LEFT JOIN devices d ON d.id = u.device_id WHERE u.rollout_id = ? ORDER BY u.position',
      ).all(Number(r.id)) as Row[]).map((u) => ({
        id: String(u.device_id), label: String(u.label ?? ''), state: String(u.state) as UnitState, detail: String(u.detail), updated_at: Number(u.updated_at),
      })),
    }));
  }

  private setUnit(rolloutId: number, deviceId: string, state: UnitState, detail: string): void {
    this.db.prepare('UPDATE rollout_units SET state = ?, detail = ?, updated_at = ? WHERE rollout_id = ? AND device_id = ?')
      .run(state, detail, this.now(), rolloutId, deviceId);
    this.live.publish({ kind: 'rollout', id: rolloutId });
  }

  private finish(id: number, state: 'done' | 'failed' | 'stopped', why: string): void {
    this.db.prepare("UPDATE rollout_units SET state = 'skipped', detail = ?, updated_at = ? WHERE rollout_id = ? AND state = 'pending'")
      .run(why, this.now(), id);
    this.db.prepare('UPDATE rollouts SET state = ?, finished_at = ? WHERE id = ?').run(state, this.now(), id);
    this.log(`rollout ${id}: ${state}${state === 'done' ? '' : ` (${why})`}`);
    this.live.publish({ kind: 'rollout', id });
  }

  private advance(id: number): void {
    const rollout = this.db.prepare('SELECT r.*, f.version, f.size FROM rollouts r JOIN firmware f ON f.sha256 = r.sha256 WHERE r.id = ?').get(id) as Row | undefined;
    if (!rollout || rollout.state !== 'running') return;
    const units = () => this.db.prepare('SELECT * FROM rollout_units WHERE rollout_id = ? ORDER BY position').all(id) as Row[];

    const active = units().find((u) => u.state === 'offered' || u.state === 'downloaded');
    if (active) {
      if (this.now() - Number(active.updated_at) >= this.unitTimeoutMs) {
        const deviceId = String(active.device_id);
        this.setUnit(id, deviceId, 'failed', `no progress for ${Math.round(this.unitTimeoutMs / 60_000)} min`);
        this.finish(id, 'failed', `rollout stopped: ${deviceId.slice(0, 8)} made no progress`);
      }
      return;
    }

    for (;;) {
      const next = units().find((u) => u.state === 'pending');
      if (!next) {
        this.finish(id, 'done', 'done');
        return;
      }
      const deviceId = String(next.device_id);
      const device = this.db.prepare('SELECT * FROM devices WHERE id = ?').get(deviceId) as Row | undefined;
      if (!device || device.removed_at !== null) {
        this.setUnit(id, deviceId, 'skipped', 'removed from the site');
        continue;
      }
      if (device.last_seen === null || this.now() - Number(device.last_seen) > ONLINE_MS) {
        this.setUnit(id, deviceId, 'skipped', 'offline when its turn came');
        continue;
      }
      if (device.fw_version === rollout.version) {
        this.setUnit(id, deviceId, 'skipped', `already running ${rollout.version}`);
        continue;
      }

      const sha = String(rollout.sha256);
      this.publish(cmdTopic(String(device.site_id), deviceId), JSON.stringify({
        cmd: 'ota', version: rollout.version, size: Number(rollout.size), sha256: sha,
        url: `https://${this.fwHost}:${this.fwPort}/fw/${sha}.bin`,
      }), { qos: 1 });
      this.siteLedger?.append('fw_offered', { unit: deviceId, version: String(rollout.version), image: fromHex(sha), by: String(rollout.by) });
      this.setUnit(id, deviceId, 'offered', `offered ${rollout.version}`);
      return;
    }
  }
}
