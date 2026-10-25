import { useRef, useState, type FormEvent } from 'react';

import { api, useApi, type Device, type FirmwareImage, type Me, type Rollout } from '../api.ts';
import { atMs, bytes, ROLLOUT_STATE_TEXT, ROLLOUT_UNIT_TEXT, shortHash, SIGNATURE_TEXT, unitName } from '../format.ts';

/** Images a unit would refuse: rolling them out only produces failures. */
const BLOCKED: Record<string, string> = {
  missing: 'Cannot be rolled out: the image is not signed, and every unit would refuse it.',
  wrong_key: 'Cannot be rolled out: it is signed with a key these units do not trust.',
};

export function Firmware({ me, devices }: { me: Me; devices: Device[] }) {
  const images = useApi<FirmwareImage[]>('/api/firmware');
  const rollouts = useApi<Rollout[]>('/api/rollouts');
  const isAdmin = me.role === 'admin';
  const fileRef = useRef<HTMLInputElement>(null);
  const [file, setFile] = useState<File | null>(null);
  const [uploading, setUploading] = useState(false);
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [planFor, setPlanFor] = useState<FirmwareImage | null>(null);

  const head = (
    <div className="page-head">
      <h1>Firmware</h1>
      <span className="small muted">Signed updates for the units, installed one unit at a time.</span>
    </div>
  );

  if (images.status === 404) {
    return (
      <>
        {head}
        <p className="muted">Firmware updates are not available on this gateway.</p>
      </>
    );
  }

  const upload = async (event: FormEvent) => {
    event.preventDefault();
    if (!file) return;
    setUploading(true);
    try {
      const result = await api<{ sha256: string; version: string; signature: string }>('/api/firmware', {
        method: 'POST',
        raw: file,
        contentType: 'application/octet-stream',
      });
      setMessage(`Added firmware ${result.version}: ${(SIGNATURE_TEXT[result.signature] ?? result.signature).toLowerCase()}.`);
      setError(null);
      setFile(null);
      if (fileRef.current) fileRef.current.value = '';
      images.reload();
    } catch (err) {
      setError((err as Error).message);
      setMessage(null);
    } finally {
      setUploading(false);
    }
  };

  const list = [...(images.data ?? [])].sort((a, b) => b.uploaded_at - a.uploaded_at);
  const runs = [...(rollouts.data ?? [])].sort((a, b) => b.started_at - a.started_at);

  return (
    <>
      {head}
      <div className="panel" style={{ marginBottom: 'var(--gap)' }}>
        <div className="panel-body">
          <p>
            A unit installs an update only while it is safe, restarts when it is safe, and must confirm the new firmware within 60 seconds of starting it. If it
            cannot, it returns to its previous firmware by itself, and the rollout stops before any other unit is touched.
          </p>
        </div>
      </div>

      {isAdmin && (
        <form className="panel" onSubmit={upload} style={{ marginBottom: 'var(--gap)' }}>
          <h2>Add an image</h2>
          <div className="panel-body form">
            <label className="field">
              Signed firmware image (.bin)
              <input ref={fileRef} type="file" accept=".bin,application/octet-stream" onChange={(e) => setFile(e.target.files?.[0] ?? null)} />
            </label>
            {error && <p className="error" role="alert">{error}</p>}
            {message && <p role="status">{message}</p>}
            <div>
              <button className="button" type="submit" disabled={!file || uploading}>
                {uploading ? 'Uploading' : 'Upload image'}
              </button>
            </div>
          </div>
        </form>
      )}

      <h2 style={{ marginBottom: '0.75rem' }}>Images</h2>
      {images.error && <p className="error">{images.error}</p>}
      {!images.data && !images.error && <p className="muted">Loading images</p>}
      {images.data && list.length === 0 && <p className="muted">No firmware images have been added to this gateway.</p>}
      {list.length > 0 && (
        <div className="table-wrap" style={{ marginBottom: 'var(--gap)' }}>
          <table>
            <thead>
              <tr>
                <th scope="col">Version</th>
                <th scope="col">Image</th>
                <th scope="col" className="num">Size</th>
                <th scope="col">Signature</th>
                <th scope="col">Added</th>
                {isAdmin && <th scope="col">Roll out</th>}
              </tr>
            </thead>
            <tbody>
              {list.map((image) => {
                const blocked = BLOCKED[image.signature];
                return (
                  <tr key={image.sha256}>
                    <td>
                      <strong>{image.version}</strong>
                      <br />
                      <span className="small muted">{image.project}</span>
                    </td>
                    <td className="mono" title={image.sha256}>
                      {shortHash(image.sha256)}
                    </td>
                    <td className="num">{bytes(image.size)}</td>
                    <td>
                      <span className="sig" data-ok={!blocked}>
                        {SIGNATURE_TEXT[image.signature] ?? image.signature}
                      </span>
                    </td>
                    <td>
                      {atMs(image.uploaded_at)}
                      <br />
                      <span className="small muted">by {image.uploaded_by}</span>
                    </td>
                    {isAdmin && (
                      <td>
                        <button
                          className="button secondary"
                          type="button"
                          disabled={Boolean(blocked)}
                          aria-describedby={blocked ? `blocked-${image.sha256}` : undefined}
                          onClick={() => setPlanFor(image)}
                        >
                          Roll out
                        </button>
                        {blocked && (
                          <p className="small muted" id={`blocked-${image.sha256}`} style={{ marginTop: '0.3rem' }}>
                            {blocked}
                          </p>
                        )}
                      </td>
                    )}
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}

      {isAdmin && planFor && (
        <RolloutPlan
          key={planFor.sha256}
          image={planFor}
          devices={devices}
          onCancel={() => setPlanFor(null)}
          onStarted={() => {
            setPlanFor(null);
            rollouts.reload();
          }}
        />
      )}

      <h2 style={{ marginBottom: '0.75rem' }}>Rollouts</h2>
      {rollouts.error && rollouts.status !== 404 && <p className="error">{rollouts.error}</p>}
      {rollouts.data && runs.length === 0 && <p className="muted">No firmware has been rolled out from this gateway.</p>}
      {runs.map((rollout) => (
        <RolloutCard key={rollout.id} rollout={rollout} isAdmin={isAdmin} onChange={rollouts.reload} />
      ))}
    </>
  );
}

function RolloutPlan({ image, devices, onCancel, onStarted }: { image: FirmwareImage; devices: Device[]; onCancel: () => void; onStarted: () => void }) {
  const [mode, setMode] = useState<'all' | 'some'>('all');
  const [chosen, setChosen] = useState<string[]>([]);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const already = devices.filter((d) => d.fw_version === image.version).length;

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    setBusy(true);
    try {
      await api(`/api/firmware/${image.sha256}/rollout`, { method: 'POST', body: { units: mode === 'all' ? 'all' : chosen } });
      setError(null);
      onStarted();
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setBusy(false);
    }
  };

  return (
    <form className="panel" onSubmit={submit} style={{ marginBottom: 'var(--gap)' }} aria-labelledby="plan-title">
      <h2 id="plan-title">Roll out firmware {image.version}</h2>
      <div className="panel-body form">
        <fieldset className="checkbox-list">
          <legend>Which units</legend>
          <label>
            <input type="radio" name="units" checked={mode === 'all'} onChange={() => setMode('all')} />
            Every unit at this site ({devices.length})
          </label>
          <label>
            <input type="radio" name="units" checked={mode === 'some'} onChange={() => setMode('some')} />
            Only the units I choose
          </label>
        </fieldset>
        {mode === 'some' && (
          <fieldset className="checkbox-list">
            <legend>Units</legend>
            {devices.length === 0 && <p className="muted">No units at this site.</p>}
            {devices.map((d) => (
              <label key={d.id}>
                <input
                  type="checkbox"
                  checked={chosen.includes(d.id)}
                  onChange={() => setChosen((current) => (current.includes(d.id) ? current.filter((x) => x !== d.id) : [...current, d.id]))}
                />
                {unitName(d)} <span className="small muted">firmware {d.fw_version ?? 'unknown'}</span>
              </label>
            ))}
          </fieldset>
        )}
        {already > 0 && <p className="small muted">{already === 1 ? 'One unit already runs' : `${already} units already run`} this version and will be skipped.</p>}
        {error && <p className="error" role="alert">{error}</p>}
        <div className="form-row">
          <button className="button" type="submit" disabled={busy || (mode === 'some' && chosen.length === 0)}>
            Start rollout
          </button>
          <button className="button secondary" type="button" onClick={onCancel}>
            Cancel
          </button>
        </div>
      </div>
    </form>
  );
}

function RolloutCard({ rollout, isAdmin, onChange }: { rollout: Rollout; isAdmin: boolean; onChange: () => void }) {
  const [error, setError] = useState<string | null>(null);
  const confirmed = rollout.units.filter((u) => u.state === 'confirmed').length;

  const stop = async () => {
    try {
      await api(`/api/rollouts/${rollout.id}/stop`, { method: 'POST', body: {} });
      setError(null);
      onChange();
    } catch (err) {
      setError((err as Error).message);
    }
  };

  return (
    <section className="panel" style={{ marginBottom: 'var(--gap)' }} aria-labelledby={`rollout-${rollout.id}`}>
      <h2 id={`rollout-${rollout.id}`}>
        Firmware {rollout.version}: {ROLLOUT_STATE_TEXT[rollout.state] ?? rollout.state}
      </h2>
      <div className="panel-body">
        <div className="form-row" style={{ justifyContent: 'space-between', alignItems: 'baseline', marginBottom: '0.6rem' }}>
          <span className="small muted">
            Started {atMs(rollout.started_at)} by {rollout.by}. {confirmed} of {rollout.units.length} confirmed.
          </span>
          {isAdmin && rollout.state === 'running' && (
            <button className="button secondary" type="button" onClick={stop}>
              Stop rollout
            </button>
          )}
        </div>
        {error && <p className="error" role="alert">{error}</p>}
        <table className="compact">
          <thead>
            <tr>
              <th scope="col">Unit</th>
              <th scope="col">Progress</th>
              <th scope="col">Detail</th>
            </tr>
          </thead>
          <tbody>
            {rollout.units.map((unit) => (
              <tr key={unit.id}>
                <td>
                  <a href={`#/nodes/${unit.id}`}>{unit.label || `Unit ${unit.id.slice(0, 6)}`}</a>
                </td>
                <td>
                  <span className="rollout-state" data-state={unit.state}>
                    {ROLLOUT_UNIT_TEXT[unit.state] ?? unit.state}
                  </span>
                </td>
                <td className="small">{unit.detail || '–'}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}
