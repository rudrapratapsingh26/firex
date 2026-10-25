/**
 * Escalation dry run (roadmap M4): the alert path from a threshold crossing on
 * a unit to an alert delivered at each service, timed end to end, with no
 * hardware and no real service.
 *
 *   npm run escalation:dry-run -- [--incidents 20] [--nodes 4] [--dwell-ms 10000] [--ack-timeout-s 5]
 *        [--max-realerts 1] [--poll-ms 250] [--silent police|none] [--ack fire=1-3 ...] [--delay-ms 0-20]
 *        [--out reports/escalation-dry-run]
 *
 * Everything is real except the edges: simulated units (the same ledger bytes,
 * provisioning and topics as the firmware) provision with pairing codes and
 * connect over mutual TLS to an in-process broker with the site's access
 * rules; the gateway's own ingest verifies each entry and opens the incident;
 * the escalation runner alerts through HttpAlert; mock endpoints receive and
 * answer. One service stays silent (police unless --silent says otherwise) so
 * the re-alert timer is measured too. Services not silent answer after
 * 20-60 % of the ack window unless --ack sets them.
 *
 * Writes <out>.json (kind "alert_latency", read by tools/validation) and <out>.md.
 * Exit status 1 if any alert was not delivered, a re-alert fired before its
 * window, or a service that should have answered did not.
 *
 * The numbers time the gateway's software on one machine. They are not a site
 * network, Wi-Fi from a unit, or a real service's response.
 */
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import type { AddressInfo } from 'node:net';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { createServer } from 'node:tls';
import { fileURLToPath } from 'node:url';

import { Aedes } from 'aedes';

import { loadConfig } from '../server/config.ts';
import { ensureSite, openDb, type Row } from '../server/db.ts';
import { EscalationRunner } from '../server/escalate.ts';
import { buildProvisionApp } from '../server/http.ts';
import { Ingest } from '../server/ingest.ts';
import { LiveBus } from '../server/live.ts';
import { startBridge } from '../server/mqtt-bridge.ts';
import { ensureServiceCertificates, loadOrCreateCa, readServiceCredentials } from '../server/pki.ts';
import { createPairing, resetProvisionRateLimit } from '../server/provision.ts';
import { certificateCommonName, mayPublish, maySubscribe } from '../sim/acl.ts';
import { SimNode } from '../sim/node-sim.ts';
import { HttpAlert } from './http-alert.ts';
import { parseAck, parseRange, startMockEndpoints, type AckBehaviour, type MockRecord } from './mock-endpoints.ts';
import type { Service } from './provider.ts';
import { parseRegistry, SERVICES } from './registry.ts';

const SITE = 'site-dry-run';

export interface Summary {
  n: number;
  min: number | null;
  median: number | null;
  p90: number | null;
  p99: number | null;
  max: number | null;
  mean: number | null;
}

/** Linear interpolation between closest ranks, as tools/validation/fxstats.py computes it. */
export function percentile(values: number[], p: number): number | null {
  if (!values.length) return null;
  const ordered = [...values].sort((a, b) => a - b);
  const rank = ((ordered.length - 1) * p) / 100;
  const low = Math.floor(rank);
  const high = Math.ceil(rank);
  return low === high ? ordered[low] : ordered[low] + (ordered[high] - ordered[low]) * (rank - low);
}

export function summarize(values: number[]): Summary {
  if (!values.length) return { n: 0, min: null, median: null, p90: null, p99: null, max: null, mean: null };
  return {
    n: values.length,
    min: Math.min(...values),
    median: percentile(values, 50),
    p90: percentile(values, 90),
    p99: percentile(values, 99),
    max: Math.max(...values),
    mean: values.reduce((a, b) => a + b, 0) / values.length,
  };
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

async function until(what: string, predicate: () => boolean, timeoutMs = 15_000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (!predicate()) {
    if (Date.now() > deadline) throw new Error(`timed out waiting for ${what}`);
    await sleep(10);
  }
}

interface Options {
  incidents: number;
  nodes: number;
  dwellMs: number;
  ackTimeoutS: number;
  maxReAlerts: number;
  pollMs: number;
  silent: Service[];
  ack: Partial<Record<Service, AckBehaviour>>;
  delayMs: [number, number] | undefined;
  out: string;
}

function parseOptions(args: string[]): Options {
  const values = (name: string) => args.flatMap((a, i) => (a === `--${name}` && args[i + 1] !== undefined ? [args[i + 1]] : []));
  const flag = (name: string) => values(name)[0];
  const int = (name: string, fallback: number, min: number) => {
    const v = flag(name) === undefined ? fallback : Number(flag(name));
    if (!Number.isFinite(v) || v < min) throw new Error(`--${name} must be a number of at least ${min}`);
    return v;
  };
  const silentArg = values('silent');
  const silent = (silentArg.length ? silentArg.flatMap((s) => s.split(',')) : ['police'])
    .filter((s) => s !== 'none') as Service[];
  for (const s of silent) if (!SERVICES.includes(s)) throw new Error(`--silent ${s}: not a service`);
  const ack: Partial<Record<Service, AckBehaviour>> = {};
  for (const spec of values('ack')) {
    const [service, behaviour] = spec.split('=');
    if (!SERVICES.includes(service as Service) || !behaviour) throw new Error(`--ack ${spec}: use service=behaviour`);
    ack[service as Service] = parseAck(behaviour);
  }
  const incidents = Math.trunc(int('incidents', 20, 1));
  const ackTimeoutS = int('ack-timeout-s', 5, 0.5);
  for (const s of SERVICES) {
    if (silent.includes(s)) ack[s] = { kind: 'silent' };
    else ack[s] ??= { kind: 'range', min: 0.2 * ackTimeoutS, max: 0.6 * ackTimeoutS };
  }
  return {
    incidents,
    nodes: Math.trunc(Math.min(int('nodes', 4, 1), incidents)),
    dwellMs: int('dwell-ms', 10_000, 0),
    ackTimeoutS,
    maxReAlerts: Math.trunc(int('max-realerts', 1, 0)),
    pollMs: int('poll-ms', 250, 10),
    silent,
    ack,
    delayMs: flag('delay-ms') ? parseRange(flag('delay-ms')!) : undefined,
    out: flag('out') ?? 'reports/escalation-dry-run',
  };
}

interface Sample {
  incident: number;
  incident_id: number | null;
  node_id: string;
  ledger_seq: number;
  crossing_at_ms: number;
  critical_at_ms: number;
  gateway_received_at_ms: number | null;
  delivered: Partial<Record<Service, { received_at_ms: number; gateway_to_delivered_ms: number | null; crossing_to_delivered_ms: number }>>;
  acknowledged: Partial<Record<Service, number>>;
}

async function main(): Promise<number> {
  const opt = parseOptions(process.argv.slice(2));
  const started = Date.now();
  resetProvisionRateLimit();
  const dataDir = mkdtempSync(join(tmpdir(), 'firex-dry-run-'));
  const config = loadConfig({ FIREX_DATA_DIR: dataDir, FIREX_SITE_ID: SITE, FIREX_MQTT_ADVERTISED_HOST: 'localhost', FIREX_BROKER_NAMES: 'localhost,127.0.0.1' });
  const db = openDb(':memory:');
  ensureSite(db, SITE, 'Dry-run site');
  const pki = await loadOrCreateCa(config.pkiDir, 'Dry run');
  await ensureServiceCertificates(pki, config.brokerNames);

  const broker = await Aedes.createBroker({
    authorizePublish: (client, packet, callback) => {
      callback(client && !mayPublish(certificateCommonName(client.conn), packet.topic) ? new Error('not your topic') : null);
    },
    authorizeSubscribe: (client, sub, callback) => {
      const ok = maySubscribe(certificateCommonName(client.conn), sub.topic);
      callback(ok ? null : new Error('not your topic'), ok ? sub : null);
    },
  });
  const tlsServer = createServer({
    key: readFileSync(join(pki.dir, 'broker.key')),
    cert: readFileSync(join(pki.dir, 'broker.crt')),
    ca: pki.caCertPem,
    requestCert: true,
    rejectUnauthorized: true,
  }, broker.handle);
  await new Promise<void>((resolve) => tlsServer.listen(0, '127.0.0.1', resolve));
  const mqttPort = (tlsServer.address() as AddressInfo).port;

  const endpoints = await startMockEndpoints({ ack: opt.ack, delayMs: opt.delayMs });
  const example = JSON.parse(readFileSync(fileURLToPath(new URL('./registry.example.json', import.meta.url)), 'utf8'));
  /* Every service live: a dry run exercises the whole fan-out, and its "agreement" is with the mock endpoints. */
  const registry = parseRegistry({
    ...example, site_id: SITE, site_name: 'Dry-run site',
    contacts: example.contacts.map((c: Record<string, unknown>) => ({ ...c, mode: 'live', agreement: { reference: 'dry run against mock endpoints', date: '2026-09-15' } })),
  });

  const live = new LiveBus();
  let bridge: ReturnType<typeof startBridge> | null = null;
  const ingest = new Ingest({ db, mediaDir: config.mediaDir, live, publish: (t, p, o) => bridge?.publish(t, p, o) });
  const runner = new EscalationRunner({
    db, live, registry, provider: new HttpAlert({ baseUrl: endpoints.url }),
    ackTimeoutS: opt.ackTimeoutS, maxReAlerts: opt.maxReAlerts, pollMs: opt.pollMs,
  });
  runner.start();
  bridge = startBridge({ url: `mqtts://localhost:${mqttPort}`, ...readServiceCredentials(pki), onMessage: (t, p) => ingest.handle(t, p) });
  await until('the gateway to reach the broker', () => bridge!.connected());

  const provisionApp = await buildProvisionApp({ db, pki, live, advertisedMqttHost: 'localhost', advertisedMqttPort: mqttPort });
  await provisionApp.listen({ host: '127.0.0.1', port: 0 });
  const provisionUrl = `http://127.0.0.1:${(provisionApp.server.address() as AddressInfo).port}`;

  const nodes: SimNode[] = [];
  const samples: Sample[] = [];
  const failures: string[] = [];
  const stored = (node: SimNode, seq: number) => db.prepare('SELECT received_at FROM ledger_entries WHERE device_id = ? AND seq = ?').get(node.deviceId, seq) as Row | undefined;

  try {
    for (let i = 0; i < opt.nodes; i++) {
      const pairing = createPairing(db, { siteId: SITE, label: `Dry-run unit ${i + 1}`, by: 'dry-run', ttlHours: 1 });
      const node = new SimNode();
      await node.provision(provisionUrl, pairing.code);
      await node.connect();
      node.runNormal();
      nodes.push(node);
    }
    await until('every unit to mirror its boot entries', () => nodes.every((n) => n.ackedSeq === n.lastSeq));
    console.log(`dry run: ${opt.nodes} simulated units online; ${opt.incidents} incidents, dwell ${opt.dwellMs} ms, ack window ${opt.ackTimeoutS} s, silent: ${opt.silent.join(', ') || 'none'}`);

    await Promise.all(nodes.map(async (node, index) => {
      for (let k = index; k < opt.incidents; k += opt.nodes) {
        const crossingAt = Date.now();
        node.crossThreshold('gas', 66);
        await sleep(opt.dwellMs);
        const criticalAt = Date.now();
        const entry = node.raiseCritical(['gas', 'heat'], { gas: 81, heat: 64, smoke: 30, flame: 0, humidity: 40 });
        await until(`incident ${k} at the gateway`, () => Boolean(stored(node, entry.seq)));
        samples.push({
          incident: k, incident_id: null, node_id: node.deviceId, ledger_seq: entry.seq, crossing_at_ms: crossingAt,
          critical_at_ms: criticalAt, gateway_received_at_ms: null, delivered: {}, acknowledged: {},
        });
        node.settle();
        await until(`unit ${index + 1} to mirror its return to NORMAL`, () => node.ackedSeq === node.lastSeq);
      }
    }));

    const worstCase = opt.ackTimeoutS * (opt.maxReAlerts + 1) * 1000 + opt.pollMs * 4 + 10_000;
    await Promise.race([runner.idle(), sleep(worstCase)]);
    if (runner.running) failures.push(`${runner.running} escalation(s) still running ${Math.round(worstCase / 1000)} s after the last incident`);
  } finally {
    runner.stop();
    await Promise.allSettled(nodes.map((n) => n.disconnect()));
    await bridge?.close();
    await provisionApp.close();
    await new Promise<void>((resolve) => broker.close(() => resolve()));
    tlsServer.close();
    await endpoints.close();
  }

  /* --- measure ------------------------------------------------------------ */
  samples.sort((a, b) => a.incident - b.incident);
  const records = endpoints.records;
  const delivered = records.filter((r): r is Extract<MockRecord, { event: 'delivered' }> => r.event === 'delivered');
  const acked = new Map(records.filter((r): r is Extract<MockRecord, { event: 'acked' }> => r.event === 'acked').map((r) => [r.receipt_id, r]));
  const events = (db.prepare('SELECT * FROM alert_events ORDER BY id').all() as Row[])
    .map((r) => ({ ...r, payload: JSON.parse(String(r.payload)) }) as Row & { payload: Record<string, unknown> });

  const segments = { crossing_to_critical_ms: [] as number[], critical_to_gateway_ms: [] as number[], gateway_to_delivered_ms: [] as number[], crossing_to_delivered_ms: [] as number[] };
  const perService = Object.fromEntries(SERVICES.map((s) => [s, { delivered: 0, crossing_to_delivered_ms: [] as number[], ack_latency_s: [] as number[] }]));

  for (const sample of samples) {
    const incident = db.prepare('SELECT id FROM incidents WHERE device_id = ? AND start_seq = ?').get(sample.node_id, sample.ledger_seq) as Row | undefined;
    sample.incident_id = incident ? Number(incident.id) : null;
    const received = db.prepare('SELECT received_at FROM ledger_entries WHERE device_id = ? AND seq = ?').get(sample.node_id, sample.ledger_seq) as Row | undefined;
    sample.gateway_received_at_ms = received ? Number(received.received_at) : null;
    segments.crossing_to_critical_ms.push(sample.critical_at_ms - sample.crossing_at_ms);
    if (sample.gateway_received_at_ms !== null) segments.critical_to_gateway_ms.push(sample.gateway_received_at_ms - sample.critical_at_ms);
    if (sample.incident_id === null) failures.push(`incident ${sample.incident}: the gateway opened no incident`);

    const incidentEvents = events.filter((e) => e.incident_id === sample.incident_id);
    for (const service of SERVICES) {
      const primary = delivered.find((d) => d.node_id === sample.node_id && d.ledger_seq === sample.ledger_seq && d.service === service && d.retry_of === null);
      if (!primary) {
        failures.push(`incident ${sample.incident}: ${service} was never delivered`);
        continue;
      }
      const gatewayToDelivered = primary.ledger_received_at_ms === null ? null : primary.received_at_ms - primary.ledger_received_at_ms;
      const crossingToDelivered = primary.received_at_ms - sample.crossing_at_ms;
      sample.delivered[service] = { received_at_ms: primary.received_at_ms, gateway_to_delivered_ms: gatewayToDelivered, crossing_to_delivered_ms: crossingToDelivered };
      if (gatewayToDelivered !== null) segments.gateway_to_delivered_ms.push(gatewayToDelivered);
      segments.crossing_to_delivered_ms.push(crossingToDelivered);
      perService[service].delivered += 1;
      perService[service].crossing_to_delivered_ms.push(crossingToDelivered);

      const ack = incidentEvents.find((e) => e.type === 'alert_ack' && e.service === service);
      if (ack) {
        const answered = acked.get(String(ack.receipt_id));
        const receipt = delivered.find((d) => d.receipt_id === String(ack.receipt_id));
        if (answered && receipt) {
          const latency = (answered.at_ms - receipt.received_at_ms) / 1000;
          sample.acknowledged[service] = latency;
          perService[service].ack_latency_s.push(latency);
        }
      } else if (!opt.silent.includes(service)) {
        failures.push(`incident ${sample.incident}: ${service} should have answered and no acknowledgement was recorded`);
      }
    }
  }

  const realertObserved: number[] = [];
  let early = 0;
  for (const retry of events.filter((e) => e.type === 'alert_sent' && typeof e.payload.retry_of === 'string')) {
    const original = events.find((e) => e.type === 'alert_sent' && e.receipt_id === retry.payload.retry_of);
    if (!original) continue;
    const observed = (Number(retry.at_ms) - Number(original.at_ms)) / 1000;
    realertObserved.push(observed);
    if (observed < opt.ackTimeoutS) {
      early += 1;
      failures.push(`incident ${retry.incident_id}: ${retry.service} re-alerted after ${observed.toFixed(3)} s, inside the ${opt.ackTimeoutS} s window`);
    }
  }
  const expectedRealerts = opt.maxReAlerts > 0 ? samples.length * opt.silent.length : 0;
  if (realertObserved.length < expectedRealerts) {
    failures.push(`${expectedRealerts - realertObserved.length} expected re-alert(s) to silent services never went out`);
  }
  const failedSends = events.filter((e) => e.type === 'alert_failed').length;

  const report = {
    kind: 'alert_latency',
    source: 'dry_run',
    /* The gateway's ingest, escalation and HTTP provider are the real code, timed for real;
     * the units and the services are stand-ins, which `source` says. */
    synthetic: false,
    generated_at: new Date().toISOString(),
    config: {
      incidents: opt.incidents, nodes: opt.nodes, dwell_ms: opt.dwellMs, ack_timeout_s: opt.ackTimeoutS,
      max_realerts: opt.maxReAlerts, poll_ms: opt.pollMs, silent: opt.silent, ack: opt.ack, delay_ms: opt.delayMs ?? null,
    },
    incidents: samples.length,
    segments: Object.fromEntries(Object.entries(segments).map(([k, v]) => [k, summarize(v)])),
    per_service: Object.fromEntries(SERVICES.map((s) => [s, {
      delivered: perService[s].delivered,
      crossing_to_delivered_ms: summarize(perService[s].crossing_to_delivered_ms),
      ack_latency_s: summarize(perService[s].ack_latency_s),
    }])),
    realert: { configured_s: opt.ackTimeoutS, observed_s: summarize(realertObserved), early },
    failed_sends: failedSends,
    failures,
    duration_s: (Date.now() - started) / 1000,
    samples,
  };

  mkdirSync(dirname(opt.out) || '.', { recursive: true });
  writeFileSync(`${opt.out}.json`, JSON.stringify(report, null, 2) + '\n');
  writeFileSync(`${opt.out}.md`, markdown(report));
  rmSync(dataDir, { recursive: true, force: true });
  db.close();

  const seg = report.segments as Record<string, Summary>;
  const ms = (v: number | null) => (v === null ? '-' : v.toFixed(1));
  console.log(`dry run: ${samples.length} incidents, ${delivered.length} deliveries in ${report.duration_s.toFixed(1)} s`);
  for (const [name, s] of Object.entries(seg)) console.log(`  ${name.padEnd(26)} median ${ms(s.median)} ms  p90 ${ms(s.p90)}  max ${ms(s.max)}  (n ${s.n})`);
  const r = report.realert.observed_s;
  console.log(`  re-alert after              median ${ms(r.median)} s  min ${ms(r.min)}  max ${ms(r.max)}  (configured ${opt.ackTimeoutS} s, early ${early})`);
  console.log(`  wrote ${opt.out}.json and ${opt.out}.md`);
  if (failures.length) {
    console.log(`FAIL: ${failures.length} problem(s)`);
    for (const f of failures.slice(0, 20)) console.log(`  - ${f}`);
    return 1;
  }
  console.log('PASS: every alert delivered, every answering service acknowledged, no re-alert inside its window');
  return 0;
}

function markdown(report: {
  generated_at: string; config: Record<string, unknown>; incidents: number; segments: Record<string, Summary>;
  per_service: Record<string, { delivered: number; crossing_to_delivered_ms: Summary; ack_latency_s: Summary }>;
  realert: { configured_s: number; observed_s: Summary; early: number }; failed_sends: number; failures: string[];
}): string {
  const f = (v: number | null, digits = 1) => (v === null ? '-' : v.toFixed(digits));
  const c = report.config;
  const lines = [
    '# Escalation dry run',
    '',
    '**SYNTHETIC.** Simulated units, an in-process broker and mock endpoints on one machine. These numbers time the',
    "gateway's software path; they are not a site network, a unit's Wi-Fi, or a real service's response.",
    '',
    `Generated ${report.generated_at}. ${report.incidents} incidents on ${c.nodes} simulated units; threshold-to-CRITICAL dwell ${c.dwell_ms} ms;`,
    `acknowledgement window ${c.ack_timeout_s} s, ${c.max_realerts} re-alert(s), polled every ${c.poll_ms} ms; silent: ${(c.silent as string[]).join(', ') || 'none'}.`,
    '',
    '| Segment | n | median ms | p90 ms | p99 ms | max ms |',
    '|---|---|---|---|---|---|',
    ...Object.entries(report.segments).map(([name, s]) => `| ${name} | ${s.n} | ${f(s.median)} | ${f(s.p90)} | ${f(s.p99)} | ${f(s.max)} |`),
    '',
    '| Service | delivered | crossing to delivered, median ms | p90 ms | acknowledged | ack latency, median s |',
    '|---|---|---|---|---|---|',
    ...Object.entries(report.per_service).map(([name, s]) =>
      `| ${name} | ${s.delivered} | ${f(s.crossing_to_delivered_ms.median)} | ${f(s.crossing_to_delivered_ms.p90)} | ${s.ack_latency_s.n} | ${f(s.ack_latency_s.median, 2)} |`),
    '',
    `Re-alert timer: configured ${report.realert.configured_s} s; observed median ${f(report.realert.observed_s.median, 3)} s, ` +
      `min ${f(report.realert.observed_s.min, 3)} s, max ${f(report.realert.observed_s.max, 3)} s (n ${report.realert.observed_s.n}); early: ${report.realert.early}.`,
    '',
    `Failed sends: ${report.failed_sends}.`,
    '',
    report.failures.length ? `## Failures\n\n${report.failures.map((x) => `- ${x}`).join('\n')}` : 'Result: **PASS**. Every alert was delivered, every answering service acknowledged, and no re-alert fired inside its window.',
    '',
  ];
  return lines.join('\n');
}

if (process.argv[1]?.replace(/\\/g, '/').endsWith('gateway/escalation/dry-run.ts')) {
  main().then((code) => process.exit(code), (err) => {
    console.error(String(err instanceof Error ? err.stack ?? err.message : err));
    process.exit(1);
  });
}
