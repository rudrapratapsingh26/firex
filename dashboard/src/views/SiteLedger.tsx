import { useMemo, useState } from 'react';

import { useApi, type Device, type SiteEntry } from '../api.ts';
import { atSeconds, describeSiteEntry, shortHash, TS_SRC_TEXT, unitName } from '../format.ts';

const PAGE = 50;

/** The gateway's own signed chain: alerts, answers, silences, gaps, labels, drills. */
export function SiteLedger({ devices }: { devices: Device[] }) {
  const [before, setBefore] = useState<number | null>(null);
  const page = useApi<SiteEntry[]>(`/api/site/ledger?limit=${PAGE}${before !== null ? `&before_seq=${before}` : ''}`);
  const names = useMemo(() => Object.fromEntries(devices.map((d) => [d.id, unitName(d)])), [devices]);
  const entries = page.data ?? [];

  const head = (
    <div className="page-head">
      <h1>Site record</h1>
      <span className="small muted">What the gateway itself witnessed, signed by the gateway: who was alerted, who answered, and when units went quiet.</span>
    </div>
  );

  if (page.status === 404) {
    return (
      <>
        {head}
        <p className="muted">This gateway does not keep a site record.</p>
      </>
    );
  }

  return (
    <>
      {head}
      <div className="panel" style={{ marginBottom: 'var(--gap)' }}>
        <div className="panel-body">
          <p>
            Each unit signs its own record; nobody else can add to it. Alerts, acknowledgements, drills and labels happen on the gateway, so the gateway keeps
            this second record in the same format, signed with its own key. Entries that concern an alarm name the unit’s entry by number and hash.
          </p>
          <div className="form-row" style={{ marginTop: '0.75rem' }}>
            <a className="button" href="/api/site/ledger/bundle" download="firex-site-record.json">
              Export the site record
            </a>
          </div>
          <pre className="command">npm run verify -- firex-site-record.json</pre>
        </div>
      </div>

      {page.error && <p className="error">{page.error}</p>}
      {!page.data && !page.error && <p className="muted">Loading the site record</p>}
      {page.data && entries.length === 0 && <p className="muted">Nothing has been written to the site record yet.</p>}
      {entries.length > 0 && (
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th scope="col">Time on gateway</th>
                <th scope="col">What happened</th>
                <th scope="col" className="num">Entry</th>
                <th scope="col">Hash</th>
                <th scope="col">Clock</th>
              </tr>
            </thead>
            <tbody>
              {entries.map((e) => (
                <tr key={e.seq}>
                  <td>{atSeconds(e.ts)}</td>
                  <td>{describeSiteEntry(e, names)}</td>
                  <td className="num">{e.seq}</td>
                  <td className="mono" title={e.hash}>
                    {shortHash(e.hash)}
                  </td>
                  <td className="small">{TS_SRC_TEXT[e.ts_src] ?? e.ts_src}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      <div className="form-row" style={{ marginTop: 'var(--gap)' }}>
        {before !== null && (
          <button className="button secondary" type="button" onClick={() => setBefore(null)}>
            Newest entries
          </button>
        )}
        {entries.length === PAGE && (
          <button className="button secondary" type="button" onClick={() => setBefore(entries[entries.length - 1].seq)}>
            Older entries
          </button>
        )}
      </div>
    </>
  );
}
