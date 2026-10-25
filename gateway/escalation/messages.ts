/**
 * What an alert says.
 *
 * Short, plain ASCII, and one SMS (160 characters) where it fits: a message
 * split in two can arrive in the wrong order, or half of it not at all. The
 * site name, the unit's location and the hazard are shortened before anything
 * else is, so "Reply 1 to acknowledge" is never the part that is cut. A drill
 * says so before it says anything else.
 */
import type { Alert, ServiceContact } from './provider.ts';

export const SMS_LIMIT = 160;
export const DRILL_PREFIX = 'DRILL - NOT A REAL INCIDENT';
const REPLY = ' Reply 1 to acknowledge.';

/** Characters a GSM modem in text mode sends without surprises. */
export function plain(text: string): string {
  return text.normalize('NFKD').replace(/[^\x20-\x7e]/g, ' ').replace(/\s+/g, ' ').trim();
}

/** `head + middle + tail`, with only the middle shortened to fit one SMS. */
function fit(head: string, middle: string, tail: string): string {
  const room = SMS_LIMIT - head.length - tail.length;
  if (middle.length <= room) return `${head}${middle}${tail}`;
  return `${head}${middle.slice(0, Math.max(0, room - 2)).trimEnd()}..${tail}`;
}

export function alertText(alert: Alert, contact?: ServiceContact): string {
  const site = plain(alert.site.site_name).slice(0, 40);
  const what = plain(`${alert.where} ${alert.hazard}`);
  switch (alert.kind ?? 'incident') {
    case 'drill':
      return fit(`${DRILL_PREFIX}. FIREX ${site}: test alert`, contact ? ` for ${plain(contact.name)}` : '', `.${REPLY}`);
    case 'late':
      return fit(`FIREX LATE ${site}: `, what, `. Reported late: unit was offline.${REPLY}`);
    case 'notice':
      return plain(alert.notice_text ?? '').slice(0, SMS_LIMIT);
    default:
      return fit(`FIREX CRITICAL ${site}: `, what, `.${REPLY}`);
  }
}
