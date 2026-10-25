import type { Registry } from '../api.ts';
import { Badge } from '../components.tsx';
import { CHANNEL_TEXT, duration, SERVICE_TEXT, shortHash } from '../format.ts';

/** Read-only: the registry is changed on the gateway, and each change goes into the site record. */
export function RegistryView({ registry, status, error }: { registry?: Registry; status: number | null; error: string | null }) {
  const head = (
    <div className="page-head">
      <h1>Who is alerted</h1>
      <span className="small muted">The contacts mapped to this site at installation.</span>
    </div>
  );

  if (status === 404) {
    return (
      <>
        {head}
        <p className="muted">Alerting is not set up on this gateway, so nobody outside this dashboard is told about an alarm.</p>
      </>
    );
  }
  if (error) {
    return (
      <>
        {head}
        <p className="error">{error}</p>
      </>
    );
  }
  if (!registry) {
    return (
      <>
        {head}
        <p className="muted">Loading the service registry</p>
      </>
    );
  }

  return (
    <>
      {head}
      <div className="columns">
        <section className="panel" aria-labelledby="rules-title">
          <h2 id="rules-title">Rules</h2>
          <div className="panel-body">
            <ul style={{ margin: 0, paddingLeft: '1.1rem' }}>
              <li>Only a critical alarm alerts these contacts. An elevated unit shows on this dashboard and nowhere else.</li>
              <li>A service in drill mode receives drills only, never a real incident, until an agreement with it is recorded.</li>
              <li>A contact that does not answer is alerted again, then the silence is raised to the site.</li>
            </ul>
          </div>
        </section>
        <section className="panel" aria-labelledby="timing-title">
          <h2 id="timing-title">{registry.site_name}</h2>
          <div className="panel-body">
            <dl className="facts">
              <dt>Answer within</dt>
              <dd>{duration(registry.ack_timeout_s)}</dd>
              <dt>Re-alerts</dt>
              <dd>{registry.max_realerts}</dd>
              <dt>Unit offline</dt>
              <dd>site told after {registry.offline_notify_min} min</dd>
              <dt>Registry</dt>
              <dd>
                version {registry.version}, <span className="mono" title={registry.sha256}>{shortHash(registry.sha256)}</span>
              </dd>
            </dl>
          </div>
        </section>
      </div>

      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th scope="col">Service</th>
              <th scope="col">Contact</th>
              <th scope="col">How</th>
              <th scope="col">Mode</th>
              <th scope="col">Agreement</th>
              <th scope="col">If there is no answer</th>
            </tr>
          </thead>
          <tbody>
            {registry.contacts.map((c) => (
              <tr key={c.service}>
                <td>
                  <strong>{SERVICE_TEXT[c.service] ?? c.service}</strong>
                </td>
                <td>{c.name}</td>
                <td>
                  {CHANNEL_TEXT[c.channel] ?? c.channel}
                  {c.recipient_masked && <span className="mono"> {c.recipient_masked}</span>}
                </td>
                <td>
                  <Badge kind={c.mode}>{c.mode === 'live' ? 'Live' : 'Drill mode'}</Badge>
                </td>
                <td>{c.agreement ? `${c.agreement.reference}, ${c.agreement.date}` : <span className="muted">none recorded</span>}</td>
                <td>
                  {c.backup ? (
                    <>
                      {c.backup.name}, {CHANNEL_TEXT[c.backup.channel] ?? c.backup.channel}
                      {c.backup.recipient_masked && <span className="mono"> {c.backup.recipient_masked}</span>}
                    </>
                  ) : (
                    <span className="muted">the same contact again</span>
                  )}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </>
  );
}
