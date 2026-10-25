import '@fontsource/barlow/400.css';
import '@fontsource/barlow/500.css';
import '@fontsource/barlow/600.css';
import '@fontsource/barlow-condensed/700.css';
import '@fontsource/ibm-plex-mono/400.css';
import './styles.css';

import { StrictMode, useEffect, useMemo, useState } from 'react';
import { createRoot } from 'react-dom/client';

import { api, LiveContext, useApi, useLiveConnection, type Device, type Entry, type Me, type Registry, type Site } from './api.ts';
import { ConditionBand, ElevatedNotices, SvgDefs } from './components.tsx';
import { Drills } from './views/Drills.tsx';
import { Firmware } from './views/Firmware.tsx';
import { Incidents } from './views/Incidents.tsx';
import { Ledger } from './views/Ledger.tsx';
import { NodeDetail } from './views/NodeDetail.tsx';
import { Pairing } from './views/Pairing.tsx';
import { Plan } from './views/Plan.tsx';
import { RegistryView } from './views/Registry.tsx';
import { SignIn } from './views/SignIn.tsx';
import { SiteLedger } from './views/SiteLedger.tsx';
import { Timeline } from './views/Timeline.tsx';

const ROLE_TEXT: Record<Me['role'], string> = { viewer: 'Viewer', operator: 'Operator', admin: 'Administrator' };

function useRoute(): string[] {
  const [hash, setHash] = useState(location.hash);
  useEffect(() => {
    const onChange = () => setHash(location.hash);
    window.addEventListener('hashchange', onChange);
    return () => window.removeEventListener('hashchange', onChange);
  }, []);
  return hash.replace(/^#\/?/, '').split('/').filter(Boolean);
}

function Shell({ me, onSignOut }: { me: Me; onSignOut: () => void }) {
  const live = useLiveConnection(true);
  return (
    <LiveContext.Provider value={live}>
      <Frame me={me} onSignOut={onSignOut} liveConnected={live.connected} />
    </LiveContext.Provider>
  );
}

function Frame({ me, onSignOut, liveConnected }: { me: Me; onSignOut: () => void; liveConnected: boolean }) {
  const route = useRoute();
  const site = useApi<Site>('/api/site');
  const devices = useApi<Device[]>('/api/devices');
  const changes = useApi<Entry[]>('/api/timeline?types=state_change&limit=100');
  /* The registry changes only when an installer edits it on the gateway. A 404 means alerting is not set up. */
  const registry = useApi<Registry>('/api/registry', false);

  /* The reason each unit is in its current state: the newest state_change per unit. */
  const reasons = useMemo(() => {
    const out: Record<string, string[]> = {};
    for (const e of changes.data ?? []) {
      if (!(e.device_id in out)) out[e.device_id] = (e.payload.reason as string[]) ?? [];
    }
    return out;
  }, [changes.data]);

  /* Removed units keep their records (ledger, site record) but leave the plan and the band. */
  const all = useMemo(() => devices.data ?? [], [devices.data]);
  const list = useMemo(() => all.filter((d) => !d.removed), [all]);
  const alerting = registry.data !== undefined || (registry.error !== null && registry.status !== 404);

  const openIncidents = list.filter((d) => d.open_incident !== null).length;
  const page = route[0] ?? 'plan';
  const nav: [string, string, number?][] = [
    ['plan', 'Site plan'],
    ['incidents', 'Incidents', openIncidents || undefined],
  ];
  if (alerting) nav.push(['drills', 'Drills']);
  nav.push(['timeline', 'Timeline'], ['ledger', 'Ledger'], ['site-record', 'Site record']);
  if (alerting) nav.push(['registry', 'Who is alerted']);
  nav.push(['firmware', 'Firmware']);
  if (me.role !== 'viewer') nav.push(['pairing', 'Add a unit']);

  let view;
  if (page === 'nodes' && route[1]) view = <NodeDetail id={route[1]} me={me} />;
  else if (page === 'incidents') view = <Incidents me={me} registry={registry.data ?? null} />;
  else if (page === 'drills') view = <Drills me={me} registry={registry.data ?? null} />;
  else if (page === 'timeline') view = <Timeline />;
  else if (page === 'ledger') view = <Ledger selected={route[1] ?? null} devices={all} />;
  else if (page === 'site-record') view = <SiteLedger devices={all} />;
  else if (page === 'registry') view = <RegistryView registry={registry.data} status={registry.status} error={registry.error} />;
  else if (page === 'firmware') view = <Firmware me={me} devices={list} />;
  else if (page === 'pairing') view = <Pairing me={me} />;
  else view = <Plan me={me} devices={list} site={site.data} reloadSite={site.reload} />;

  return (
    <div className="shell">
      <SvgDefs />
      <header className="titleblock">
        <a className="wordmark" href="#/plan">
          <svg width="26" height="26" viewBox="0 0 100 100" aria-hidden="true">
            <polygon points="50,3 91,26.5 91,73.5 50,97 9,73.5 9,26.5" fill="#00824a" stroke="#1c2321" strokeWidth="7" />
          </svg>
          FireX
        </a>
        <div className="site">
          <strong>{site.data?.name ?? 'Site gateway'}</strong>
          <span className="small muted">{site.data?.address || 'Fire and hazard detection, this building'}</span>
        </div>
        <div className="account">
          <span className="feed" data-live={liveConnected && Boolean(site.data?.mqtt_connected)}>
            {!liveConnected ? 'Reconnecting to gateway' : site.data?.mqtt_connected ? 'Live' : 'Gateway cannot reach its broker'}
          </span>
          <span>
            {me.username} <span className="role">{ROLE_TEXT[me.role]}</span>
          </span>
          <button className="link-button" type="button" onClick={onSignOut}>
            Sign out
          </button>
        </div>
      </header>
      <div>
        <ConditionBand devices={list} reasons={reasons} />
        <ElevatedNotices devices={list} />
      </div>
      <div className="body">
        <nav className="nav" aria-label="Sections">
          {nav.map(([key, label, count]) => (
            <a key={key} href={`#/${key}`} aria-current={page === key || (key === 'plan' && page === 'nodes') ? 'page' : undefined}>
              {label}
              {count ? <span className="count" aria-label={`${count} open`}>{count}</span> : null}
            </a>
          ))}
        </nav>
        <main>{devices.error ? <p className="error">{devices.error}</p> : view}</main>
      </div>
    </div>
  );
}

function App() {
  const [me, setMe] = useState<Me | null | undefined>(undefined);

  useEffect(() => {
    api<Me>('/api/me').then(setMe, () => setMe(null));
    const onSignedOut = () => setMe(null);
    window.addEventListener('firex:signed-out', onSignedOut);
    return () => window.removeEventListener('firex:signed-out', onSignedOut);
  }, []);

  if (me === undefined) return null;
  if (me === null) return <SignIn onSignedIn={setMe} />;
  return (
    <Shell
      me={me}
      onSignOut={() => {
        api('/api/logout', { method: 'POST', body: {} }).finally(() => setMe(null));
      }}
    />
  );
}

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
