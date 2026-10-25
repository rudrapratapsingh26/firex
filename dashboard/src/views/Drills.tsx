import { useState, type FormEvent } from 'react';

import { api, useApi, type Drill, type Me, type Registry } from '../api.ts';
import { Badge, EscalationPanel } from '../components.tsx';
import { atMs, CHANNEL_TEXT, SERVICE_TEXT, SERVICES } from '../format.ts';

export function Drills({ me, registry }: { me: Me; registry: Registry | null }) {
  const drills = useApi<Drill[]>('/api/drills');
  const [chosen, setChosen] = useState<string[]>([...SERVICES]);
  const [busy, setBusy] = useState(false);
  const [started, setStarted] = useState<number | null>(null);
  const [error, setError] = useState<string | null>(null);
  const isAdmin = me.role === 'admin';
  const list = [...(drills.data ?? [])].sort((a, b) => b.started_at_ms - a.started_at_ms);

  const head = (
    <div className="page-head">
      <h1>Drills</h1>
      <span className="small muted">Practice alerts, marked DRILL in every message, and timed as for a real alarm.</span>
    </div>
  );

  if (drills.status === 404) {
    return (
      <>
        {head}
        <p className="muted">Drills need alerting set up on this gateway: a service registry and an alert provider.</p>
      </>
    );
  }

  const toggle = (service: string) =>
    setChosen((current) => (current.includes(service) ? current.filter((s) => s !== service) : SERVICES.filter((s) => s === service || current.includes(s))));

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    setBusy(true);
    try {
      const result = await api<{ id: number }>('/api/drills', { method: 'POST', body: { services: chosen } });
      setStarted(result.id);
      setError(null);
      drills.reload();
    } catch (err) {
      setError((err as Error).message);
      setStarted(null);
    } finally {
      setBusy(false);
    }
  };

  return (
    <>
      {head}
      {isAdmin ? (
        <form className="panel" onSubmit={submit} style={{ marginBottom: 'var(--gap)' }}>
          <h2>Start a drill</h2>
          <div className="panel-body form">
            <fieldset className="checkbox-list">
              <legend>Who takes part</legend>
              {SERVICES.map((service) => (
                <label key={service}>
                  <input type="checkbox" checked={chosen.includes(service)} onChange={() => toggle(service)} />
                  {SERVICE_TEXT[service]}
                </label>
              ))}
            </fieldset>
            {chosen.length > 0 ? (
              <div>
                <p>These contacts will receive a message marked DRILL and be asked to acknowledge it:</p>
                <ul className="recipients">
                  {chosen.map((service) => {
                    const contact = registry?.contacts.find((c) => c.service === service);
                    return (
                      <li key={service}>
                        <strong>{SERVICE_TEXT[service]}</strong>:{' '}
                        {contact
                          ? `${contact.name}, ${CHANNEL_TEXT[contact.channel] ?? contact.channel}${contact.recipient_masked ? ` ${contact.recipient_masked}` : ''}`
                          : 'as named in the service registry'}
                      </li>
                    );
                  })}
                </ul>
              </div>
            ) : (
              <p className="muted">Choose at least one service.</p>
            )}
            <p className="small muted">
              A service in drill mode receives drills and nothing else until an agreement is recorded. Each answer, and each silence, goes into the site record.
            </p>
            {error && <p className="error" role="alert">{error}</p>}
            {started !== null && <p role="status">Drill {started} started. Its messages appear below as they go out.</p>}
            <div>
              <button className="button" type="submit" disabled={busy || chosen.length === 0}>
                Send drill messages
              </button>
            </div>
          </div>
        </form>
      ) : (
        <p className="small muted" style={{ marginBottom: 'var(--gap)' }}>
          An administrator can start a drill.
        </p>
      )}

      {drills.error && <p className="error">{drills.error}</p>}
      {!drills.data && !drills.error && <p className="muted">Loading drills</p>}
      {drills.data && list.length === 0 && <p className="muted">No drills have been run at this site.</p>}
      {list.map((drill) => (
        <article className="incident" data-drill="true" key={drill.id} aria-labelledby={`drill-${drill.id}`}>
          <div className="incident-head">
            <h2 id={`drill-${drill.id}`} style={{ fontSize: 'var(--t-lead)' }}>
              <Badge kind="drill">Drill</Badge> Drill {drill.id}
            </h2>
            <span>
              {drill.finished_at_ms ? `${atMs(drill.started_at_ms)} to ${atMs(drill.finished_at_ms)}` : `Running since ${atMs(drill.started_at_ms)}`}, started by{' '}
              {drill.by}
            </span>
          </div>
          <div className="incident-body">
            <EscalationPanel
              alerts={drill.alerts ?? []}
              registry={registry}
              drill
              include={drill.services}
              escalation={{ state: drill.finished_at_ms ? 'finished' : 'running' }}
            />
          </div>
        </article>
      ))}
    </>
  );
}
