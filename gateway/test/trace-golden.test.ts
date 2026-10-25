import { readFileSync } from 'node:fs';

import { describe as suite, expect, test } from 'vitest';

import { encodeTrace, parseTrace } from '../server/traces.ts';

/* firmware/test/trace_golden.fxt is written by the node's C encoder (`make -C firmware
 * trace-golden`); the gateway must read exactly what the firmware writes, and back. */
suite("the node's golden trace file", () => {
  const bytes = readFileSync('firmware/test/trace_golden.fxt');

  test('parses as the firmware wrote it', () => {
    const trace = parseTrace(bytes);
    expect(trace).toMatchObject({ version: 1, periodMs: 500, preSamples: 4, trigger: 'ELEVATED', triggerUptimeMs: 4000, bootCount: 7 });
    expect(trace.samples).toHaveLength(10);
    expect(trace.samples[0]).toMatchObject({ scores: [0.05, 0.04, 0, 0.1, 0.45], fusion: 'NORMAL', alarm: 'NORMAL', fanDuty: 100, fanState: 'on' });
    expect(trace.samples[4]).toMatchObject({ fusion: 'ELEVATED', alarm: 'ELEVATED' });
    expect(trace.samples[4].scores[0]).toBe(0.65);
    expect(trace.samples[6]).toMatchObject({ fusion: 'CRITICAL', alarm: 'CRITICAL' });
    expect(trace.samples[6].scores[4]).toBeNull();
  });

  test("the gateway's encoder writes the same bytes", () => {
    const { version: _version, ...rest } = parseTrace(bytes);
    expect(Buffer.compare(encodeTrace(rest), bytes)).toBe(0);
  });
});
