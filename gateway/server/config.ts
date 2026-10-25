/**
 * Gateway configuration, from the environment. Every value has a default that
 * works on a laptop; docker-compose.yml sets the ones a Pi needs.
 */
import { join, resolve } from 'node:path';

export interface GatewayConfig {
  dataDir: string;
  dbPath: string;
  pkiDir: string;
  mediaDir: string;
  dashboardDir: string;
  httpHost: string;
  httpPort: number;
  provisionPort: number;
  mqttUrl: string;
  /** Hostname and port the nodes are told to connect to; also in the broker certificate. */
  advertisedMqttHost: string;
  advertisedMqttPort: number;
  brokerNames: string[];
  siteId: string;
  siteName: string;
  mediaRetentionDays: number;
  telemetryRetentionDays: number;
  pairingTtlHours: number;
  /** The site's service registry (docs/service-registry.md); escalation needs it. */
  serviceRegistryPath: string | null;
  /** off, console or http. In M4, http points only at mock endpoints. */
  alertProvider: string;
  alertHttpUrl: string | null;
  alertAckTimeoutS: number;
  alertMaxReAlerts: number;
  alertPollMs: number;
  /** HTTPS port units download firmware from, with their client certificates. */
  fwPort: number;
  /** SHA-256 of the secure boot v2 public key the units trust (`espsecure.py digest_sbv2_public_key`). */
  fwSigningKeySha256: string | null;
  firmwareDir: string;
  tracesDir: string;
  /** The USB GSM modem's tty, for FIREX_ALERT_PROVIDER=modem. */
  modemDevice: string;
  modemBaud: number;
  /** How long a voice alert rings before the gateway hangs up. */
  modemRingS: number;
}

function num(value: string | undefined, fallback: number): number {
  const n = value === undefined ? NaN : Number(value);
  return Number.isFinite(n) ? n : fallback;
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): GatewayConfig {
  const dataDir = resolve(env.FIREX_DATA_DIR ?? 'gateway-data');
  const advertisedMqttHost = env.FIREX_MQTT_ADVERTISED_HOST ?? 'firex-gateway.local';
  const brokerNames = (env.FIREX_BROKER_NAMES ?? `${advertisedMqttHost},mosquitto,localhost,127.0.0.1`)
    .split(',')
    .map((s) => s.trim())
    .filter(Boolean);
  return {
    dataDir,
    dbPath: join(dataDir, 'firex.sqlite'),
    pkiDir: join(dataDir, 'pki'),
    mediaDir: join(dataDir, 'media'),
    dashboardDir: resolve(env.FIREX_DASHBOARD_DIR ?? 'dashboard/dist'),
    httpHost: env.FIREX_HTTP_HOST ?? '0.0.0.0',
    httpPort: num(env.FIREX_HTTP_PORT, 8080),
    provisionPort: num(env.FIREX_PROVISION_PORT, 8081),
    mqttUrl: env.FIREX_MQTT_URL ?? 'mqtts://localhost:8883',
    advertisedMqttHost,
    advertisedMqttPort: num(env.FIREX_MQTT_ADVERTISED_PORT, 8883),
    brokerNames,
    siteId: env.FIREX_SITE_ID ?? 'site-001',
    siteName: env.FIREX_SITE_NAME ?? 'Bench site',
    mediaRetentionDays: num(env.FIREX_MEDIA_RETENTION_DAYS, 30),
    telemetryRetentionDays: num(env.FIREX_TELEMETRY_RETENTION_DAYS, 90),
    pairingTtlHours: num(env.FIREX_PAIRING_TTL_HOURS, 24),
    serviceRegistryPath: env.FIREX_SERVICE_REGISTRY ? resolve(env.FIREX_SERVICE_REGISTRY) : null,
    alertProvider: (env.FIREX_ALERT_PROVIDER ?? 'off').trim().toLowerCase() || 'off',
    alertHttpUrl: env.FIREX_ALERT_HTTP_URL?.trim() || null,
    alertAckTimeoutS: num(env.FIREX_ALERT_ACK_TIMEOUT_S, 60),
    alertMaxReAlerts: num(env.FIREX_ALERT_MAX_REALERTS, 1),
    alertPollMs: num(env.FIREX_ALERT_POLL_MS, 1000),
    fwPort: num(env.FIREX_FW_PORT, 8443),
    fwSigningKeySha256: env.FIREX_FW_SIGNING_KEY_SHA256?.trim().toLowerCase() || null,
    firmwareDir: resolve(env.FIREX_FIRMWARE_DIR ?? join(dataDir, 'firmware')),
    tracesDir: join(dataDir, 'traces'),
    modemDevice: env.FIREX_MODEM_DEVICE?.trim() || '/dev/ttyUSB2',
    modemBaud: num(env.FIREX_MODEM_BAUD, 115200),
    modemRingS: num(env.FIREX_MODEM_RING_S, 30),
  };
}
