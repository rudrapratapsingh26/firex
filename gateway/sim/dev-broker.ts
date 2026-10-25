/**
 * A development MQTT broker for a laptop: Aedes over mutual TLS, with the same
 * access rules as infra/mosquitto/acl and the certificates firex-server keeps in
 * its data directory. For rehearsing the gateway, dashboard and node simulator
 * without Docker. A site runs Mosquitto (infra/docker-compose.yml).
 *
 *   npm run gateway -- --init-only      # creates the site CA and broker certificate
 *   npm run dev:broker                  # listens on 8883
 *   npm run gateway                     # in another terminal
 *
 * The site's CRL is loaded, and reloaded whenever firex-server rewrites it, so a
 * removed unit cannot connect again here either.
 */
import { existsSync, readFileSync, watchFile } from 'node:fs';
import { join } from 'node:path';
import { createServer } from 'node:tls';

import { Aedes } from 'aedes';

import { loadConfig } from '../server/config.ts';
import { certificateCommonName, mayPublish, maySubscribe } from './acl.ts';

async function main(): Promise<void> {
  const config = loadConfig();
  const port = Number(process.env.FIREX_DEV_BROKER_PORT ?? 8883);
  const broker = await Aedes.createBroker({
    authorizePublish: (client, packet, callback) => {
      const cn = client ? certificateCommonName(client.conn) : '';
      callback(client && !mayPublish(cn, packet.topic) ? new Error(`${cn} may not publish ${packet.topic}`) : null);
    },
    authorizeSubscribe: (client, sub, callback) => {
      const ok = maySubscribe(certificateCommonName(client.conn), sub.topic);
      callback(ok ? null : new Error(`not permitted: ${sub.topic}`), ok ? sub : null);
    },
  });
  broker.on('client', (client) => console.log(`dev-broker: ${certificateCommonName(client.conn) || client.id} connected`));
  broker.on('clientDisconnect', (client) => console.log(`dev-broker: ${client.id} disconnected`));

  const crlFile = join(config.pkiDir, 'crl.pem');
  const secure = () => ({
    key: readFileSync(join(config.pkiDir, 'broker.key')),
    cert: readFileSync(join(config.pkiDir, 'broker.crt')),
    ca: readFileSync(join(config.pkiDir, 'ca.crt')),
    crl: existsSync(crlFile) ? readFileSync(crlFile) : undefined,
  });
  const server = createServer({ ...secure(), requestCert: true, rejectUnauthorized: true }, broker.handle);
  watchFile(crlFile, { interval: 2000 }, () => {
    server.setSecureContext(secure());
    console.log('dev-broker: CRL reloaded');
  });
  server.listen(port, '0.0.0.0', () => console.log(`dev-broker: mutual TLS on :${port}, certificates from ${config.pkiDir}`));
}

main().catch((err) => {
  console.error('dev-broker failed:', err instanceof Error ? err.message : err);
  process.exit(1);
});
