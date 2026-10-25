import { useState, type FormEvent } from 'react';

import { api, useApi, type Me, type Pairing as PairingRow } from '../api.ts';
import { atMs } from '../format.ts';

export function Pairing({ me }: { me: Me }) {
  const pairings = useApi<PairingRow[]>(me.role === 'viewer' ? null : '/api/pairings');
  const [code, setCode] = useState('');
  const [label, setLabel] = useState('');
  const [added, setAdded] = useState<PairingRow | null>(null);
  const [error, setError] = useState<string | null>(null);

  if (me.role === 'viewer') return <p className="muted">Adding units needs the operator role.</p>;

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    try {
      const result = await api<PairingRow>('/api/pairings', { method: 'POST', body: { code, label } });
      setAdded(result);
      setCode('');
      setLabel('');
      setError(null);
      pairings.reload();
    } catch (err) {
      setError((err as Error).message);
    }
  };

  const now = Date.now();
  return (
    <>
      <div className="page-head">
        <h1>Add a unit</h1>
      </div>
      <div className="columns">
        <form className="panel" onSubmit={submit}>
          <h2>Pairing code</h2>
          <div className="panel-body form">
            <p>
              Enter the ten-character code printed on the unit’s label and where it is mounted. Then power the unit on the site Wi-Fi; it joins within a
              minute. A code works once, for 24 hours.
            </p>
            <label className="field">
              Code from the label
              <input
                value={code}
                onChange={(e) => setCode(e.target.value.toUpperCase())}
                placeholder="7K3QD-9M2XA"
                autoComplete="off"
                spellCheck={false}
                className="mono"
                required
              />
            </label>
            <label className="field">
              Where it is mounted
              <input value={label} onChange={(e) => setLabel(e.target.value)} placeholder="Block B, room 206" maxLength={64} required />
            </label>
            {error && <p className="error" role="alert">{error}</p>}
            {added && (
              <p role="status">
                Code <span className="mono">{added.code}</span> is ready for {added.label}. Power the unit on now.
              </p>
            )}
            <div>
              <button className="button" type="submit">
                Add unit
              </button>
            </div>
          </div>
        </form>
      </div>

      <h2 style={{ marginBottom: '0.75rem' }}>Codes entered</h2>
      {(pairings.data ?? []).length === 0 ? (
        <p className="muted">No codes have been entered yet.</p>
      ) : (
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th scope="col">Code</th>
                <th scope="col">Location</th>
                <th scope="col">Status</th>
                <th scope="col">Entered by</th>
              </tr>
            </thead>
            <tbody>
              {(pairings.data ?? []).map((p) => (
                <tr key={p.code}>
                  <td className="mono">{`${p.code.slice(0, 5)}-${p.code.slice(5)}`}</td>
                  <td>{p.label || '–'}</td>
                  <td>
                    {p.used_at ? (
                      <>
                        Joined {atMs(p.used_at)}, <a href={`#/nodes/${p.device_id}`}>open unit</a>
                      </>
                    ) : p.expires_at < now ? (
                      'Expired unused'
                    ) : (
                      `Waiting for the unit, until ${atMs(p.expires_at)}`
                    )}
                  </td>
                  <td>{p.created_by}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </>
  );
}
