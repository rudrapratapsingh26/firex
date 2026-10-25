/**
 * The in-process event bus the WebSocket feeds from. Ingest publishes what
 * changed; the dashboard refetches what it shows.
 */
import { EventEmitter } from 'node:events';

export type LiveEvent =
  | { kind: 'state'; device_id: string; fusion: string; alarm: string; faults: Record<string, boolean> }
  | { kind: 'telemetry'; device_id: string; scores: Record<string, number>; at: number }
  | { kind: 'entry'; device_id: string; seq: number; type: string; verify_result: string }
  | { kind: 'chain'; device_id: string; status: string; detail: string }
  | { kind: 'incident'; device_id: string; incident_id: number; open: boolean }
  | { kind: 'media'; device_id: string; sha256: string }
  | { kind: 'alert'; device_id?: string; incident_id?: number; drill_id?: number; type: string; service: string }
  | { kind: 'gap'; device_id: string; open: boolean }
  | { kind: 'drill'; id: number }
  | { kind: 'device'; device_id: string }
  | { kind: 'elevated'; device_id: string }
  | { kind: 'rollout'; id: number }
  | { kind: 'fw'; device_id: string; phase: string };

export class LiveBus extends EventEmitter {
  publish(event: LiveEvent): void {
    this.emit('event', event);
  }

  subscribe(listener: (event: LiveEvent) => void): () => void {
    this.on('event', listener);
    return () => this.off('event', listener);
  }
}
