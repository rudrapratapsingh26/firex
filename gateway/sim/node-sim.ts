/**
 * A FireX node in software. Same identity scheme, same ledger bytes, same
 * provisioning exchange and MQTT topics as the firmware (docs/transport.md), so
 * the gateway and dashboard can be exercised end to end on a laptop, and the
 * M3 demonstration rehearsed before the hardware is on the bench.
 *
 *   npm run sim:node -- --code 7K3QD-9M2XA --provision http://localhost:8081
 *   npm run sim:node -- --state sim-node.json --scenario incident
 *   npm run sim:node -- --state sim-node.json --scenario trace --stay --ota confirm --ota-download
 *
 * From M5 it also answers the gateway's `time` command (its entries then carry
 * ts_src gateway, after a clock_sync entry), records episode traces, and acts on
 * an `ota` command the way the firmware reports one: fw_update accepted,
 * downloaded, and confirmed after a simulated reboot, or failed and rolled back.
 *
 * It is a stand-in for testing the gateway, not a model of the sensors.
 */
import { createPrivateKey, createPublicKey, randomBytes } from 'node:crypto';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { request } from 'node:https';

import mqtt, { type MqttClient } from 'mqtt';

import { fromHex, toHex } from '../../ledger/src/cbor.ts';
import { deviceIdFor, entryToJson, LedgerWriter, sha256, type EntryJson, type EventType } from '../../ledger/src/entry.ts';
import { normalisePairingCode, replyMac, type ProvisionReply } from '../server/pairing.ts';
import { encodeTrace, type TraceSample } from '../server/traces.ts';
import { flatGreyJpeg } from './jpeg.ts';
import { buildProvisionRequest, newP256, type KeyPair } from './keys.ts';

const WINDOW = 8;
const CLOCK_STEP_MS = 2000;

export interface SimCredentials {
  siteId: string;
  mqttHost: string;
  mqttPort: number;
  caPem: string;
  certPem: string;
}

export interface SimNodeOptions {
  fwVersion?: string;
  /** Connect here instead of the host the gateway advertised (tests, port forwards). */
  mqttUrl?: string;
  /** Certificate name to expect when mqttUrl uses an address the broker certificate does not name. */
  servername?: string;
  clock?: () => number;
  log?: (line: string) => void;
  /** What an `ota` command does: install and confirm (the default), fail to confirm and roll back, or nothing. */
  ota?: 'confirm' | 'fail' | 'ignore';
  /** Fetch the image from the gateway's firmware server, and check its hash, before reporting it downloaded. */
  otaDownload?: boolean;
}

interface Command {
  cmd: string;
  have_seq?: number;
  seq?: number;
  from_seq?: number;
  sha256?: string;
  unix_ms?: number;
  version?: string;
  size?: number;
  url?: string;
}

/** Ten quiet samples, six ELEVATED on gas, fourteen quiet again: an episode that never became an incident. */
export function quietEpisode(): TraceSample[] {
  const at = (fusion: string, gas: number): TraceSample => ({ scores: [gas, 0.05, 0, 0.1, 0.45], fusion, alarm: fusion, fanDuty: 100, fanState: 'on' });
  return [
    ...Array.from({ length: 10 }, () => at('NORMAL', 0.1)),
    ...Array.from({ length: 6 }, () => at('ELEVATED', 0.66)),
    ...Array.from({ length: 14 }, () => at('NORMAL', 0.2)),
  ];
}

export class SimNode {
  readonly ledgerKey: KeyPair;
  readonly tlsKey: KeyPair;
  readonly deviceId: string;
  credentials: SimCredentials | null = null;
  fusion = 'WARMUP';
  alarm = 'WARMUP';
  fwVersion: string;
  clock: 'uncertain' | 'gateway' = 'uncertain';
  partition: 'ota_0' | 'ota_1' = 'ota_0';
  /** The update an `ota` command started, for tests to await. */
  otaTask: Promise<void> | null = null;

  private readonly options: SimNodeOptions;
  private readonly writer: LedgerWriter;
  /** The node's flash: entries still on the node, oldest first. */
  private readonly flash: EntryJson[] = [];
  private readonly media = new Map<string, Buffer>();
  private readonly traces = new Map<string, Buffer>();
  private client: MqttClient | null = null;
  private nextToSend = 0;
  private acked = -1;
  private bootCount = 1;
  private clockOffsetMs = 0;
  private readonly startedAt = Date.now();

  constructor(options: SimNodeOptions = {}, keys?: { ledger: KeyPair; tls: KeyPair }) {
    this.options = options;
    this.fwVersion = options.fwVersion ?? '0.4.0-sim';
    this.ledgerKey = keys?.ledger ?? newP256();
    this.tlsKey = keys?.tls ?? newP256();
    this.deviceId = toHex(deviceIdFor(this.ledgerKey.publicKey));
    this.writer = new LedgerWriter(deviceIdFor(this.ledgerKey.publicKey), sha256(Buffer.from(`firex-sim-${this.fwVersion}`)), this.ledgerKey.privateKey);
  }

  private log(line: string): void {
    this.options.log?.(`[sim ${this.deviceId.slice(0, 8)}] ${line}`);
  }

  get lastSeq(): number {
    return this.flash.length ? this.flash[this.flash.length - 1].seq : -1;
  }

  get oldestSeq(): number {
    return this.flash.length ? this.flash[0].seq : this.nextSeqValue();
  }

  get ackedSeq(): number {
    return this.acked;
  }

  get pendingMedia(): number {
    return this.media.size;
  }

  get pendingTraces(): number {
    return this.traces.size;
  }

  get connected(): boolean {
    return Boolean(this.client?.connected);
  }

  /** POST /api/provision with the code from the label; trust the reply only if its MAC verifies. */
  async provision(provisionUrl: string, label: string): Promise<SimCredentials> {
    const code = normalisePairingCode(label);
    if (!code) throw new Error(`"${label}" is not a pairing code`);
    const request = buildProvisionRequest(code, this.ledgerKey, this.tlsKey, this.fwVersion);
    const response = await fetch(`${provisionUrl.replace(/\/$/, '')}/api/provision`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify(request),
    });
    const body = await response.json() as ProvisionReply & { error?: string };
    if (!response.ok) throw new Error(`provisioning refused (${response.status}): ${body.error}`);
    const { mac, ...reply } = body;
    if (mac !== replyMac(code, request.nonce, reply)) throw new Error('provisioning reply MAC does not verify: refusing its CA');
    this.credentials = { siteId: reply.site_id, mqttHost: reply.mqtt_host, mqttPort: reply.mqtt_port, caPem: reply.ca_pem, certPem: reply.cert_pem };
    this.log(`provisioned into ${reply.site_id}`);
    return this.credentials;
  }

  private topic(kind: string): string {
    return `firex/${this.credentials!.siteId}/${this.deviceId}/${kind}`;
  }

  private tlsKeyPem(): string {
    return this.tlsKey.privateKey.export({ type: 'pkcs8', format: 'pem' }).toString();
  }

  async connect(): Promise<void> {
    if (!this.credentials) throw new Error('provision first');
    const c = this.credentials;
    const url = this.options.mqttUrl ?? `mqtts://${c.mqttHost}:${c.mqttPort}`;
    const client = await mqtt.connectAsync(url, {
      clientId: this.deviceId,
      protocolVersion: 4,
      clean: true,
      reconnectPeriod: 0,
      ca: c.caPem,
      cert: c.certPem,
      key: this.tlsKeyPem(),
      rejectUnauthorized: true,
      servername: this.options.servername,
    });
    this.client = client;
    client.on('message', (_topic, payload) => this.onCommand(JSON.parse(payload.toString('utf8'))));
    await client.subscribeAsync(this.topic('cmd'), { qos: 1 });
    this.publishHello();
    this.publishState();
    this.log('connected');
  }

  async disconnect(): Promise<void> {
    const client = this.client;
    this.client = null;
    if (client) await client.endAsync();
  }

  private publishHello(): void {
    this.client?.publish(this.topic('hello'), JSON.stringify({
      fw_version: this.fwVersion, boot_count: this.bootCount,
      next_seq: this.nextSeqValue(), oldest_seq: this.oldestSeq, uptime_ms: Date.now() - this.startedAt,
      clock: this.clock, ota: { partition: this.partition, pending_verify: false },
    }), { qos: 1 });
  }

  private onCommand(cmd: Command): void {
    if (cmd.cmd === 'sync') {
      this.acked = Math.max(this.acked, cmd.have_seq ?? -1);
      this.nextToSend = Math.max((cmd.have_seq ?? -1) + 1, this.oldestSeq);
    } else if (cmd.cmd === 'ack') {
      this.acked = Math.max(this.acked, cmd.seq ?? -1);
    } else if (cmd.cmd === 'resend') {
      this.nextToSend = Math.max(cmd.from_seq ?? 0, this.oldestSeq);
    } else if (cmd.cmd === 'media_ack' && cmd.sha256) {
      this.media.delete(cmd.sha256);
    } else if (cmd.cmd === 'trace_ack' && cmd.sha256) {
      this.traces.delete(cmd.sha256);
    } else if (cmd.cmd === 'time' && typeof cmd.unix_ms === 'number') {
      this.onTime(cmd.unix_ms);
    } else if (cmd.cmd === 'ota' && cmd.sha256 && cmd.version) {
      this.otaTask = this.onOta(cmd);
    }
    this.pump();
  }

  /** The gateway's clock: taken on the first command of a boot, and afterwards only a step of 2 s or more, as the firmware does. */
  private onTime(unixMs: number): void {
    const local = (this.options.clock ?? Date.now)() + this.clockOffsetMs;
    const step = Math.round(unixMs - local);
    if (this.clock === 'gateway' && Math.abs(step) < CLOCK_STEP_MS) return;
    this.clockOffsetMs += step;
    this.clock = 'gateway';
    this.append('clock_sync', { source: 'gateway', step_ms: Math.abs(step), direction: step >= 0 ? 'forward' : 'back' });
  }

  private download(url: string): Promise<Buffer> {
    const c = this.credentials!;
    return new Promise((resolve, reject) => {
      const req = request(url, { ca: c.caPem, cert: c.certPem, key: this.tlsKeyPem(), servername: this.options.servername, agent: false }, (res) => {
        const chunks: Buffer[] = [];
        res.on('data', (d: Buffer) => chunks.push(d));
        res.on('end', () => (res.statusCode === 200 ? resolve(Buffer.concat(chunks)) : reject(new Error(`HTTP ${res.statusCode}`))));
        res.on('error', reject);
      });
      req.on('error', reject);
      req.end();
    });
  }

  private async onOta(cmd: Command): Promise<void> {
    const behaviour = this.options.ota ?? 'confirm';
    if (behaviour === 'ignore') return;
    const from = this.fwVersion;
    const version = String(cmd.version);
    const image = Buffer.from(String(cmd.sha256), 'hex');
    const report = (phase: string, reason = '') => this.append('fw_update', { phase, version, from_version: from, image, reason });

    report('accepted');
    if (this.options.otaDownload && cmd.url) {
      try {
        const bytes = await this.download(cmd.url);
        if (toHex(sha256(bytes)) !== cmd.sha256 || (cmd.size !== undefined && bytes.length !== cmd.size)) {
          report('failed', 'the downloaded image does not match its hash and size');
          return;
        }
      } catch (err) {
        report('failed', `download failed: ${err instanceof Error ? err.message : String(err)}`);
        return;
      }
    }
    report('downloaded');

    /* The reboot into the other slot. */
    this.bootCount += 1;
    this.append('boot', { fw_version: version, reset_reason: 'software', boot_count: this.bootCount });
    if (behaviour === 'fail') {
      report('failed', 'not confirmed within 60 s');
      this.bootCount += 1;
      this.append('boot', { fw_version: from, reset_reason: 'software', boot_count: this.bootCount });
      report('rolled_back', 'not confirmed within 60 s');
    } else {
      this.fwVersion = version;
      this.partition = this.partition === 'ota_0' ? 'ota_1' : 'ota_0';
      report('confirmed');
    }
    this.publishHello();
    this.log(`ota ${version}: ${behaviour === 'fail' ? 'rolled back' : 'confirmed'}`);
  }

  /** Send what the gateway lacks, at most WINDOW entries beyond the last acknowledgement. */
  private pump(): void {
    const client = this.client;
    if (!client?.connected) return;
    while (this.nextToSend <= this.lastSeq && this.nextToSend <= this.acked + WINDOW) {
      const entry = this.flash.find((e) => e.seq === this.nextToSend);
      this.nextToSend += 1;
      if (entry) client.publish(this.topic('ledger'), JSON.stringify(entry), { qos: 1 });
    }
    for (const [sha, bytes] of this.media) client.publish(`${this.topic('media')}/${sha}`, bytes, { qos: 1 });
    for (const [sha, bytes] of this.traces) client.publish(`${this.topic('trace')}/${sha}`, bytes, { qos: 1 });
  }

  private now(): number {
    return Math.floor(((this.options.clock ?? Date.now)() + this.clockOffsetMs) / 1000);
  }

  append(type: EventType, payload: Record<string, unknown>): EntryJson {
    const entry = entryToJson(this.writer.append(type, payload as never, this.now(), this.clock));
    this.flash.push(entry);
    this.pump();
    return entry;
  }

  /** Simulate the node's ring buffer removing its oldest entries. Logs the loss if any were never mirrored. */
  rotate(count: number): void {
    const removed = this.flash.splice(0, Math.min(count, this.flash.length));
    const unmirrored = removed.filter((e) => e.seq > this.acked);
    if (unmirrored.length) {
      this.append('log_overwrite', { first_seq: unmirrored[0].seq, last_seq: unmirrored[unmirrored.length - 1].seq, mirrored: false });
    }
  }

  publishState(): void {
    this.client?.publish(this.topic('state'), JSON.stringify({
      fusion: this.fusion, alarm: this.alarm, faults: { sensor: false, ledger: false, selftest: false, buzzer: false }, uptime_ms: 0,
    }), { qos: 1, retain: true });
  }

  publishTelemetry(scores: Record<string, number>): void {
    this.client?.publish(this.topic('telemetry'), JSON.stringify({ uptime_ms: 0, scores, fan: { state: 'on', duty: 100, rpm: 1450 } }), { qos: 0 });
  }

  private setState(to: string, reason: string[], scores: Record<string, number>): EntryJson {
    this.lastScores = scores;
    const entry = this.append('state_change', { from: this.fusion, to, reason, scores });
    this.fusion = to;
    this.alarm = to;
    this.publishState();
    this.publishTelemetry(scores);
    return entry;
  }

  /** One modality crossing its threshold. The escalation dry run times the alert path from here. */
  crossThreshold(modality: string, score: number): EntryJson {
    return this.append('threshold_cross', { modality, score, direction: 'up' });
  }

  /** Enter CRITICAL now, on these agreeing modalities. Returns the state_change entry. */
  raiseCritical(reasons: string[], scores: Record<string, number>): EntryJson {
    return this.setState('CRITICAL', reasons, scores);
  }

  /** Back to NORMAL, which closes the incident on the gateway. */
  settle(scores: Record<string, number> = { gas: 8, smoke: 5, flame: 0, heat: 11, humidity: 48 }): EntryJson {
    return this.setState('NORMAL', [], scores);
  }

  /**
   * Record an episode trace as the firmware does once an episode is over: the
   * file, a trace_captured entry carrying its hash, and the file on its own
   * topic until the gateway acknowledges it. Returns the file's SHA-256.
   */
  captureTrace(options: { samples?: TraceSample[]; preSamples?: number; periodMs?: number; trigger?: string } = {}): string {
    const samples = options.samples ?? quietEpisode();
    const periodMs = options.periodMs ?? 500;
    const preSamples = options.preSamples ?? 10;
    const trigger = options.trigger ?? 'ELEVATED';
    const uptime = Date.now() - this.startedAt;
    const bytes = encodeTrace({
      periodMs, preSamples, trigger, bootCount: this.bootCount, samples,
      triggerUptimeMs: Math.max(preSamples * periodMs, uptime - (samples.length - preSamples) * periodMs),
    });
    const sha = toHex(sha256(bytes));
    this.traces.set(sha, bytes);
    this.append('trace_captured', { trigger, trace: Buffer.from(sha, 'hex'), samples: samples.length, period_ms: periodMs, pre_samples: preSamples });
    return sha;
  }

  /** The M3 demonstration story: warm-up, gas, heat agrees, CRITICAL with photos, silence, recovery. */
  runIncident(): void {
    const quiet = { gas: 8, smoke: 5, flame: 0, heat: 10, humidity: 45 };
    this.append('boot', { fw_version: this.fwVersion, reset_reason: 'power_on', boot_count: this.bootCount });
    this.append('self_test', { result: 'pass', checks: { heater: true, buzzer: true, flame_loopback: true, fan: true, camera: true, ledger: true, heap: true } });
    this.setState('NORMAL', [], quiet);
    this.append('threshold_cross', { modality: 'gas', score: 66, direction: 'up' });
    this.setState('ELEVATED', ['gas'], { ...quiet, gas: 66 });
    this.append('threshold_cross', { modality: 'heat', score: 63, direction: 'up' });
    this.setState('CRITICAL', ['gas', 'heat'], { ...quiet, gas: 81, heat: 63, smoke: 34 });

    const frames: Uint8Array[] = []; /* byte strings, as SPEC.md defines them; the JSON transport shows them as hex */
    for (let i = 0; i < 3; i++) {
      const jpeg = flatGreyJpeg(64, 48, 70 + 60 * i, `firex-sim frame ${i} ${randomBytes(4).toString('hex')}`);
      const sha = toHex(sha256(jpeg));
      this.media.set(sha, jpeg);
      frames.push(Buffer.from(sha, 'hex'));
    }
    this.append('media_captured', { trigger: 'CRITICAL', frames, clip: null, codec: 'jpeg', resolution: '64x48' });
    this.append('silence', { seconds: 300, by: 'button' });
    this.setState('ELEVATED', ['gas'], { ...quiet, gas: 52, heat: 38 });
    this.setState('NORMAL', [], quiet);
  }

  /** A normal day: boot, a passing self-test, warm-up done. */
  runNormal(): void {
    this.append('boot', { fw_version: this.fwVersion, reset_reason: 'power_on', boot_count: this.bootCount });
    this.append('self_test', { result: 'pass', checks: { heater: true, buzzer: true, fan: true, camera: true, ledger: true, heap: true } });
    this.setState('NORMAL', [], { gas: 8, smoke: 5, flame: 0, heat: 11, humidity: 48 });
  }

  /** Steam from a shower or a kettle lifts gas on its own: ELEVATED, never CRITICAL. */
  runElevated(): void {
    this.runNormal();
    this.append('threshold_cross', { modality: 'gas', score: 64, direction: 'up' });
    this.setState('ELEVATED', ['gas'], { gas: 64, smoke: 9, flame: 0, heat: 12, humidity: 91 });
  }

  /** An ELEVATED episode that settles, and its trace: the shape quietEpisode() records. */
  runTrace(): string {
    this.runElevated();
    this.settle();
    return this.captureTrace();
  }

  /** Stays CRITICAL with photos: for showing the dashboard's alarm state. */
  runCritical(): void {
    this.runNormal();
    this.append('threshold_cross', { modality: 'smoke', score: 71, direction: 'up' });
    this.append('threshold_cross', { modality: 'heat', score: 66, direction: 'up' });
    this.setState('CRITICAL', ['smoke', 'heat'], { gas: 22, smoke: 71, flame: 12, heat: 66, humidity: 40 });
    const frames: Uint8Array[] = [];
    for (let i = 0; i < 3; i++) {
      const jpeg = flatGreyJpeg(64, 48, 90 + 50 * i, `firex-sim critical frame ${i} ${randomBytes(4).toString('hex')}`);
      this.media.set(toHex(sha256(jpeg)), jpeg);
      frames.push(sha256(jpeg));
    }
    this.append('media_captured', { trigger: 'CRITICAL', frames, clip: null, codec: 'jpeg', resolution: '64x48' });
  }

  /** Periodic reports while connected, scores wandering a little around their last values. */
  lastScores: Record<string, number> = { gas: 8, smoke: 5, flame: 0, heat: 11, humidity: 48 };

  heartbeat(): void {
    const jitter = (v: number) => Math.max(0, Math.min(100, Math.round(v + (Math.random() - 0.5) * 4)));
    this.publishTelemetry(Object.fromEntries(Object.entries(this.lastScores).map(([k, v]) => [k, jitter(v)])));
  }

  /** Keys, credentials and the simulated flash, so a restarted simulator is the same unit with the same chain. */
  save(path: string): void {
    writeFileSync(path, JSON.stringify({
      ledger_key: this.ledgerKey.privateKey.export({ type: 'pkcs8', format: 'pem' }),
      tls_key: this.tlsKey.privateKey.export({ type: 'pkcs8', format: 'pem' }),
      credentials: this.credentials,
      boot_count: this.bootCount,
      fw_version: this.fwVersion,
      partition: this.partition,
      flash: this.flash,
      next_seq: this.writer.entries.length === 0 && this.flash.length ? this.lastSeq + 1 : this.nextSeqValue(),
      fusion: this.fusion,
    }, null, 2));
  }

  private resumedNextSeq = 0;

  private nextSeqValue(): number {
    return this.resumedNextSeq + this.writer.entries.length;
  }

  static load(path: string, options: SimNodeOptions): SimNode {
    const saved = JSON.parse(readFileSync(path, 'utf8'));
    const pair = (pem: string): KeyPair => {
      const privateKey = createPrivateKey(pem);
      return { privateKey, publicKey: createPublicKey(privateKey) };
    };
    const node = new SimNode({ ...options, fwVersion: options.fwVersion ?? saved.fw_version }, { ledger: pair(saved.ledger_key), tls: pair(saved.tls_key) });
    node.credentials = saved.credentials;
    node.bootCount = Number(saved.boot_count ?? 0) + 1;
    node.partition = saved.partition === 'ota_1' ? 'ota_1' : 'ota_0';
    const flash = (saved.flash ?? []) as EntryJson[];
    node.flash.push(...flash);
    if (flash.length) {
      const last = flash[flash.length - 1];
      node.resumedNextSeq = last.seq + 1;
      node.writer.resume(last.seq + 1, fromHex(last.hash));
    }
    node.fusion = 'WARMUP';
    return node;
  }
}

async function main(): Promise<number> {
  const args = process.argv.slice(2);
  const flag = (name: string) => {
    const i = args.indexOf(`--${name}`);
    return i >= 0 ? args[i + 1] : undefined;
  };
  const statePath = flag('state') ?? 'sim-node.json';
  const ota = flag('ota');
  const options: SimNodeOptions = {
    mqttUrl: flag('mqtt-url'), servername: flag('servername'), log: console.log,
    ota: ota === 'fail' || ota === 'ignore' ? ota : 'confirm', otaDownload: args.includes('--ota-download'),
  };
  let node: SimNode;
  if (existsSync(statePath)) {
    node = SimNode.load(statePath, options);
  } else {
    const code = flag('code');
    const provisionUrl = flag('provision') ?? 'http://localhost:8081';
    if (!code) {
      console.error('usage: sim:node -- --code <pairing code> [--provision http://gateway:8081] [--state sim-node.json] [--mqtt-url mqtts://host:8883] [--scenario incident|normal|elevated|critical|trace] [--stay] [--ota confirm|fail|ignore] [--ota-download]');
      return 2;
    }
    node = new SimNode(options);
    await node.provision(provisionUrl, code);
    node.save(statePath);
    console.log(`saved identity and credentials to ${statePath}`);
  }
  await node.connect();
  const scenario = flag('scenario') ?? 'incident';
  if (scenario === 'incident') node.runIncident();
  else if (scenario === 'normal') node.runNormal();
  else if (scenario === 'elevated') node.runElevated();
  else if (scenario === 'critical') node.runCritical();
  else if (scenario === 'trace') node.runTrace();
  node.save(statePath);

  if (args.includes('--stay')) {
    console.log('staying connected; Ctrl+C to stop');
    node.heartbeat();
    const beat = setInterval(() => node.heartbeat(), 10_000);
    const state = setInterval(() => node.publishState(), 60_000);
    const stop = async () => {
      clearInterval(beat);
      clearInterval(state);
      node.save(statePath);
      await node.disconnect();
      process.exit(0);
    };
    process.on('SIGINT', stop);
    process.on('SIGTERM', stop);
    return new Promise<number>(() => undefined);
  }

  const deadline = Date.now() + 30_000;
  while ((node.ackedSeq < node.lastSeq || node.pendingMedia > 0 || node.pendingTraces > 0) && Date.now() < deadline) {
    await new Promise((r) => setTimeout(r, 200));
  }
  console.log(`device ${node.deviceId}: ${node.ackedSeq + 1} entries acknowledged, ${node.pendingMedia} images and ${node.pendingTraces} traces pending`);
  node.save(statePath);
  await node.disconnect();
  return node.ackedSeq === node.lastSeq && node.pendingMedia === 0 && node.pendingTraces === 0 ? 0 : 1;
}

if (process.argv[1]?.replace(/\\/g, '/').endsWith('gateway/sim/node-sim.ts')) {
  main().then((code) => process.exit(code), (err) => {
    console.error(String(err instanceof Error ? err.message : err));
    process.exit(1);
  });
}
