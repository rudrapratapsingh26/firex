import { useContext, useEffect, useState, type ReactNode } from 'react';

import { LiveContext, type AlertEvent, type Device, type Escalation, type Registry } from './api.ts';
import {
  CHANNEL_TEXT, clockMs, CONDITION_TEXT, condition, duration, maskRecipient, modalityList, SERVICE_TEXT, SERVICES, SEVERITY, unitName, VERIFY_TEXT,
  type Condition,
} from './format.ts';

const HEX_POINTS = '50,3 91,26.5 91,73.5 50,97 9,73.5 9,26.5';

/** Shared SVG definitions: the diagonal hatch that marks a fault, as on safety signage. */
export function SvgDefs() {
  return (
    <svg width="0" height="0" style={{ position: 'absolute' }} aria-hidden="true">
      <defs>
        <pattern id="hatch" width="12" height="12" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">
          <rect width="12" height="12" fill="#f2b705" />
          <rect width="5" height="12" fill="#1c2321" />
        </pattern>
      </defs>
    </svg>
  );
}

export function Hex({ condition: c, size = 28, title }: { condition: Condition; size?: number; title?: string }) {
  return (
    <svg className="hex" data-condition={c} width={size} height={size} viewBox="0 0 100 100" role="img" aria-label={title ?? CONDITION_TEXT[c]}>
      <polygon points={HEX_POINTS} />
    </svg>
  );
}

export function VerifyMark({ result }: { result: string }) {
  const ok = result === 'ok' || result === 'start' || result === 'gap';
  return (
    <span className="verify" data-ok={ok}>
      <svg width="14" height="14" viewBox="0 0 14 14" aria-hidden="true">
        {ok ? (
          <path d="M2 7.5 5.5 11 12 3.5" fill="none" stroke="currentColor" strokeWidth="2.2" />
        ) : (
          <path d="M3 3l8 8M11 3l-8 8" fill="none" stroke="currentColor" strokeWidth="2.2" />
        )}
      </svg>
      {VERIFY_TEXT[result] ?? result}
    </span>
  );
}

/** A small uppercase tag: DRILL, LIVE, LATE. */
export function Badge({ kind, children }: { kind: string; children: ReactNode }) {
  return (
    <span className="badge" data-kind={kind}>
      {children}
    </span>
  );
}

/** A score trace from 0 to 100, with the entry and exit thresholds drawn in. */
export function Sparkline({ values, enter = 60, exit = 40, label }: { values: number[]; enter?: number; exit?: number; label: string }) {
  const n = values.length;
  const y = (v: number) => 40 - (Math.max(0, Math.min(100, v)) / 100) * 40;
  const points = values.map((v, i) => `${n === 1 ? 100 : (i / (n - 1)) * 100},${y(v)}`).join(' ');
  return (
    <svg className="spark" viewBox="0 0 100 40" preserveAspectRatio="none" role="img" aria-label={label}>
      <line className="threshold" x1="0" x2="100" y1={y(enter)} y2={y(enter)} />
      <line className="threshold" x1="0" x2="100" y1={y(exit)} y2={y(exit)} />
      {n > 0 && <polyline className="trace" points={points} />}
    </svg>
  );
}

/** Across the top of every page: what the building's worst unit is doing, in words. */
export function ConditionBand({ devices, reasons }: { devices: Device[]; reasons: Record<string, string[]> }) {
  const ranked = [...devices].sort((a, b) => SEVERITY.indexOf(condition(a)) - SEVERITY.indexOf(condition(b)));
  const top = ranked[0];
  const c = top ? condition(top) : 'unknown';
  const sameCount = ranked.filter((d) => condition(d) === c).length;

  if (!top) {
    return (
      <div className="band" data-condition="offline" role="status">
        <span className="band-state">No units yet</span>
        <span>Add a unit from the pairing page with the code printed on its label.</span>
      </div>
    );
  }
  if (c === 'safe') {
    return (
      <div className="band" data-condition="safe" role="status">
        <span className="band-state">All {devices.length} {devices.length === 1 ? 'unit' : 'units'} safe</span>
      </div>
    );
  }

  const where = unitName(top);
  const others = sameCount > 1 ? ` and ${sameCount - 1} more` : '';
  const why = modalityList(reasons[top.id]);
  const text: Record<Condition, string> = {
    critical: `Critical at ${where}${others}`,
    silenced: `Critical at ${where}, buzzer silenced`,
    elevated: `Elevated at ${where}${others}`,
    fault: `Fault at ${where}${others}`,
    offline: `${where}${others} not reporting`,
    selftest: `Self-test running at ${where}`,
    warmup: `${where}${others} warming up, no alarm possible yet`,
    unknown: `${where} has not reported yet`,
    safe: '',
  };
  return (
    <div className="band" data-condition={c} role={c === 'critical' ? 'alert' : 'status'}>
      <span className="band-state">{text[c]}</span>
      {why && (c === 'critical' || c === 'silenced' || c === 'elevated') && <span>{why[0].toUpperCase() + why.slice(1)} {c === 'elevated' ? 'above threshold' : 'agree'}</span>}
      <a href={`#/nodes/${top.id}`}>Open {where}</a>
      {top.open_incident !== null && <a href="#/incidents">See the photos</a>}
    </div>
  );
}

/**
 * A quiet notice for each unit that went ELEVATED since the page opened. Elevated
 * notifies the site on this dashboard and nowhere else, so the notice says so.
 */
export function ElevatedNotices({ devices }: { devices: Device[] }) {
  const { events } = useContext(LiveContext);
  const [ids, setIds] = useState<string[]>([]);

  useEffect(() => {
    const fresh = events.filter((e) => e.kind === 'elevated' && typeof e.device_id === 'string').map((e) => e.device_id as string);
    if (fresh.length) setIds((current) => [...current, ...fresh.filter((id, i) => !current.includes(id) && fresh.indexOf(id) === i)]);
  }, [events]);

  const visible = ids
    .map((id) => devices.find((d) => d.id === id))
    .filter((d): d is Device => d !== undefined)
    .filter((d) => !['safe', 'critical', 'silenced'].includes(condition(d)));
  if (visible.length === 0) return null;

  return (
    <div className="notices" role="status" aria-live="polite">
      {visible.map((d) => (
        <div className="notice" key={d.id}>
          <Hex condition="elevated" size={18} title="Elevated" />
          <span>
            <strong>{unitName(d)} is elevated.</strong> The site is notified on this dashboard only. Outside services are alerted only on a critical alarm.
          </span>
          <a href={`#/nodes/${d.id}`}>Open {unitName(d)}</a>
          <button type="button" className="link-button dismiss" onClick={() => setIds((current) => current.filter((x) => x !== d.id))} aria-label={`Dismiss the notice about ${unitName(d)}`}>
            Dismiss
          </button>
        </div>
      ))}
    </div>
  );
}

/* ---- who was alerted ----------------------------------------------------------------- */

interface Step {
  at: number;
  text: string;
  bad?: boolean;
}

type StatusKind = 'ok' | 'waiting' | 'escalated' | 'failed' | 'withheld';

interface ServiceView {
  service: string;
  steps: Step[];
  status: { kind: StatusKind; text: string };
  contactLine: string | null;
  mode: 'drill' | 'live' | null;
}

function viewService(
  service: string,
  alerts: AlertEvent[],
  registry: Registry | null | undefined,
  drill: boolean,
  escalation: Escalation | undefined,
  late: boolean,
  include: string[] | undefined,
): ServiceView {
  const mine = alerts.filter((a) => a.service === service).sort((a, b) => a.at_ms - b.at_ms);
  const contact = registry?.contacts.find((c) => c.service === service);
  const steps: Step[] = [];
  let firstChannel = '';
  let firstRecipient = '';

  for (const a of mine) {
    const p = a.payload ?? {};
    if (a.type === 'alert_sent') {
      const how = CHANNEL_TEXT[String(p.channel)] ?? String(p.channel ?? '');
      const to = contact?.recipient_masked ?? maskRecipient(String(p.recipient ?? ''));
      if (!firstChannel) {
        firstChannel = how;
        firstRecipient = to;
      }
      steps.push({ at: a.at_ms, text: `${p.retry_of ? 'Sent again' : 'Sent'}${how ? ` by ${how}` : ''}${to && p.retry_of ? ` to ${to}` : ''}` });
      if (typeof p.delivered_at_ms === 'number') steps.push({ at: p.delivered_at_ms, text: 'Delivered' });
    } else if (a.type === 'alert_ack') {
      steps.push({ at: a.at_ms, text: `Acknowledged${p.by ? ` by ${String(p.by)}` : ''} after ${duration(Number(p.latency_s))}` });
    } else if (a.type === 'alert_timeout') {
      const again = p.action === 're_alert';
      steps.push({
        at: a.at_ms,
        text: `No answer after ${duration(Number(p.elapsed_s))}: ${again ? 'alerting again' : 'no more re-alerts, raised to the site'}`,
        bad: !again,
      });
    } else if (a.type === 'alert_failed') {
      const how = CHANNEL_TEXT[String(p.channel)] ?? String(p.channel ?? '');
      steps.push({ at: a.at_ms, text: `Could not send${how ? ` by ${how}` : ''}${p.error ? `: ${String(p.error)}` : ''}`, bad: true });
    }
  }
  steps.sort((x, y) => x.at - y.at);

  const sends = mine.filter((a) => a.type === 'alert_sent');
  const ack = mine.find((a) => a.type === 'alert_ack');
  const escalated = mine.some((a) => a.type === 'alert_timeout' && a.payload?.action !== 're_alert');
  const failed = mine.some((a) => a.type === 'alert_failed');
  const state = escalation?.state;

  let status: ServiceView['status'];
  if (ack) {
    status = { kind: 'ok', text: `Acknowledged${ack.payload?.by ? ` by ${String(ack.payload.by)}` : ''}` };
  } else if (sends.length === 0) {
    if (drill && include && !include.includes(service)) status = { kind: 'withheld', text: 'Not in this drill' };
    else if (failed) status = { kind: 'failed', text: 'Could not be alerted' };
    else if (!drill && contact?.mode === 'drill') status = { kind: 'withheld', text: 'Drill mode: not alerted' };
    else if (!drill && escalation?.reason === 'drill_mode') status = { kind: 'withheld', text: 'Drill mode: not alerted' };
    else if (!drill && escalation?.reason === 'verification') status = { kind: 'withheld', text: 'Not alerted' };
    else if (!drill && (late || escalation?.reason === 'late') && service !== 'site') status = { kind: 'withheld', text: 'Late report: not alerted' };
    else if (state === 'running') status = { kind: 'waiting', text: 'Not alerted yet' };
    else status = { kind: 'withheld', text: 'Not alerted' };
  } else if (escalated) {
    status = { kind: 'escalated', text: 'No answer' };
  } else if (state === 'finished' || state === 'withheld') {
    status = { kind: 'escalated', text: 'No answer recorded' };
  } else {
    status = { kind: 'waiting', text: 'Waiting for an answer' };
  }

  const contactLine = contact
    ? `${contact.name}, ${CHANNEL_TEXT[contact.channel] ?? contact.channel}${contact.recipient_masked ? ` ${contact.recipient_masked}` : ''}`
    : firstChannel
      ? `${firstChannel}${firstRecipient ? ` ${firstRecipient}` : ''}`
      : null;
  return { service, steps, status, contactLine, mode: contact?.mode ?? null };
}

/**
 * One row per recipient of a critical alarm or a drill: how it was sent, whether it
 * arrived, who answered and how long they took, and every silence in between.
 */
export function EscalationPanel({
  alerts, registry, drill = false, escalation, late = false, include, onAck,
}: {
  alerts: AlertEvent[];
  registry?: Registry | null;
  drill?: boolean;
  escalation?: Escalation;
  late?: boolean;
  /** For a drill: the services it included. */
  include?: string[];
  /** Given when the viewer may acknowledge for the site. */
  onAck?: () => Promise<void>;
}) {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const extra = [...new Set(alerts.map((a) => a.service))].filter((svc) => !SERVICES.includes(svc));
  const rows = [...SERVICES, ...extra].map((svc) => viewService(svc, alerts, registry, drill, escalation, late, include));
  const state = escalation?.state ?? (alerts.length ? 'finished' : 'none');

  let summary: string;
  if (drill) summary = 'Every message was marked DRILL, not a real incident. Answers and silences are timed as for a real alarm.';
  else if (escalation?.reason === 'verification') summary = 'Nobody was alerted: the unit’s alarm entry failed verification when it reached the gateway.';
  else if (late || escalation?.reason === 'late') summary = 'Reported late, after the unit had been offline: only the site was alerted.';
  else if (state === 'none' && alerts.length === 0) summary = 'Alerting is not set up on this gateway. Nobody outside this dashboard was told.';
  else if (state === 'running') summary = 'Alerting now. A service that does not answer in time is alerted again.';
  else summary = 'Alerting finished.';

  const siteSent = alerts.some((a) => a.type === 'alert_sent' && a.service === 'site');
  const siteAcked = alerts.some((a) => a.type === 'alert_ack' && a.service === 'site');

  const acknowledge = async () => {
    if (!onAck) return;
    setBusy(true);
    try {
      await onAck();
      setError(null);
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setBusy(false);
    }
  };

  return (
    <section className="escalation" aria-label={drill ? 'Who the drill alerted' : 'Who was alerted'}>
      <div className="escalation-head">
        <h3>{drill ? 'Drill messages' : 'Who was alerted'}</h3>
        <p className="small">{summary}</p>
      </div>
      <ul className="services">
        {rows.map((row) => (
          <li className="service" data-status={row.status.kind} key={row.service}>
            <div className="service-head">
              <strong>{SERVICE_TEXT[row.service] ?? row.service}</strong>
              {!drill && row.mode && <Badge kind={row.mode}>{row.mode === 'live' ? 'Live' : 'Drill mode'}</Badge>}
              {row.contactLine && <span className="small muted">{row.contactLine}</span>}
              <span className="service-status">{row.status.text}</span>
            </div>
            {row.steps.length > 0 && (
              <ol className="alert-steps">
                {row.steps.map((step, i) => (
                  <li key={i} data-bad={step.bad ? 'true' : undefined}>
                    <time dateTime={new Date(step.at).toISOString()}>{clockMs(step.at)}</time>
                    <span>{step.text}</span>
                  </li>
                ))}
              </ol>
            )}
          </li>
        ))}
      </ul>
      {onAck && !drill && siteSent && !siteAcked && (
        <div className="form-row">
          <button className="button" type="button" disabled={busy} onClick={acknowledge}>
            Acknowledge for the site
          </button>
          <span className="small muted">Records your name and the time, and stops the site being alerted again.</span>
        </div>
      )}
      {error && <p className="error" role="alert">{error}</p>}
    </section>
  );
}
