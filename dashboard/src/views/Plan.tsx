import { useRef, useState, type PointerEvent as ReactPointerEvent } from 'react';

import { api, type Device, type Me, type Site } from '../api.ts';
import { Hex } from '../components.tsx';
import { ago, atMs, CONDITION_TEXT, condition, SEVERITY, unitName, type Condition } from '../format.ts';

const LEGEND: Condition[] = ['critical', 'elevated', 'fault', 'offline', 'warmup', 'safe'];

export function Plan({ me, devices, site, reloadSite }: { me: Me; devices: Device[]; site?: Site; reloadSite: () => void }) {
  const [editing, setEditing] = useState(false);
  const [dragging, setDragging] = useState<{ id: string; x: number; y: number } | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [planVersion, setPlanVersion] = useState(0);
  const planRef = useRef<HTMLDivElement>(null);
  const fileRef = useRef<HTMLInputElement>(null);
  const canEdit = me.role !== 'viewer';

  const placed = devices.filter((d) => d.floor_x !== null && d.floor_y !== null);
  const unplaced = devices.filter((d) => d.floor_x === null || d.floor_y === null);
  const attention = devices
    .filter((d) => condition(d) !== 'safe')
    .sort((a, b) => SEVERITY.indexOf(condition(a)) - SEVERITY.indexOf(condition(b)));

  const positionFor = (event: { clientX: number; clientY: number }) => {
    const rect = planRef.current!.getBoundingClientRect();
    return {
      x: Math.min(1, Math.max(0, (event.clientX - rect.left) / rect.width)),
      y: Math.min(1, Math.max(0, (event.clientY - rect.top) / rect.height)),
    };
  };

  const startDrag = (device: Device, event: ReactPointerEvent) => {
    if (!editing) return;
    event.preventDefault();
    (event.target as Element).setPointerCapture?.(event.pointerId);
    setDragging({ id: device.id, ...positionFor(event) });
  };

  const drop = async () => {
    if (!dragging) return;
    const { id, x, y } = dragging;
    try {
      await api(`/api/devices/${id}`, { method: 'PATCH', body: { floor_x: Number(x.toFixed(4)), floor_y: Number(y.toFixed(4)) } });
      setError(null);
    } catch (err) {
      setError((err as Error).message);
    }
    setDragging(null);
  };

  const uploadPlan = async (file: File | undefined) => {
    if (!file) return;
    try {
      await api('/api/site/floor-plan', { method: 'PUT', raw: file });
      setPlanVersion((v) => v + 1);
      reloadSite();
      setError(null);
    } catch (err) {
      setError((err as Error).message);
    }
  };

  const counts = (c: Condition) => devices.filter((d) => {
    const dc = condition(d);
    return dc === c || (c === 'critical' && dc === 'silenced') || (c === 'offline' && dc === 'unknown') || (c === 'warmup' && dc === 'selftest');
  }).length;

  return (
    <>
      <div className="page-head">
        <h1>Site plan</h1>
        {canEdit && (
          <div className="form-row">
            <button className="button secondary" type="button" onClick={() => fileRef.current?.click()}>
              {site?.has_floor_plan ? 'Replace floor plan' : 'Upload floor plan'}
            </button>
            <input ref={fileRef} type="file" accept="image/png,image/jpeg,image/webp,image/svg+xml" hidden onChange={(e) => uploadPlan(e.target.files?.[0])} />
            <button className="button secondary" type="button" aria-pressed={editing} onClick={() => setEditing((v) => !v)}>
              {editing ? 'Done placing units' : 'Place units'}
            </button>
          </div>
        )}
      </div>
      {error && <p className="error" role="alert">{error}</p>}

      <div className="plan-layout">
        <div>
          <div
            className="plan"
            ref={planRef}
            data-editing={editing}
            onPointerMove={(e) => dragging && setDragging({ ...dragging, ...positionFor(e) })}
            onPointerUp={drop}
          >
            {site?.has_floor_plan && <img src={`/api/site/floor-plan?v=${planVersion}`} alt="Floor plan of the building" />}
            {!site?.has_floor_plan && placed.length === 0 && (
              <div className="plan-empty">
                <h2>No floor plan yet</h2>
                <p className="muted">
                  {canEdit ? 'Upload a drawing of this floor, then place each unit where it is mounted.' : 'An operator can upload the floor plan for this building.'}
                </p>
              </div>
            )}
            {placed.map((d) => {
              const live = dragging?.id === d.id ? dragging : { x: d.floor_x!, y: d.floor_y! };
              const c = condition(d);
              const Tag = editing ? 'button' : 'a';
              return (
                <Tag
                  key={d.id}
                  className="plan-node"
                  style={{ left: `${live.x * 100}%`, top: `${live.y * 100}%` }}
                  {...(editing ? { type: 'button' as const, onPointerDown: (e: ReactPointerEvent) => startDrag(d, e) } : { href: `#/nodes/${d.id}` })}
                  aria-label={`${unitName(d)}: ${CONDITION_TEXT[c]}${d.offline_since ? `, not reporting since ${atMs(d.offline_since)}` : ''}`}
                >
                  <Hex condition={c} size={c === 'critical' || c === 'silenced' ? 44 : 34} />
                  <span className="name">{unitName(d)}</span>
                </Tag>
              );
            })}
          </div>
          {unplaced.length > 0 && (
            <div className="unplaced">
              <span className="muted">Not on the plan yet:</span>
              {unplaced.map((d) => (
                <button
                  key={d.id}
                  type="button"
                  className="link-button"
                  disabled={!editing}
                  onClick={async () => {
                    await api(`/api/devices/${d.id}`, { method: 'PATCH', body: { floor_x: 0.5, floor_y: 0.5 } });
                  }}
                  title={editing ? 'Put it in the middle of the plan, then drag it into place' : 'Choose Place units first'}
                >
                  {unitName(d)}
                </button>
              ))}
            </div>
          )}
        </div>

        <aside className="aside">
          <section className="panel" aria-labelledby="attention-title">
            <h2 id="attention-title">Needs attention</h2>
            {attention.length === 0 ? (
              <p className="panel-body muted">Every unit is reporting and safe.</p>
            ) : (
              <ul className="attention">
                {attention.map((d) => (
                  <li key={d.id}>
                    <a href={`#/nodes/${d.id}`}>
                      <Hex condition={condition(d)} size={24} />
                      <span>
                        <strong>{unitName(d)}</strong>
                        <br />
                        <span className="small">
                          {d.offline_since
                            ? `Not reporting since ${atMs(d.offline_since)}`
                            : `${CONDITION_TEXT[condition(d)]}, last report ${ago(d.last_seen)}`}
                        </span>
                      </span>
                    </a>
                  </li>
                ))}
              </ul>
            )}
          </section>

          <section className="panel" aria-labelledby="legend-title">
            <h2 id="legend-title">Legend</h2>
            <ul className="legend">
              {LEGEND.map((c) => (
                <li key={c}>
                  <Hex condition={c} size={20} />
                  <span>{CONDITION_TEXT[c]}</span>
                  <span className="small muted">{counts(c)}</span>
                </li>
              ))}
            </ul>
          </section>
        </aside>
      </div>
    </>
  );
}
