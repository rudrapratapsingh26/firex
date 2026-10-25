/**
 * Mock emergency-service endpoints for escalation dry runs (roadmap M4).
 *
 * Stands in for the site desk, the fire station, the police station and the
 * ambulance service: it takes alerts from HttpAlert, answers each one after a
 * configured delay, or never, and writes every delivery and acknowledgement to
 * a JSONL log with millisecond timestamps. The log is what the latency tools
 * read (tools/validation), so the numbers come from the receiving side.
 *
 *   npm run mock:endpoints -- --port 9090 --log endpoints.jsonl \
 *        [--ack fire=20-40 --ack police=silent --ack site=5] [--delay-ms 0-50] [--fail-rate 0]
 *
 * An --ack value is fixed seconds, a uniform range "min-max", or "silent".
 *
 * Log records:
 *   {"event":"delivered", service, recipient, channel, receipt_id, incident_id, node_id, ledger_seq,
 *    ledger_hash, ledger_received_at_ms, retry_of, received_at_ms}
 *   {"event":"acked", receipt_id, service, by, at_ms}
 *   {"event":"rejected", service, recipient, incident_id, received_at_ms, status}   (the --fail-rate answers)
 */
import { appendFileSync } from 'node:fs';
import { createServer, type IncomingMessage, type ServerResponse } from 'node:http';
import type { AddressInfo } from 'node:net';

import type { Service } from './provider.ts';

export type AckBehaviour = { kind: 'fixed'; seconds: number } | { kind: 'range'; min: number; max: number } | { kind: 'silent' };

export type MockRecord =
  | {
    event: 'delivered'; service: string; recipient: string; channel: string; receipt_id: string; incident_id: string;
    node_id: string; ledger_seq: number | null; ledger_hash: string | null; ledger_received_at_ms: number | null;
    retry_of: string | null; received_at_ms: number;
  }
  | { event: 'acked'; receipt_id: string; service: string; by: string; at_ms: number }
  | { event: 'rejected'; service: string; recipient: string; incident_id: string; received_at_ms: number; status: number };

export interface MockEndpointsOptions {
  port?: number;
  host?: string;
  /** Per service; services not named use defaultAck. */
  ack?: Partial<Record<Service, AckBehaviour>>;
  defaultAck?: AckBehaviour;
  /** Simulated delivery delay, uniform in [min, max] ms, before the alert counts as received. */
  delayMs?: [number, number];
  /** Share of alerts answered 503 and not delivered, 0-1. */
  failRate?: number;
  logPath?: string;
  onRecord?: (record: MockRecord) => void;
  random?: () => number;
}

export interface MockEndpoints {
  url: string;
  port: number;
  records: MockRecord[];
  close: () => Promise<void>;
}

export const DEFAULT_ACK: AckBehaviour = { kind: 'range', min: 10, max: 30 };

export function parseAck(spec: string): AckBehaviour {
  const s = spec.trim().toLowerCase();
  if (s === 'silent') return { kind: 'silent' };
  const range = /^(\d+(?:\.\d+)?)-(\d+(?:\.\d+)?)$/.exec(s);
  if (range) {
    const min = Number(range[1]);
    const max = Number(range[2]);
    if (max < min) throw new Error(`ack range ${spec}: max below min`);
    return { kind: 'range', min, max };
  }
  if (/^\d+(?:\.\d+)?$/.test(s)) return { kind: 'fixed', seconds: Number(s) };
  throw new Error(`ack behaviour "${spec}": give seconds, a range like 20-40, or silent`);
}

export function parseRange(spec: string): [number, number] {
  const [a, b] = spec.split('-').map(Number);
  const min = a;
  const max = b === undefined ? a : b;
  if (!Number.isFinite(min) || !Number.isFinite(max) || min < 0 || max < min) throw new Error(`range "${spec}"`);
  return [min, max];
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

function readBody(req: IncomingMessage): Promise<string> {
  return new Promise((resolve, reject) => {
    const chunks: Buffer[] = [];
    let size = 0;
    req.on('data', (chunk: Buffer) => {
      size += chunk.length;
      if (size > 1024 * 1024) {
        reject(new Error('body too large'));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    req.on('error', reject);
  });
}

function reply(res: ServerResponse, status: number, body?: unknown): void {
  if (body === undefined) {
    res.writeHead(status).end();
    return;
  }
  res.writeHead(status, { 'content-type': 'application/json' }).end(JSON.stringify(body));
}

export async function startMockEndpoints(options: MockEndpointsOptions = {}): Promise<MockEndpoints> {
  const random = options.random ?? Math.random;
  const records: MockRecord[] = [];
  const acks = new Map<string, { receipt_id: string; service: string; by: string; at_ms: number }>();
  const timers = new Set<NodeJS.Timeout>();
  let counter = 0;

  const record = (r: MockRecord) => {
    records.push(r);
    if (options.logPath) appendFileSync(options.logPath, JSON.stringify(r) + '\n');
    options.onRecord?.(r);
  };

  const uniform = (min: number, max: number) => min + (max - min) * random();

  const ackDelaySeconds = (service: string): number | null => {
    const behaviour = options.ack?.[service as Service] ?? options.defaultAck ?? DEFAULT_ACK;
    if (behaviour.kind === 'silent') return null;
    return behaviour.kind === 'fixed' ? behaviour.seconds : uniform(behaviour.min, behaviour.max);
  };

  const onAlert = async (req: IncomingMessage, res: ServerResponse, recipient: string) => {
    const body = JSON.parse(await readBody(req)) as {
      alert?: { incident_id?: string; node_id?: string; ledger_seq?: number; ledger_hash?: string };
      contact?: { service?: string; channel?: string };
      ledger_received_at_ms?: number | null;
      retry_of?: string | null;
    };
    const service = String(body.contact?.service ?? 'unknown');
    const incident = String(body.alert?.incident_id ?? '');
    if (options.delayMs) await sleep(uniform(options.delayMs[0], options.delayMs[1]));
    const receivedAt = Date.now();

    if (options.failRate && random() < options.failRate) {
      record({ event: 'rejected', service, recipient, incident_id: incident, received_at_ms: receivedAt, status: 503 });
      reply(res, 503, { error: 'mock endpoint: simulated failure' });
      return;
    }

    counter += 1;
    const receiptId = `m-${String(counter).padStart(6, '0')}`;
    record({
      event: 'delivered',
      service,
      recipient,
      channel: String(body.contact?.channel ?? ''),
      receipt_id: receiptId,
      incident_id: incident,
      node_id: String(body.alert?.node_id ?? ''),
      ledger_seq: typeof body.alert?.ledger_seq === 'number' ? body.alert.ledger_seq : null,
      ledger_hash: typeof body.alert?.ledger_hash === 'string' ? body.alert.ledger_hash : null,
      ledger_received_at_ms: typeof body.ledger_received_at_ms === 'number' ? body.ledger_received_at_ms : null,
      retry_of: body.retry_of ?? null,
      received_at_ms: receivedAt,
    });

    const delay = ackDelaySeconds(service);
    if (delay !== null) {
      const atMs = receivedAt + Math.round(delay * 1000);
      const timer = setTimeout(() => {
        timers.delete(timer);
        const ack = { receipt_id: receiptId, service, by: recipient, at_ms: atMs };
        acks.set(receiptId, ack);
        record({ event: 'acked', ...ack });
      }, Math.max(0, atMs - Date.now()));
      timers.add(timer);
    }
    reply(res, 200, { receipt_id: receiptId, received_at_ms: receivedAt });
  };

  const server = createServer((req, res) => {
    const url = new URL(req.url ?? '/', 'http://mock');
    const parts = url.pathname.split('/').filter(Boolean).map(decodeURIComponent);
    if (req.method === 'POST' && parts.length === 2 && parts[0] === 'alerts') {
      onAlert(req, res, parts[1]).catch((err) => reply(res, 400, { error: String(err instanceof Error ? err.message : err) }));
      return;
    }
    if (req.method === 'GET' && parts.length === 2 && parts[0] === 'acks') {
      const ack = acks.get(parts[1]);
      if (ack) reply(res, 200, ack);
      else reply(res, 204);
      return;
    }
    if (req.method === 'GET' && url.pathname === '/healthz') {
      reply(res, 200, { ok: true, delivered: records.filter((r) => r.event === 'delivered').length });
      return;
    }
    reply(res, 404, { error: 'not found' });
  });

  await new Promise<void>((resolve) => server.listen(options.port ?? 0, options.host ?? '127.0.0.1', resolve));
  const port = (server.address() as AddressInfo).port;
  const host = options.host && options.host !== '0.0.0.0' ? options.host : '127.0.0.1';

  return {
    url: `http://${host}:${port}`,
    port,
    records,
    close: () => new Promise<void>((resolve) => {
      for (const timer of timers) clearTimeout(timer);
      timers.clear();
      server.closeAllConnections();
      server.close(() => resolve());
    }),
  };
}

async function main(): Promise<number> {
  const args = process.argv.slice(2);
  const values = (name: string) => args.flatMap((a, i) => (a === `--${name}` && args[i + 1] !== undefined ? [args[i + 1]] : []));
  const flag = (name: string) => values(name)[0];

  const ack: Partial<Record<Service, AckBehaviour>> = {};
  for (const spec of values('ack')) {
    const [service, behaviour] = spec.split('=');
    if (!['fire', 'police', 'ambulance', 'site'].includes(service) || !behaviour) {
      console.error(`--ack ${spec}: use service=behaviour, for example fire=20-40 or police=silent`);
      return 2;
    }
    ack[service as Service] = parseAck(behaviour);
  }
  const endpoints = await startMockEndpoints({
    port: Number(flag('port') ?? 9090),
    host: flag('host') ?? '127.0.0.1',
    ack,
    delayMs: flag('delay-ms') ? parseRange(flag('delay-ms')!) : undefined,
    failRate: flag('fail-rate') ? Number(flag('fail-rate')) : 0,
    logPath: flag('log'),
    onRecord: (r) => console.log(JSON.stringify(r)),
  });
  console.log(`mock endpoints on ${endpoints.url}${flag('log') ? `, logging to ${flag('log')}` : ''}`);
  console.log('MOCK ONLY: nothing here reaches a real service. Point FIREX_ALERT_HTTP_URL at this address.');
  const stop = async () => {
    await endpoints.close();
    process.exit(0);
  };
  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);
  return new Promise<number>(() => undefined);
}

if (process.argv[1]?.replace(/\\/g, '/').endsWith('gateway/escalation/mock-endpoints.ts')) {
  main().then((code) => process.exit(code), (err) => {
    console.error(String(err instanceof Error ? err.message : err));
    process.exit(1);
  });
}
