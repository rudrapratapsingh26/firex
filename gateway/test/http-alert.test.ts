import { mkdtempSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { afterEach, beforeEach, describe as suite, expect, test } from 'vitest';

import { HttpAlert } from '../escalation/http-alert.ts';
import { parseAck, startMockEndpoints, type MockEndpoints } from '../escalation/mock-endpoints.ts';
import type { Alert, ServiceContact } from '../escalation/provider.ts';
import { parseRegistry } from '../escalation/registry.ts';

const site = parseRegistry({
  site_id: 'site-001', site_name: 'Hostel, Block B', address: '', location: { lat: 0, lon: 0 },
  contacts: [
    { service: 'site', name: 'Warden on duty', recipient: 'warden-1', channel: 'sms' },
    { service: 'fire', name: 'Fire station 42', recipient: 'station-042', channel: 'sms' },
    { service: 'police', name: 'Police station 17', recipient: 'ps-017', channel: 'sms' },
    { service: 'ambulance', name: 'EMS north', recipient: 'ems/north', channel: 'api' },
  ],
});
const contact = (service: string): ServiceContact => site.contacts.find((c) => c.service === service)!;

const alert: Alert = {
  incident_id: '7', site, node_id: 'ab'.repeat(16), where: 'Room 206', hazard: 'gas + heat agree',
  scores: { gas: 81, heat: 64 }, media: { frames: 0, clip_seconds: 0, hashes: [] },
  ledger_seq: 12, ledger_hash: 'cd'.repeat(32), raised_at: 1789000950.5, ledger_received_at_ms: 1789000950400,
};

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

let endpoints: MockEndpoints;
let logPath: string;

beforeEach(async () => {
  logPath = join(mkdtempSync(join(tmpdir(), 'firex-mock-')), 'endpoints.jsonl');
  endpoints = await startMockEndpoints({
    ack: { fire: { kind: 'fixed', seconds: 0.15 }, police: { kind: 'silent' } },
    defaultAck: { kind: 'fixed', seconds: 0.05 },
    logPath,
  });
});

afterEach(async () => {
  await endpoints.close();
});

const log = () => readFileSync(logPath, 'utf8').trim().split('\n').map((line) => JSON.parse(line) as Record<string, unknown>);

suite('HttpAlert against the mock endpoints', () => {
  test('an alert is delivered with a receipt, and the answer comes after the configured delay', async () => {
    const provider = new HttpAlert({ baseUrl: endpoints.url });
    const receipt = await provider.send(alert, contact('fire'));
    expect(receipt).toMatchObject({ service: 'fire', channel: 'sms', recipient: 'station-042' });
    expect(receipt.receipt_id).toMatch(/^m-\d{6}$/);
    expect(receipt.delivered_at).toBeGreaterThanOrEqual(receipt.sent_at - 0.005);
    expect(await provider.ack(receipt.receipt_id)).toBeNull();

    await sleep(300);
    const ack = await provider.ack(receipt.receipt_id);
    expect(ack).toMatchObject({ receipt_id: receipt.receipt_id, service: 'fire', by: 'station-042' });
    expect(ack!.at - receipt.delivered_at!).toBeCloseTo(0.15, 3);
  });

  test('the log carries every field the latency tools read; a silent service never answers', async () => {
    const provider = new HttpAlert({ baseUrl: endpoints.url });
    const receipt = await provider.send(alert, contact('police'));
    await provider.send(alert, contact('ambulance'), { retry_of: 'm-000099' });
    await sleep(200);
    expect(await provider.ack(receipt.receipt_id)).toBeNull();

    const records = log();
    expect(records.find((r) => r.event === 'delivered' && r.service === 'police')).toEqual({
      event: 'delivered', service: 'police', recipient: 'ps-017', channel: 'sms', receipt_id: receipt.receipt_id,
      incident_id: '7', node_id: 'ab'.repeat(16), ledger_seq: 12, ledger_hash: 'cd'.repeat(32),
      ledger_received_at_ms: 1789000950400, retry_of: null, received_at_ms: Math.round(receipt.delivered_at! * 1000),
    });
    const ambulance = records.find((r) => r.event === 'delivered' && r.service === 'ambulance')!;
    expect(ambulance).toMatchObject({ recipient: 'ems/north', retry_of: 'm-000099' });
    expect(records.filter((r) => r.event === 'acked').map((r) => r.service)).toEqual(['ambulance']);
  });

  test('an endpoint that keeps failing is reported once the attempts run out', async () => {
    const failing = await startMockEndpoints({ failRate: 1 });
    try {
      const provider = new HttpAlert({ baseUrl: failing.url, attempts: 2, backoffMs: 1 });
      await expect(provider.send(alert, contact('site'))).rejects.toThrow(/not delivered after 2 attempts: HTTP 503/);
      expect(failing.records.map((r) => r.event)).toEqual(['rejected', 'rejected']);
    } finally {
      await failing.close();
    }
  });

  test('an unreachable endpoint is a failed send, not a crash', async () => {
    const provider = new HttpAlert({ baseUrl: 'http://127.0.0.1:1', attempts: 1, timeoutMs: 500 });
    await expect(provider.send(alert, contact('site'))).rejects.toThrow(/not delivered/);
  });
});

suite('mock endpoint settings', () => {
  test('ack behaviours', () => {
    expect(parseAck('silent')).toEqual({ kind: 'silent' });
    expect(parseAck('20-40')).toEqual({ kind: 'range', min: 20, max: 40 });
    expect(parseAck('2.5')).toEqual({ kind: 'fixed', seconds: 2.5 });
    expect(() => parseAck('soon')).toThrow();
    expect(() => parseAck('40-20')).toThrow();
  });
});
