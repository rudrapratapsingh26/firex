/**
 * The broker access rules of infra/mosquitto/acl, as functions, for the
 * in-process brokers used by the tests and the laptop dev broker. A client's
 * identity is the CN of its client certificate.
 */
import type { TLSSocket } from 'node:tls';

export const SERVER_CN = 'firex-server';
const NODE_WRITES = ['hello', 'state', 'telemetry', 'ledger'];

export function certificateCommonName(conn: unknown): string {
  const cert = (conn as TLSSocket | undefined)?.getPeerCertificate?.();
  return cert?.subject?.CN ? String(cert.subject.CN) : '';
}

/** topic write firex/+/+/cmd for the server; firex/+/%u/{hello,state,telemetry,ledger,media/+,trace/+} for a node. */
export function mayPublish(cn: string, topic: string): boolean {
  const parts = topic.split('/');
  if (parts[0] !== 'firex' || parts.length < 4) return false;
  if (cn === SERVER_CN) return parts.length === 4 && parts[3] === 'cmd';
  if (parts[2] !== cn) return false;
  return (parts.length === 4 && NODE_WRITES.includes(parts[3])) || (parts.length === 5 && (parts[3] === 'media' || parts[3] === 'trace'));
}

/** topic read firex/# for the server; firex/+/%u/cmd for a node. */
export function maySubscribe(cn: string, topic: string): boolean {
  if (cn === SERVER_CN) return topic.startsWith('firex/');
  const parts = topic.split('/');
  return parts.length === 4 && parts[0] === 'firex' && parts[2] === cn && parts[3] === 'cmd';
}
