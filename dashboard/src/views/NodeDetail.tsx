import { useState } from 'react';

import { api, useApi, type DeviceDetail, type Entry, type Me, type TelemetryPoint } from '../api.ts';
import { Hex, Sparkline, VerifyMark } from '../components.tsx';
import {
  ago, atMs, atSeconds, CHAIN_TEXT, condition, CONDITION_TEXT, describeEntry, duration, fwUpdateText, MODALITY, shortHash, unitName,
} from '../format.ts';

const MODALITIES = ['gas', 'smoke', 'flame', 'heat', 'humidity'];

const CLOCK_TEXT: Record<string, string> = {
  gateway: 'Synchronised from the gateway',
  uncertain: 'Uncertain: not yet set from the gateway',
};

export function NodeDetail({ id, me }: { id: string; me: Me }) {
  const device = useApi<DeviceDetail>(`/api/devices/${id}`);
  const telemetry = useApi<TelemetryPoint[]>(`/api/devices/${id}/telemetry?minutes=60`);
  const entries = useApi<Entry[]>(`/api/devices/${id}/ledger?limit=15`);

  if (device.error) return <p className="error">{device.error}</p>;
  const d = device.data;
  if (!d) return <p className="muted">Loading unit</p>;
  const c = condition(d);
  const points = telemetry.data ?? [];
  const latest = points[points.length - 1]?.scores ?? {};
  const selfTest = d.last_self_test;
  const counts = d.verify_counts ?? {};
  const failed = (counts.chain ?? 0) + (counts.signature ?? 0);
  const gaps = [...(d.gaps ?? [])].sort((a, b) => b.start_ms - a.start_ms);
  const updates = [...(d.fw_updates ?? [])].sort((a, b) => b.seq - a.seq);
  const traces = [...(d.traces ?? [])].sort((a, b) => b.seq - a.seq);

  return (
    <>
      <div className="node-head">
        <Hex condition={c} size={64} />
        <div>
          <h1>{unitName(d)}</h1>
          <div className="node-state">{d.removed ? 'Removed' : CONDITION_TEXT[c]}</div>
          <p className="small muted">
            Last report {ago(d.last_seen)}
            {d.open_incident !== null && (
              <>
                , <a href="#/incidents">incident open</a>
              </>
            )}
          </p>
        </div>
      </div>

      {d.removed && (
        <p className="banner" role="status">
          This unit has been removed from the site and its certificate revoked. Its record, photos and traces stay on the gateway.
        </p>
      )}
      {!d.removed && d.offline_since && (
        <p className="banner" role="status">
          <strong>Not reporting since {atMs(d.offline_since)}</strong> ({ago(d.offline_since)}). The unit still alarms on its own; what it records reaches the
          gateway when it reconnects.
        </p>
      )}

      <div className="columns">
        <section className="panel" aria-labelledby="scores-title">
          <h2 id="scores-title">Hazard scores, last hour</h2>
          <div className="panel-body">
            {points.length === 0 ? (
              <p className="muted">No readings in the last hour.</p>
            ) : (
              MODALITIES.map((m) => {
                const values = points.map((p) => p.scores?.[m]).filter((v): v is number => typeof v === 'number');
                const now = latest[m];
                return (
                  <div className="score-row" key={m}>
                    <span>{MODALITY[m]}</span>
                    <Sparkline values={values} label={`${MODALITY[m]} score over the last hour`} enter={m === 'humidity' ? 75 : 60} exit={m === 'humidity' ? 75 : 40} />
                    <span className="score-value">{typeof now === 'number' ? now : '–'}</span>
                  </div>
                );
              })
            )}
            <p className="small muted" style={{ marginTop: '0.5rem' }}>
              Dashed lines mark where a signal starts and stops voting. Humidity never votes; above its line it damps the gas score.
            </p>
          </div>
        </section>

        <section className="panel" aria-labelledby="unit-title">
          <h2 id="unit-title">This unit</h2>
          <div className="panel-body">
            <dl className="facts">
              <dt>Firmware</dt>
              <dd>
                {d.fw_version ?? 'unknown'}
                {d.ota && (
                  <span className="small muted">
                    {' '}
                    in {d.ota.partition}
                    {d.ota.pending_verify ? ', not yet confirmed' : ''}
                  </span>
                )}
              </dd>
              <dt>Clock</dt>
              <dd>{d.clock ? CLOCK_TEXT[d.clock] ?? d.clock : 'unknown'}</dd>
              <dt>Boots</dt>
              <dd>{d.boot_count ?? 'unknown'}</dd>
              <dt>Added</dt>
              <dd>{atMs(d.provisioned_at)}</dd>
              <dt>Calibrated</dt>
              <dd>{d.last_calibration_ts ? atSeconds(d.last_calibration_ts) : 'no calibration received'}</dd>
              <dt>Self-test</dt>
              <dd>{selfTest ? `${selfTest.result === 'pass' ? 'Passed' : 'Failed'}, ${atSeconds(selfTest.ts)}` : 'none received'}</dd>
              <dt>Unit ID</dt>
              <dd className="mono">{d.id}</dd>
            </dl>
            {selfTest && (
              <ul className="checks" style={{ marginTop: '0.75rem' }}>
                {Object.entries(selfTest.checks ?? {}).map(([name, ok]) => (
                  <li key={name} data-ok={ok}>
                    {ok ? 'Pass' : 'Fail'}: {name.replace('_', ' ')}
                  </li>
                ))}
              </ul>
            )}
          </div>
        </section>

        <section className="panel" aria-labelledby="record-title">
          <h2 id="record-title">Record</h2>
          <div className="panel-body">
            <p>
              <strong>{CHAIN_TEXT[d.chain_status] ?? d.chain_status}</strong>
            </p>
            <p className="small muted">
              {d.last_seq === null ? 'No entries yet.' : `Entry ${d.last_seq} is the latest.`}{' '}
              {failed > 0 ? `${failed} ${failed === 1 ? 'entry fails' : 'entries fail'} verification.` : 'Every entry checks against this unit’s key.'}
            </p>
            {d.chain_events.length > 0 && (
              <ul className="small" style={{ margin: '0.75rem 0 0', paddingLeft: '1.1rem' }}>
                {d.chain_events.slice(0, 5).map((e) => (
                  <li key={e.id}>
                    {atMs(e.at)}: {e.detail || e.kind}
                    {e.from_seq !== null && ` (entries ${e.from_seq}${e.to_seq !== null && e.to_seq !== e.from_seq ? ` to ${e.to_seq}` : ''})`}
                  </li>
                ))}
              </ul>
            )}
            <div className="form-row" style={{ marginTop: '0.9rem' }}>
              <a className="button" href={`/api/devices/${d.id}/bundle`} download>
                Export log bundle
              </a>
              <a className="button secondary" href={`#/ledger/${d.id}`}>
                Open in ledger
              </a>
            </div>
          </div>
        </section>
      </div>

      <div className="columns">
        <section className="panel" aria-labelledby="gaps-title">
          <h2 id="gaps-title">Reporting gaps</h2>
          <div className="panel-body">
            {gaps.length === 0 ? (
              <p className="muted">No gaps in reporting recorded.</p>
            ) : (
              <table className="compact">
                <thead>
                  <tr>
                    <th scope="col">Went quiet</th>
                    <th scope="col">Back</th>
                    <th scope="col" className="num">Length</th>
                  </tr>
                </thead>
                <tbody>
                  {gaps.slice(0, 10).map((gap) => (
                    <tr key={gap.start_ms}>
                      <td>{atMs(gap.start_ms)}</td>
                      <td>{gap.end_ms ? atMs(gap.end_ms) : <strong>still offline</strong>}</td>
                      <td className="num">{duration(gap.end_ms ? gap.seconds : (Date.now() - gap.start_ms) / 1000)}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            )}
            {gaps.length > 10 && <p className="small muted" style={{ marginTop: '0.5rem' }}>The 10 most recent of {gaps.length}; every gap is in the site record.</p>}
          </div>
        </section>

        <section className="panel" aria-labelledby="fw-title">
          <h2 id="fw-title">Firmware history</h2>
          <div className="panel-body">
            {updates.length === 0 ? (
              <p className="muted">No firmware updates recorded.</p>
            ) : (
              <ul className="history">
                {updates.slice(0, 10).map((u) => (
                  <li key={u.seq} data-bad={u.phase === 'failed' || u.phase === 'rolled_back' ? 'true' : undefined}>
                    <span>{fwUpdateText(u)}</span>
                    <span className="small muted">
                      {atSeconds(u.ts)}, entry {u.seq}
                    </span>
                  </li>
                ))}
              </ul>
            )}
          </div>
        </section>

        <section className="panel" aria-labelledby="traces-title">
          <h2 id="traces-title">Score traces</h2>
          <div className="panel-body">
            {traces.length === 0 ? (
              <p className="muted">No traces received. The unit records one around every elevated or critical episode.</p>
            ) : (
              <ul className="history">
                {traces.slice(0, 10).map((t) => (
                  <li key={t.sha256}>
                    <a href={`/api/traces/${t.sha256}`} download={`firex-trace-${t.sha256.slice(0, 10)}.fxt`}>
                      {duration((t.samples * t.period_ms) / 1000)} around the change to {t.trigger.toLowerCase()}
                    </a>
                    <span className="small muted">
                      received {atMs(t.received_at)}, entry {t.seq}, <span className="mono">{shortHash(t.sha256)}</span>
                    </span>
                  </li>
                ))}
              </ul>
            )}
          </div>
        </section>
      </div>

      <section aria-labelledby="entries-title">
        <div className="page-head">
          <h2 id="entries-title">Latest entries</h2>
          {me.role === 'viewer' ? null : <span className="small muted">Entries are written and signed by the unit; nobody can edit them here.</span>}
        </div>
        <EntryTable entries={entries.data ?? []} showUnit={false} />
      </section>

      {me.role === 'admin' && !d.removed && <RemoveUnit device={d} />}
    </>
  );
}

function RemoveUnit({ device }: { device: DeviceDetail }) {
  const [typed, setTyped] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const expected = device.label || device.id;
  const answer = typed.trim();
  const matches = answer !== '' && (answer === device.label || answer === device.id);

  const remove = async () => {
    setBusy(true);
    try {
      await api(`/api/devices/${device.id}`, { method: 'DELETE' });
      location.hash = '#/plan';
    } catch (err) {
      setError((err as Error).message);
      setBusy(false);
    }
  };

  return (
    <section className="panel danger" aria-labelledby="remove-title" style={{ marginTop: 'calc(var(--gap) * 2)' }}>
      <h2 id="remove-title">Remove this unit</h2>
      <div className="panel-body form">
        <p>
          Removing a unit revokes its certificate. It can no longer connect to this gateway, and the only way back is to pair it again. This cannot be undone.
          Its record, photos and traces stay on the gateway, and the removal is written to the site record.
        </p>
        <label className="field">
          <span>
            Type <span className="mono">{expected}</span> to confirm
          </span>
          <input value={typed} onChange={(e) => setTyped(e.target.value)} autoComplete="off" spellCheck={false} />
        </label>
        {error && <p className="error" role="alert">{error}</p>}
        <div>
          <button className="button danger" type="button" disabled={!matches || busy} onClick={remove}>
            Remove unit and revoke its certificate
          </button>
        </div>
      </div>
    </section>
  );
}

export function EntryTable({ entries, showUnit }: { entries: Entry[]; showUnit: boolean }) {
  if (entries.length === 0) return <p className="muted">No entries received yet.</p>;
  return (
    <div className="table-wrap">
      <table>
        <thead>
          <tr>
            <th scope="col">Time on unit</th>
            {showUnit && <th scope="col">Unit</th>}
            <th scope="col">What happened</th>
            <th scope="col">Entry</th>
            <th scope="col">Hash</th>
            <th scope="col">Check</th>
          </tr>
        </thead>
        <tbody>
          {entries.map((e) => (
            <tr key={`${e.device_id}-${e.epoch}-${e.seq}`}>
              <td>{atSeconds(e.ts)}</td>
              {showUnit && (
                <td>
                  <a href={`#/nodes/${e.device_id}`}>{e.label || e.device_id.slice(0, 6)}</a>
                </td>
              )}
              <td>{describeEntry(e)}</td>
              <td className="num">{e.seq}</td>
              <td className="mono" title={e.hash}>
                {shortHash(e.hash)}
              </td>
              <td>
                <VerifyMark result={e.verify_result} />
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
