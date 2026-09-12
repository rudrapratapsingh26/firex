# ADR 0001 — TypeScript runs without a build step in M0

Date: 2026-09-12
Status: accepted, revisit at M3

## Context

The ledger verifier, the escalation service and (from M3) the gateway are TypeScript. M0
has one developer and no deployment target: everything runs on a laptop or in CI. A build
step at this stage costs time on every run and adds a `dist/` directory to reason about.

Node 22 strips types and executes `.ts` sources directly, but its strip-only mode has two
constraints:

1. It does not rewrite `.js` import specifiers to `.ts`, so imports must name the file
   that actually exists on disk.
2. It rejects TypeScript-only runtime syntax: parameter properties
   (`constructor(private readonly x: T)`), enums and namespaces.

## Decision

Run TypeScript directly with `node --experimental-strip-types`. Imports carry explicit
`.ts` extensions (`allowImportingTsExtensions` in `tsconfig.json`, with `noEmit`), and
classes declare their fields explicitly instead of using parameter properties.

Type checking stays a separate step (`npm run typecheck`), and Vitest runs the same
sources without any of these constraints.

## Consequences

- No build step, no `dist/`, and the file you read is the file that runs.
- `tsc` is a type checker here, not a compiler. When the gateway ships in a container at
  M3, it needs a bundler (esbuild or tsup) rather than plain `tsc`, because `.ts`
  specifiers do not survive to a plain Node runtime without one.
- Enums are unavailable; union types of string literals are used instead, which the ledger
  event types already needed for canonical encoding.

## Revisit when

M3, when the gateway becomes a Docker image. At that point either adopt a bundler or move
to `.js` specifiers with a real `tsc` emit.
