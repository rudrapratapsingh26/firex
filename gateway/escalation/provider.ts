/**
 * AlertProvider — the escalation interface.
 *
 * The same shape as AnchorProvider in Software Architecture v1.0: everything
 * above this line (fan-out, retries, acknowledgement tracking, ledger entries)
 * is built and tested in M0 against ConsoleAlert, so no part of the system
 * waits on an agreement with a control room.
 *
 *   ConsoleAlert   prints to a terminal            M0, no account needed
 *   HttpAlert      mock endpoints                  M4 dry runs
 *   ModemAlert     SMS + ringing call, GSM modem   M5 pilots
 *   IcccAlert      city command centre / ERSS-112  Q3, if an agreement lands
 */

export type Service = 'fire' | 'police' | 'ambulance' | 'site';
/** `dashboard`: the recipient answers on the gateway's dashboard; nothing is sent anywhere. */
export type Channel = 'api' | 'sms' | 'voice' | 'console' | 'dashboard';
/** `drill` services receive drills only; `live` services receive real incidents, under an agreement. */
export type ContactMode = 'drill' | 'live';

/** One service as mapped to a site at installation, never looked up at alarm time. */
export interface ServiceContact {
  service: Service;
  name: string;
  recipient: string;
  channel: Channel;
  backup?: { name: string; recipient: string; channel: Channel };
  /** Absent means drill: a service is connected to real incidents only on purpose. */
  mode?: ContactMode;
  /** The signed arrangement that lets a live service receive real alerts. */
  agreement?: { reference: string; date: string };
}

/** A site's registry: which station, which police station, which ambulance. */
export interface SiteRegistry {
  site_id: string;
  site_name: string;
  address: string;
  location: { lat: number; lon: number };
  contacts: ServiceContact[];
  /** 1 for files written before M5. */
  version?: number;
  ack_timeout_s?: number;
  max_realerts?: number;
  offline_notify_min?: number;
}

/**
 * incident   a verified CRITICAL, alerted as it happens
 * late       a CRITICAL that reached the gateway after the unit's outage, already over: site only
 * drill      a test of the contacts, never a real incident
 * notice     a message to the site that needs no answer (a unit gone offline)
 */
export type AlertKind = 'incident' | 'late' | 'drill' | 'notice';

export interface Alert {
  incident_id: string;
  site: SiteRegistry;
  node_id: string;
  where: string;
  hazard: string;
  scores: Record<string, number>;
  media: { frames: number; clip_seconds: number; hashes: string[] };
  ledger_seq: number;
  ledger_hash: string;
  /** Unix seconds, possibly fractional. */
  raised_at: number;
  /** When the gateway stored the CRITICAL entry (Unix ms), for timing the alert path. */
  ledger_received_at_ms?: number;
  /** Absent means incident. */
  kind?: AlertKind;
  /** The text of a notice. */
  notice_text?: string;
}

/** Unix seconds, possibly fractional, throughout: whole seconds are too coarse for a 60 s window. */
export interface Receipt {
  receipt_id: string;
  service: Service;
  channel: Channel;
  recipient: string;
  sent_at: number;
  /** When the recipient's endpoint confirmed delivery, if the channel reports it. */
  delivered_at?: number;
}

export interface SendContext {
  /** The receipt this send repeats, for a re-alert. */
  retry_of?: string;
}

export interface Ack {
  receipt_id: string;
  service: Service;
  by: string;
  at: number;
}

/** Something a recipient sent back: an SMS, or a key pressed during a call. */
export interface Reply {
  from: string;
  body: string;
  /** Unix seconds. */
  at: number;
  dtmf?: boolean;
}

export interface AlertProvider {
  readonly name: string;
  /** Rejects when the alert could not be handed over. */
  send(alert: Alert, contact: ServiceContact, context?: SendContext): Promise<Receipt>;
  /** Returns null while the service has not acknowledged yet. */
  ack(receiptId: string): Promise<Ack | null>;
  /** Providers that receive replies (SMS, key presses) report them here; the runner decides what they acknowledge. */
  onReply?(listener: (reply: Reply) => void): void;
}
