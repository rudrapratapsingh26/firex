/**
 * The gateway's own MQTT connection: mutual TLS to the site broker as
 * `firex-server`, subscribed to every node topic, handing messages to ingest
 * in arrival order. Ingest is synchronous, so a message is fully stored and
 * acknowledged before the next one is looked at.
 */
import mqtt, { type MqttClient } from 'mqtt';

import type { Publish } from './ingest.ts';

export interface BridgeOptions {
  url: string;
  ca: string;
  cert: string;
  key: string;
  onMessage: (topic: string, payload: Buffer) => void;
  onStatus?: (connected: boolean) => void;
  /** Hostname the broker certificate must name, when the URL uses an address it does not. */
  servername?: string;
}

export interface Bridge {
  publish: Publish;
  connected: () => boolean;
  close: () => Promise<void>;
}

const NODE_TOPICS = ['firex/+/+/hello', 'firex/+/+/state', 'firex/+/+/telemetry', 'firex/+/+/ledger', 'firex/+/+/media/+', 'firex/+/+/trace/+'];

export function startBridge(options: BridgeOptions): Bridge {
  const client: MqttClient = mqtt.connect(options.url, {
    clientId: 'firex-server',
    protocolVersion: 4,
    clean: true,
    reconnectPeriod: 2000,
    connectTimeout: 10_000,
    ca: options.ca,
    cert: options.cert,
    key: options.key,
    rejectUnauthorized: true,
    servername: options.servername,
  });

  client.on('connect', () => {
    client.subscribe(NODE_TOPICS, { qos: 1 });
    options.onStatus?.(true);
  });
  client.on('close', () => options.onStatus?.(false));
  client.on('error', () => options.onStatus?.(false));
  client.on('message', (topic, payload) => options.onMessage(topic, payload));

  return {
    publish: (topic, payload, opts) => {
      client.publish(topic, payload, { qos: opts.qos, retain: opts.retain ?? false });
    },
    connected: () => client.connected,
    close: () => new Promise((resolve) => client.end(false, {}, () => resolve())),
  };
}
