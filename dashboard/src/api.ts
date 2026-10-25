import { createContext, useCallback, useContext, useEffect, useRef, useState } from 'react';

export type Role = 'viewer' | 'operator' | 'admin';

export interface Me {
  username: string;
  role: Role;
}

export interface SelfTest {
  ts: number;
  result: string;
  checks: Record<string, boolean>;
}

export interface Device {
  id: string;
  site_id: string;
  label: string;
  floor_x: number | null;
  floor_y: number | null;
  fw_version: string | null;
  boot_count: number | null;
  provisioned_at: number;
  last_seen: number | null;
  online: boolean;
  state: string | null;
  alarm: string | null;
  faults: Record<string, boolean>;
  epoch: number;
  chain_status: string;
  last_seq: number | null;
  last_ts: number | null;
  last_self_test: SelfTest | null;
  last_calibration_ts: number | null;
  open_incident: number | null;
  /* M5. Optional so the dashboard still works against an M4 gateway. */
  clock?: 'gateway' | 'uncertain' | null;
  offline_since?: number | null;
  removed?: boolean;
  ota?: { partition: string; pending_verify: boolean } | null;
}

export interface ChainEvent {
  id: number;
  epoch: number;
  at: number;
  kind: string;
  from_seq: number | null;
  to_seq: number | null;
  detail: string;
}

export interface Gap {
  start_ms: number;
  end_ms: number | null;
  seconds: number;
}

export interface FwUpdate {
  seq: number;
  ts: number;
  phase: 'accepted' | 'downloaded' | 'confirmed' | 'failed' | 'rolled_back';
  version: string;
  from_version: string;
  reason: string;
}

export interface DeviceTrace {
  sha256: string;
  seq: number;
  samples: number;
  period_ms: number;
  trigger: string;
  received_at: number;
}

export interface DeviceDetail extends Device {
  chain_events: ChainEvent[];
  verify_counts: Record<string, number>;
  gaps?: Gap[];
  fw_updates?: FwUpdate[];
  traces?: DeviceTrace[];
}

export interface Entry {
  device_id: string;
  label: string;
  epoch: number;
  seq: number;
  ts: number;
  ts_src: string;
  type: string;
  payload: Record<string, unknown>;
  hash: string;
  prev_hash: string;
  received_at: number;
  verify_result: string;
}

/** An entry of the site chain, signed by the gateway (EntryJson in ledger/src/entry.ts). */
export interface SiteEntry {
  seq: number;
  ts: number;
  ts_src: string;
  device_id: string;
  type: string;
  payload: Record<string, unknown>;
  fw_hash: string;
  prev_hash: string;
  hash: string;
  sig: string;
}

export interface Frame {
  sha256: string;
  seq: number;
  available: boolean;
  deleted: boolean;
}

export interface AlertEvent {
  id: number;
  at_ms: number;
  type: 'alert_sent' | 'alert_ack' | 'alert_timeout' | 'alert_failed';
  service: string;
  receipt_id: string | null;
  payload: Record<string, unknown>;
}

export interface Escalation {
  state: 'none' | 'running' | 'finished' | 'withheld';
  reason?: 'verification' | 'late' | 'drill_mode';
}

export interface Incident {
  id: number;
  device_id: string;
  device_label: string;
  epoch: number;
  start_seq: number;
  start_ts: number;
  end_seq: number | null;
  end_ts: number | null;
  reason: string[];
  label: string | null;
  labelled_by: string | null;
  labelled_at: number | null;
  note: string | null;
  frames: Frame[];
  alerts?: AlertEvent[];
  late?: boolean;
  escalation?: Escalation;
  traces?: { sha256: string; samples: number; period_ms: number }[];
}

export interface Drill {
  id: number;
  started_at_ms: number;
  finished_at_ms: number | null;
  by: string;
  services: string[];
  alerts: AlertEvent[];
}

export interface RegistryContact {
  service: string;
  name: string;
  channel: string;
  recipient_masked: string;
  mode: 'drill' | 'live';
  agreement: { reference: string; date: string } | null;
  backup?: { name: string; channel: string; recipient_masked: string };
}

export interface Registry {
  site_id: string;
  site_name: string;
  version: number;
  ack_timeout_s: number;
  max_realerts: number;
  offline_notify_min: number;
  sha256: string;
  contacts: RegistryContact[];
}

export type Signature = 'verified' | 'present' | 'missing' | 'wrong_key';

export interface FirmwareImage {
  sha256: string;
  version: string;
  project: string;
  size: number;
  uploaded_at: number;
  uploaded_by: string;
  signature: Signature;
}

export interface Rollout {
  id: number;
  sha256: string;
  version: string;
  state: 'running' | 'done' | 'failed' | 'stopped';
  started_at: number;
  by: string;
  units: { id: string; label: string; state: string; detail: string }[];
}

export interface Pairing {
  code: string;
  label: string;
  created_by: string;
  created_at: number;
  expires_at: number;
  used_at: number | null;
  device_id: string | null;
}

export interface Site {
  id: string;
  name: string;
  address: string;
  has_floor_plan: boolean;
  mqtt_connected: boolean;
}

export interface TelemetryPoint {
  at: number;
  scores: Record<string, number>;
  fan: { state?: string; duty?: number; rpm?: number } | null;
}

export class ApiError extends Error {
  readonly status: number;

  constructor(status: number, message: string) {
    super(message);
    this.status = status;
  }
}

export async function api<T>(path: string, options: { method?: string; body?: unknown; raw?: Blob; contentType?: string } = {}): Promise<T> {
  const headers: Record<string, string> = {};
  let body: BodyInit | undefined;
  if (options.raw) {
    body = options.raw;
    headers['content-type'] = options.contentType ?? options.raw.type;
  } else if (options.body !== undefined) {
    body = JSON.stringify(options.body);
    headers['content-type'] = 'application/json';
  }
  const res = await fetch(path, { method: options.method ?? (body ? 'POST' : 'GET'), headers, body, credentials: 'same-origin' });
  const data = res.headers.get('content-type')?.includes('application/json') ? await res.json() : undefined;
  if (res.status === 401) window.dispatchEvent(new Event('firex:signed-out'));
  if (!res.ok) throw new ApiError(res.status, (data as { error?: string } | undefined)?.error ?? `The gateway answered ${res.status}.`);
  return data as T;
}

export type LiveEvent = { kind: string; device_id?: string; [key: string]: unknown };

export interface LiveState {
  tick: number;
  connected: boolean;
  lastEvent: LiveEvent | null;
  /** Every event in the latest burst, oldest first. A new array per burst. */
  events: LiveEvent[];
}

export const LiveContext = createContext<LiveState>({ tick: 0, connected: false, lastEvent: null, events: [] });

/** One WebSocket for the whole app. Bursts of events become one refresh every 1.5 s at most. */
export function useLiveConnection(enabled: boolean): LiveState {
  const [state, setState] = useState<LiveState>({ tick: 0, connected: false, lastEvent: null, events: [] });
  const pending = useRef<LiveEvent[]>([]);

  useEffect(() => {
    if (!enabled) return;
    let socket: WebSocket | null = null;
    let retry = 1000;
    let stopped = false;
    let flushTimer: number | undefined;
    let retryTimer: number | undefined;

    const flush = () => {
      flushTimer = undefined;
      const batch = pending.current;
      pending.current = [];
      if (batch.length) setState((s) => ({ ...s, tick: s.tick + 1, lastEvent: batch[batch.length - 1], events: batch }));
    };
    const open = () => {
      socket = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/api/live`);
      socket.onopen = () => {
        retry = 1000;
        setState((s) => ({ ...s, connected: true, tick: s.tick + 1 }));
      };
      socket.onmessage = (message) => {
        pending.current.push(JSON.parse(String(message.data)) as LiveEvent);
        if (pending.current.length > 200) pending.current.shift();
        if (flushTimer === undefined) flushTimer = window.setTimeout(flush, 1500);
      };
      socket.onclose = () => {
        setState((s) => ({ ...s, connected: false }));
        if (!stopped) retryTimer = window.setTimeout(open, (retry = Math.min(retry * 2, 10_000)));
      };
    };
    open();
    return () => {
      stopped = true;
      window.clearTimeout(flushTimer);
      window.clearTimeout(retryTimer);
      socket?.close();
    };
  }, [enabled]);

  return state;
}

/**
 * GET a path, and again whenever live events arrive (unless live is false).
 * `status` is the HTTP status of the last failure, so a view can tell a feature
 * this gateway does not have (404) from a real error.
 */
export function useApi<T>(path: string | null, live = true): { data: T | undefined; error: string | null; status: number | null; reload: () => void } {
  const { tick } = useContext(LiveContext);
  const [data, setData] = useState<T>();
  const [error, setError] = useState<string | null>(null);
  const [status, setStatus] = useState<number | null>(null);
  const [manual, setManual] = useState(0);
  const reload = useCallback(() => setManual((n) => n + 1), []);

  useEffect(() => {
    if (!path) return;
    let cancelled = false;
    api<T>(path)
      .then((result) => {
        if (!cancelled) {
          setData(result);
          setError(null);
          setStatus(null);
        }
      })
      .catch((err: Error) => {
        if (!cancelled) {
          setError(err.message);
          setStatus(err instanceof ApiError ? err.status : null);
        }
      });
    return () => {
      cancelled = true;
    };
  }, [path, manual, live ? tick : 0]);

  return { data, error, status, reload };
}
