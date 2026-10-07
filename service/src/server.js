import { createCameraServer } from './app.js';

const required = name => {
  const value = process.env[name];
  if (!value) throw new Error(`${name} is required`);
  return value;
};

const app = createCameraServer({
  port: process.env.PORT ?? 8790,
  host: process.env.HOST ?? '0.0.0.0',
  publicBaseUrl: process.env.PUBLIC_BASE_URL ?? 'https://tazzio.pl',
  tokenSecret: required('TOKEN_SECRET'),
  turnSecret: required('TURN_SECRET'),
  turnHost: process.env.TURN_HOST ?? '185.238.72.154',
});

await app.listen();
console.log(JSON.stringify({ level: 'info', event: 'server_started', product: 'tazzio-iphone-camera', version: '1.0.2', port: Number(process.env.PORT ?? 8790) }));
for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, async () => { await app.close(); process.exit(0); });
