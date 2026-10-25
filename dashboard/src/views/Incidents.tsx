import { useState } from 'react';

import { api, useApi, type Incident, type Me, type Registry } from '../api.ts';
import { Badge, EscalationPanel } from '../components.tsx';
import { atMs, atSeconds, duration, modalityList, shortHash } from '../format.ts';

const LABELS: [string, string][] = [
  ['true', 'Real hazard'],
  ['nuisance', 'Nuisance'],
  ['test', 'Test'],
];

export function Incidents({ me, registry }: { me: Me; registry: Registry | null }) {
  const incidents = useApi<Incident[]>('/api/incidents?limit=50');
  const list = incidents.data ?? [];

  return (
    <>
      <div className="page-head">
        <h1>Incidents</h1>
        <span className="small muted">Every critical alarm, with the photos the unit took as it went critical and who was alerted.</span>
      </div>
      {incidents.error && <p className="error">{incidents.error}</p>}
      {!incidents.data && !incidents.error && <p className="muted">Loading incidents</p>}
      {incidents.data && list.length === 0 && <p className="muted">No critical alarms have been recorded at this site.</p>}
      {list.map((incident) => (
        <IncidentCard key={incident.id} incident={incident} registry={registry} canAct={me.role !== 'viewer'} onChange={incidents.reload} />
      ))}
    </>
  );
}

function IncidentCard({ incident, registry, canAct, onChange }: { incident: Incident; registry: Registry | null; canAct: boolean; onChange: () => void }) {
  const [error, setError] = useState<string | null>(null);
  const open = incident.end_seq === null;
  const reason = modalityList(incident.reason);
  const minutes = incident.end_ts ? Math.max(1, Math.round((incident.end_ts - incident.start_ts) / 60)) : null;
  const traces = incident.traces ?? [];

  const label = async (value: string) => {
    try {
      await api(`/api/incidents/${incident.id}/label`, { method: 'POST', body: { label: value, note: incident.note ?? '' } });
      setError(null);
      onChange();
    } catch (err) {
      setError((err as Error).message);
    }
  };

  const acknowledge = async () => {
    await api(`/api/incidents/${incident.id}/ack`, { method: 'POST', body: {} });
    onChange();
  };

  return (
    <article className="incident" data-open={open} aria-labelledby={`incident-${incident.id}`}>
      <div className="incident-head">
        <h2 id={`incident-${incident.id}`} style={{ fontSize: 'var(--t-lead)' }}>
          <a href={`#/nodes/${incident.device_id}`}>{incident.device_label || `Unit ${incident.device_id.slice(0, 6)}`}</a>
          {reason ? `: ${reason}` : ''}
          {incident.late && (
            <>
              {' '}
              <Badge kind="late">Late</Badge>
            </>
          )}
        </h2>
        <span>
          {open ? `Critical since ${atSeconds(incident.start_ts)}` : `${atSeconds(incident.start_ts)}, ${minutes} min`}
        </span>
      </div>
      <div className="incident-body">
        {incident.frames.length === 0 ? (
          <p className="muted">The unit recorded no photos for this alarm.</p>
        ) : (
          <div className="photos">
            {incident.frames.map((frame, i) => (
              <figure className="photo" key={frame.sha256}>
                {frame.available ? (
                  <img src={`/api/media/${frame.sha256}`} alt={`Photo ${i + 1} taken as the alarm went critical`} loading="lazy" />
                ) : (
                  <div className="photo-missing">
                    {frame.deleted ? 'Deleted at the end of the retention period. Its hash stays in the record.' : 'Not received from the unit yet.'}
                  </div>
                )}
                <figcaption>
                  {frame.available ? 'Matches' : 'Recorded as'} <span className="mono">{shortHash(frame.sha256)}</span> in entry {frame.seq}
                </figcaption>
              </figure>
            ))}
          </div>
        )}

        <EscalationPanel
          alerts={incident.alerts ?? []}
          registry={registry}
          escalation={incident.escalation}
          late={incident.late ?? false}
          onAck={canAct ? acknowledge : undefined}
        />

        {traces.length > 0 && (
          <p className="small">
            Score traces from the unit:{' '}
            {traces.map((trace, i) => (
              <span key={trace.sha256}>
                {i > 0 && ', '}
                <a href={`/api/traces/${trace.sha256}`} download={`firex-trace-${trace.sha256.slice(0, 10)}.fxt`}>
                  trace {i + 1}, {duration((trace.samples * trace.period_ms) / 1000)}
                </a>
              </span>
            ))}
            . Replay them with the validation tools to see what the rules decided.
          </p>
        )}

        <div className="form-row">
          <span>Was this real?</span>
          <div className="segmented" role="group" aria-label="Label this incident">
            {LABELS.map(([value, text]) => (
              <button key={value} type="button" aria-pressed={incident.label === value} disabled={!canAct} onClick={() => label(value)}>
                {text}
              </button>
            ))}
          </div>
          {incident.labelled_by && (
            <span className="small muted">
              Labelled by {incident.labelled_by}, {atMs(incident.labelled_at)}
            </span>
          )}
        </div>
        {error && <p className="error" role="alert">{error}</p>}
      </div>
    </article>
  );
}
