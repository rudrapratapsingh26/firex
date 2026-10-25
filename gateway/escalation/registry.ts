/**
 * The site service registry (docs/service-registry.md), loaded and checked
 * when the gateway starts, never at alarm time.
 *
 * Rule 1 of the registry is enforced here: a site names the fire station, the
 * police station, the ambulance service and the site's own desk, once each. A
 * registry that breaks it is refused with the field that is wrong, and the
 * gateway runs without escalation rather than with an incomplete list.
 *
 * Version 2 (M5) adds each contact's mode. A service is `drill` unless the file
 * says `live` and names the agreement that allows it, and no recipient may be a
 * public emergency number (rule 5). Version 1 files still load: every service
 * in them is a drill contact.
 */
import { readFileSync } from 'node:fs';

import type { Channel, ContactMode, Service, ServiceContact, SiteRegistry } from './provider.ts';

export const SERVICES: readonly Service[] = ['fire', 'police', 'ambulance', 'site'];
export const CHANNELS: readonly Channel[] = ['api', 'sms', 'voice', 'console', 'dashboard'];
export const MODES: readonly ContactMode[] = ['drill', 'live'];

/** Public emergency numbers in India. FireX never dials or messages them (rule 5). */
const EMERGENCY_NUMBERS = new Set(['100', '101', '102', '108', '112', '1091', '1098', '181']);

export class RegistryError extends Error {}

function fail(path: string, message: string): never {
  throw new RegistryError(`${path}: ${message}`);
}

function object(value: unknown, path: string): Record<string, unknown> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) fail(path, 'must be an object');
  return value as Record<string, unknown>;
}

function text(doc: Record<string, unknown>, key: string, path: string, allowEmpty = false): string {
  const value = doc[key];
  if (typeof value !== 'string') fail(`${path}${key}`, 'must be a string');
  if (!allowEmpty && value.trim() === '') fail(`${path}${key}`, 'must not be empty');
  return value;
}

function oneOf<T extends string>(doc: Record<string, unknown>, key: string, path: string, allowed: readonly T[]): T {
  const value = doc[key];
  if (typeof value !== 'string' || !allowed.includes(value as T)) fail(`${path}${key}`, `must be one of ${allowed.join(', ')}`);
  return value as T;
}

function coordinate(doc: Record<string, unknown>, key: string, path: string, limit: number): number {
  const value = doc[key];
  if (typeof value !== 'number' || !Number.isFinite(value) || Math.abs(value) > limit) {
    fail(`${path}${key}`, `must be a number between -${limit} and ${limit}`);
  }
  return value;
}

function optionalNumber(doc: Record<string, unknown>, key: string, min: number, integer: boolean): number | undefined {
  const value = doc[key];
  if (value === undefined) return undefined;
  if (typeof value !== 'number' || !Number.isFinite(value) || value < min || (integer && !Number.isInteger(value))) {
    fail(key, `must be ${integer ? 'a whole number' : 'a number'} of at least ${min}`);
  }
  return value;
}

/** Spaces, dashes, dots and brackets removed. */
export function normalisePhone(value: string): string {
  return value.replace(/[\s\-().]/g, '');
}

export function isPhoneNumber(value: string): boolean {
  return /^\+?\d{6,15}$/.test(normalisePhone(value));
}

/** The last ten digits: how an Indian number is recognised whether it arrives as +91, 0 or bare. */
export function phoneKey(value: string): string {
  return normalisePhone(value).replace(/\D/g, '').slice(-10);
}

/** 100, 101, 102, 108, 112, 1091, 1098 and 181, with or without +91, 0091 or 0, in any spacing. */
export function isEmergencyNumber(recipient: string): boolean {
  let n = normalisePhone(recipient);
  if (n.startsWith('+91')) n = n.slice(3);
  else if (n.startsWith('0091')) n = n.slice(4);
  return EMERGENCY_NUMBERS.has(n.replace(/^0+/, ''));
}

/** `+919876501234` becomes `+91******1234`; anything that is not a phone number is shown as it is. */
export function maskRecipient(recipient: string): string {
  if (!isPhoneNumber(recipient)) return recipient;
  const n = normalisePhone(recipient);
  const prefix = n.startsWith('+') ? n.slice(0, 3) : '';
  const digits = n.slice(prefix.length);
  return `${prefix}${'*'.repeat(Math.max(0, digits.length - 4))}${digits.slice(-4)}`;
}

function recipient(doc: Record<string, unknown>, path: string): string {
  const value = text(doc, 'recipient', path);
  if (isEmergencyNumber(value)) {
    fail(`${path}recipient`, `${value} is a public emergency number; FireX never dials or messages 100, 101, 102, 108, 112, 1091, 1098 or 181 (rule 5)`);
  }
  return value;
}

export function parseRegistry(input: unknown): SiteRegistry {
  const doc = object(input, 'registry');
  const location = object(doc.location, 'location');
  if (!Array.isArray(doc.contacts)) fail('contacts', 'must be an array');
  const version = optionalNumber(doc, 'version', 1, true) ?? 1;
  if (version > 2) fail('version', `${version} is newer than this gateway understands (2)`);

  const contacts: ServiceContact[] = doc.contacts.map((raw, i) => {
    const path = `contacts[${i}].`;
    const c = object(raw, `contacts[${i}]`);
    const contact: ServiceContact = {
      service: oneOf(c, 'service', path, SERVICES),
      name: text(c, 'name', path),
      recipient: recipient(c, path),
      channel: oneOf(c, 'channel', path, CHANNELS),
      mode: c.mode === undefined ? 'drill' : oneOf(c, 'mode', path, MODES),
    };
    if (c.backup !== undefined) {
      const b = object(c.backup, `${path}backup`);
      contact.backup = {
        name: text(b, 'name', `${path}backup.`),
        recipient: recipient(b, `${path}backup.`),
        channel: oneOf(b, 'channel', `${path}backup.`, CHANNELS),
      };
    }
    if (c.agreement !== undefined) {
      const a = object(c.agreement, `${path}agreement`);
      contact.agreement = { reference: text(a, 'reference', `${path}agreement.`), date: text(a, 'date', `${path}agreement.`) };
    }
    if (contact.mode === 'live' && contact.service !== 'site' && !contact.agreement) {
      fail(`${path}agreement`, `a live ${contact.service} contact names the agreement that allows real alerts ({"reference", "date"}); without one it stays a drill contact`);
    }
    return contact;
  });

  for (const service of SERVICES) {
    const count = contacts.filter((c) => c.service === service).length;
    if (count === 0) fail('contacts', `names no ${service} contact; every site names fire, police, ambulance and site before it goes live`);
    if (count > 1) fail('contacts', `names ${count} ${service} contacts; one entry per service, with a backup for a second number`);
  }

  const registry: SiteRegistry = {
    site_id: text(doc, 'site_id', ''),
    site_name: text(doc, 'site_name', ''),
    address: text(doc, 'address', '', true),
    location: { lat: coordinate(location, 'lat', 'location.', 90), lon: coordinate(location, 'lon', 'location.', 180) },
    contacts,
    version,
  };
  const ack = optionalNumber(doc, 'ack_timeout_s', 5, false);
  const realerts = optionalNumber(doc, 'max_realerts', 0, true);
  const offline = optionalNumber(doc, 'offline_notify_min', 1, false);
  if (ack !== undefined) registry.ack_timeout_s = ack;
  if (realerts !== undefined) registry.max_realerts = realerts;
  if (offline !== undefined) registry.offline_notify_min = offline;
  return registry;
}

export function loadRegistry(path: string): SiteRegistry {
  let doc: unknown;
  try {
    doc = JSON.parse(readFileSync(path, 'utf8'));
  } catch (err) {
    throw new RegistryError(`${path}: ${err instanceof Error ? err.message : String(err)}`);
  }
  return parseRegistry(doc);
}

/** The site's contact: the one recipient every alert, late report and notice reaches. */
export function siteContact(registry: SiteRegistry): ServiceContact {
  return registry.contacts.find((c) => c.service === 'site')!;
}
