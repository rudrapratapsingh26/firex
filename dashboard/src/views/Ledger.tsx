import { useState } from 'react';

import { useApi, type Device, type DeviceDetail, type Entry } from '../api.ts';
import { CHAIN_TEXT, unitName } from '../format.ts';
import { EntryTable } from './NodeDetail.tsx';

export function Ledger({ selected, devices }: { selected: string | null; devices: Device[] }) {
  const current = selected ?? devices[0]?.id ?? null;

  return (
    <>
      <div className="page-head">
        <h1>Ledger</h1>
        <span className="small muted">What each unit signed, as the gateway received and checked it.</span>
      </div>
      {devices.length === 0 ? (
        <p className="muted">No units have sent entries yet.</p>
      ) : (
        <>
          <div className="table-wrap" style={{ marginBottom: 'var(--gap)' }}>
            <table>
              <thead>
                <tr>
                  <th scope="col">Unit</th>
                  <th scope="col">Record</th>
                  <th scope="col" className="num">Latest entry</th>
                  <th scope="col" className="num">Record restarts</th>
                </tr>
              </thead>
              <tbody>
                {devices.map((d) => (
                  <tr key={d.id} aria-current={d.id === current ? 'true' : undefined}>
                    <td>
                      <a href={`#/ledger/${d.id}`}>{unitName(d)}</a>
                      {d.removed && <span className="small muted"> (removed)</span>}
                    </td>
                    <td style={{ color: d.chain_status === 'broken' ? 'var(--fire)' : undefined, fontWeight: d.chain_status === 'broken' ? 700 : undefined }}>
                      {CHAIN_TEXT[d.chain_status] ?? d.chain_status}
                    </td>
                    <td className="num">{d.last_seq ?? '–'}</td>
                    <td className="num">{d.epoch}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          {current && <UnitLedger id={current} />}
        </>
      )}
    </>
  );
}

function UnitLedger({ id }: { id: string }) {
  const device = useApi<DeviceDetail>(`/api/devices/${id}`);
  const [before, setBefore] = useState<number | null>(null);
  const page = useApi<Entry[]>(`/api/devices/${id}/ledger?limit=50${before !== null ? `&before_seq=${before}` : ''}`);
  const entries = page.data ?? [];
  const d = device.data;
  const file = `firex-${id.slice(0, 8)}-epoch${d?.epoch ?? 0}.json`;

  return (
    <section aria-labelledby="unit-ledger-title">
      <div className="page-head">
        <h2 id="unit-ledger-title">{d ? unitName(d) : 'Unit'}</h2>
        <a className="button" href={`/api/devices/${id}/bundle`} download={file}>
          Export log bundle
        </a>
      </div>
      <div className="panel" style={{ marginBottom: 'var(--gap)' }}>
        <div className="panel-body">
          <p>
            An inspector can check the export without this gateway or FireX. Save the bundle and the incident photos into one folder, then run the
            verifier from the FireX repository:
          </p>
          <pre className="command">npm run verify -- {file} --media photos</pre>
          <p className="small muted" style={{ marginTop: '0.5rem' }}>
            It answers PASS with the time range covered, or TAMPERED with the first entry that fails and why.
          </p>
        </div>
      </div>
      {page.error && <p className="error">{page.error}</p>}
      <EntryTable entries={entries} showUnit={false} />
      <div className="form-row" style={{ marginTop: 'var(--gap)' }}>
        {before !== null && (
          <button className="button secondary" type="button" onClick={() => setBefore(null)}>
            Newest entries
          </button>
        )}
        {entries.length === 50 && (
          <button className="button secondary" type="button" onClick={() => setBefore(entries[entries.length - 1].seq)}>
            Older entries
          </button>
        )}
      </div>
    </section>
  );
}
