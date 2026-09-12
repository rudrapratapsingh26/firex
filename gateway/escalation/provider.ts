/**
 * AlertProvider — the escalation interface.
 *
 * The same shape as AnchorProvider in Software Architecture v1.0: everything
 * above this line (fan-out, retries, acknowledgement tracking, ledger entries)
 * is built and tested in M0 against ConsoleAlert, so no part of the system
 * waits on an agreement with a control room.
 *
 *   ConsoleAlert   prints to a terminal            M0, no account needed
 *   SmsVoiceAlert  SMS + automated call            M5 pilots
 *   IcccAlert      city command centre / ERSS-112  Q3, if an agreement lands
 */

export type Service = 'fire' | 'police' | 'ambulance' | 'site';
export type Channel = 'api' | 'sms' | 'voice' | 'console';

/** One service as mapped to a site at installation, never looked up at alarm time. */
export interface ServiceContact {
  service: Service;
  name: string;
  recipient: string;
  channel: Channel;
  backup?: { name: string; recipient: string; channel: Channel };
}

/** A site's registry: which station, which police station, which ambulance. */
export interface SiteRegistry {
  site_id: string;
  site_name: string;
  address: string;
  location: { lat: number; lon: number };
  contacts: ServiceContact[];
}

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
  raised_at: number;
}

export interface Receipt {
  receipt_id: string;
  service: Service;
  channel: Channel;
  recipient: string;
  sent_at: number;
}

export interface Ack {
  receipt_id: string;
  service: Service;
  by: string;
  at: number;
}

export interface AlertProvider {
  readonly name: string;
  send(alert: Alert, contact: ServiceContact): Promise<Receipt>;
  /** Returns null while the service has not acknowledged yet. */
  ack(receiptId: string): Promise<Ack | null>;
}
