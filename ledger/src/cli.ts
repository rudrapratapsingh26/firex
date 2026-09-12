#!/usr/bin/env node
/**
 * firex-verify — the command-line half of the verifier.
 *
 *   node dist/cli.js verify <log.json> [--key device.pem] [--media <dir>]
 *   node dist/cli.js demo
 *
 * `verify` takes an exported log (or any vector file) and answers PASS or
 * TAMPERED at entry N. `demo` is the M0 milestone in one command: build a
 * signed incident log, verify it, flip one byte, verify it again.
 */
import { createHash } from 'node:crypto';
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';

import { toHex } from './cbor.ts';
import { entryFromJson, publicKeyFromPem, type EntryJson } from './entry.ts';
import { buildVectors } from './vectors.ts';
import { describe, verifyLog } from './verify.ts';

function mediaHashes(dir: string): Set<string> {
  const hashes = new Set<string>();
  for (const name of readdirSync(dir)) {
    const path = join(dir, name);
    if (!statSync(path).isFile()) continue;
    hashes.add(toHex(new Uint8Array(createHash('sha256').update(readFileSync(path)).digest())));
  }
  return hashes;
}

function cmdVerify(args: string[]): number {
  const [file] = args;
  if (!file) {
    console.error('usage: firex-verify verify <log.json> [--key device.pem] [--media <dir>]');
    return 2;
  }
  const keyArg = args.indexOf('--key');
  const mediaArg = args.indexOf('--media');
  const doc = JSON.parse(readFileSync(file, 'utf8')) as {
    log?: EntryJson[]; public_key_pem?: string; media?: string[];
  } | EntryJson[];

  const entriesJson = Array.isArray(doc) ? doc : doc.log ?? [];
  const pem = keyArg >= 0
    ? readFileSync(args[keyArg + 1], 'utf8')
    : (!Array.isArray(doc) && doc.public_key_pem) || '';
  if (!pem) {
    console.error('no device public key: pass --key device.pem, or use a log bundle that carries one');
    return 2;
  }

  let media: Set<string> | undefined;
  if (mediaArg >= 0) media = mediaHashes(args[mediaArg + 1]);
  else if (!Array.isArray(doc) && doc.media) media = new Set(doc.media);

  const result = verifyLog(entriesJson.map(entryFromJson), publicKeyFromPem(pem), media);
  console.log(describe(result));
  return result.ok ? 0 : 1;
}

/** The M0 milestone: an incident log that verifies, then fails after one byte changes. */
function cmdDemo(): number {
  const valid = buildVectors()[0];
  const key = publicKeyFromPem(valid.public_key_pem);
  const entries = valid.log.map(entryFromJson);

  console.log('FireX ledger demo — a gas leak that becomes a fire\n');
  for (const e of entries) {
    const detail = e.type === 'state_change' ? `${e.payload.from} -> ${e.payload.to}` : e.type;
    console.log(`  #${String(e.seq).padStart(2, '0')}  ${new Date(e.ts * 1000).toISOString().slice(11, 19)}  ${detail}`);
  }

  const first = verifyLog(entries, key, new Set(valid.media));
  console.log(`\n  as exported:   ${describe(first)}`);

  // Flip one byte in the payload of the CRITICAL transition — the edit a
  // facility manager would make after an incident.
  const tampered = entries.map((e) => ({ ...e, payload: { ...e.payload } }));
  tampered[6].payload = { ...tampered[6].payload, to: 'ELEVATED' };
  const second = verifyLog(tampered, key, new Set(valid.media));
  console.log(`  after an edit: ${describe(second)}`);

  return first.ok && !second.ok ? 0 : 1;
}

const [cmd, ...rest] = process.argv.slice(2);
const code = cmd === 'verify' ? cmdVerify(rest)
  : cmd === 'demo' ? cmdDemo()
  : (console.error('usage: firex-verify <verify|demo> [...]'), 2);
process.exit(code);
