import { mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { describe as suite, expect, test } from 'vitest';

import { isEmergencyNumber, loadRegistry, maskRecipient, parseRegistry, phoneKey, RegistryError } from '../escalation/registry.ts';

const EXAMPLE = fileURLToPath(new URL('../escalation/registry.example.json', import.meta.url));
const example = () => JSON.parse(readFileSync(EXAMPLE, 'utf8')) as { contacts: Record<string, unknown>[] } & Record<string, unknown>;

suite('the site service registry', () => {
  test('the example registry loads with all four recipients, every service a drill contact', () => {
    const registry = loadRegistry(EXAMPLE);
    expect(registry.contacts.map((c) => c.service)).toEqual(['site', 'fire', 'police', 'ambulance']);
    expect(registry.contacts[1].backup?.recipient).toBe('+910000000017');
    expect(registry.contacts.filter((c) => c.service !== 'site').map((c) => c.mode)).toEqual(['drill', 'drill', 'drill']);
    expect(registry).toMatchObject({ version: 2, ack_timeout_s: 60, max_realerts: 1, offline_notify_min: 10 });
  });

  test('a site with no ambulance contact is an incomplete installation', () => {
    const doc = example();
    doc.contacts = doc.contacts.filter((c) => c.service !== 'ambulance');
    expect(() => parseRegistry(doc)).toThrow(/contacts: names no ambulance contact/);
  });

  test('one entry per service', () => {
    const doc = example();
    doc.contacts.push({ service: 'fire', name: 'Fire station 9', recipient: '+910000000009', channel: 'sms' });
    expect(() => parseRegistry(doc)).toThrow(/names 2 fire contacts/);
  });

  test('errors name the field that is wrong', () => {
    const doc = example();
    doc.contacts[2].channel = 'pager';
    expect(() => parseRegistry(doc)).toThrow('contacts[2].channel: must be one of api, sms, voice, console, dashboard');

    const again = example();
    (again.contacts[1].backup as Record<string, unknown>).recipient = ' ';
    expect(() => parseRegistry(again)).toThrow('contacts[1].backup.recipient: must not be empty');

    const located = example();
    located.location = { lat: 12.9, lon: 'east' };
    expect(() => parseRegistry(located)).toThrow(/location\.lon/);

    const moded = example();
    moded.contacts[3].mode = 'maybe';
    expect(() => parseRegistry(moded)).toThrow('contacts[3].mode: must be one of drill, live');
  });

  test('a file that is not JSON is refused with its path', () => {
    const path = join(mkdtempSync(join(tmpdir(), 'firex-registry-')), 'registry.json');
    writeFileSync(path, '{ "site_id": ');
    expect(() => loadRegistry(path)).toThrow(RegistryError);
    expect(() => loadRegistry(path)).toThrow(path);
  });
});

suite('registry version 2', () => {
  test('a version 1 file loads, and every service in it is a drill contact', () => {
    const doc = example();
    delete doc.version;
    delete doc.ack_timeout_s;
    for (const c of doc.contacts) delete c.mode;
    const registry = parseRegistry(doc);
    expect(registry.version).toBe(1);
    expect(registry.ack_timeout_s).toBeUndefined();
    expect(registry.contacts.every((c) => c.mode === 'drill')).toBe(true);
  });

  test('a live service names its agreement', () => {
    const doc = example();
    doc.contacts[1].mode = 'live';
    expect(() => parseRegistry(doc)).toThrow(/contacts\[1\]\.agreement: a live fire contact names the agreement/);
    doc.contacts[1].agreement = { reference: 'FS-42/2026/pilot', date: '2026-10-01' };
    expect(parseRegistry(doc).contacts[1]).toMatchObject({ mode: 'live', agreement: { reference: 'FS-42/2026/pilot' } });
  });

  test('the site contact is live without an agreement', () => {
    const doc = example();
    doc.contacts[0].mode = 'live';
    expect(parseRegistry(doc).contacts[0].mode).toBe('live');
  });

  test('no recipient is a public emergency number, however it is written', () => {
    for (const number of ['112', '+91 112', '0112', '1-0-1', '+91-100', '(108)', '0091 1098', '181', '1091', '102']) {
      expect(isEmergencyNumber(number), number).toBe(true);
      const doc = example();
      doc.contacts[2].recipient = number;
      expect(() => parseRegistry(doc), number).toThrow(/contacts\[2\]\.recipient: .* is a public emergency number/);
    }
    const backup = example();
    (backup.contacts[1].backup as Record<string, unknown>).recipient = '101';
    expect(() => parseRegistry(backup)).toThrow(/contacts\[1\]\.backup\.recipient/);
    for (const ordinary of ['+919876501112', '1120', '+911000000000', 'station-101']) expect(isEmergencyNumber(ordinary), ordinary).toBe(false);
  });

  test('timings are checked', () => {
    const doc = example();
    doc.ack_timeout_s = 2;
    expect(() => parseRegistry(doc)).toThrow('ack_timeout_s: must be a number of at least 5');
    const again = example();
    again.max_realerts = 1.5;
    expect(() => parseRegistry(again)).toThrow('max_realerts: must be a whole number of at least 0');
  });

  test('phone numbers are masked for the dashboard and matched on their last ten digits', () => {
    expect(maskRecipient('+919876501234')).toBe('+91******1234');
    expect(maskRecipient('98765 01234')).toBe('******1234');
    expect(maskRecipient('station-042')).toBe('station-042');
    expect(phoneKey('+91 98765-01234')).toBe(phoneKey('09876501234'));
  });
});
