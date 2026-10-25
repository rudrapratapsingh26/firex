import { useState } from 'react';

import { useApi, type Entry } from '../api.ts';
import { EntryTable } from './NodeDetail.tsx';

const FILTERS: [string, string][] = [
  ['', 'Everything'],
  ['state_change,silence', 'Alarms'],
  ['threshold_cross', 'Thresholds'],
  ['media_captured,trace_captured', 'Photos and traces'],
  ['fault,self_test,fan_state', 'Health'],
  ['calibration,config_change,boot', 'Setup and restarts'],
  ['fw_update,clock_sync', 'Firmware and clock'],
  ['log_overwrite', 'Lost entries'],
];

export function Timeline() {
  const [types, setTypes] = useState('');
  const [limit, setLimit] = useState(100);
  const timeline = useApi<Entry[]>(`/api/timeline?limit=${limit}${types ? `&types=${types}` : ''}`);
  const entries = timeline.data ?? [];

  return (
    <>
      <div className="page-head">
        <h1>Timeline</h1>
        <span className="small muted">Newest first, across every unit. Each entry is checked against its unit’s key as it arrives.</span>
      </div>
      <div className="segmented" role="group" aria-label="Show" style={{ marginBottom: 'var(--gap)', flexWrap: 'wrap' }}>
        {FILTERS.map(([value, text]) => (
          <button key={text} type="button" aria-pressed={types === value} onClick={() => setTypes(value)}>
            {text}
          </button>
        ))}
      </div>
      {timeline.error && <p className="error">{timeline.error}</p>}
      <EntryTable entries={entries} showUnit />
      {entries.length === limit && (
        <p style={{ marginTop: 'var(--gap)' }}>
          <button className="button secondary" type="button" onClick={() => setLimit((l) => Math.min(500, l + 100))}>
            Show older entries
          </button>
        </p>
      )}
    </>
  );
}
