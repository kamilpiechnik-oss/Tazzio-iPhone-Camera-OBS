import assert from 'node:assert/strict';
import test from 'node:test';
import { createCameraServer } from '../src/app.js';

test('standalone camera service pairs once and routes signaling', async () => {
  const app = createCameraServer({ port: 0, host: '127.0.0.1', publicBaseUrl: 'https://iphone-camera.test', tokenSecret: 'test-token-secret-123456', turnSecret: 'test-turn-secret' });
  await app.listen();
  const base = `http://127.0.0.1:${app.server.address().port}`;
  try {
    const created = await fetch(`${base}/api/camera/sessions`, { method: 'POST' }).then(r => r.json());
    assert.match(created.pairing_url, /^https:\/\/iphone-camera\.test\/camera\//);
    assert.ok(created.ice_servers.some(server => server.urls.some(url => url.startsWith('stun:'))));
    assert.ok(created.ice_servers.some(server => server.urls.some(url => url.startsWith('turn:'))));
    const pairToken = new URL(created.pairing_url).pathname.split('/').pop();
    const pairResponse = await fetch(`${base}/api/camera/pair`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pair_token: pairToken }) });
    assert.equal(pairResponse.status, 200);
    const senderToken = (await pairResponse.json()).socket_token;
    const secondPair = await fetch(`${base}/api/camera/pair`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pair_token: pairToken }) });
    assert.equal(secondPair.status, 410);
    const connect = token => new Promise((resolve, reject) => { const ws = new WebSocket(`ws://127.0.0.1:${app.server.address().port}/camera-ws?token=${encodeURIComponent(token)}`); ws.addEventListener('message', event => { if (JSON.parse(event.data).type === 'hello') resolve(ws); }); ws.addEventListener('error', reject); });
    const receiver = await connect(created.socket_token);
    const sender = await connect(senderToken);
    const received = new Promise((resolve, reject) => { const timeout = setTimeout(() => reject(new Error('timeout')), 1000); receiver.addEventListener('message', event => { const message = JSON.parse(event.data); if (message.type === 'offer') { clearTimeout(timeout); resolve(message); } }); });
    sender.send(JSON.stringify({ type: 'offer', payload: { type: 'offer', sdp: 'v=0' } }));
    assert.equal((await received).payload.sdp, 'v=0');
    sender.close(); receiver.close();
  } finally { await app.close(); }
});

test('health identifies only iPhone Camera', async () => {
  const app = createCameraServer({ port: 0, host: '127.0.0.1' });
  await app.listen();
  try {
    const health = await fetch(`http://127.0.0.1:${app.server.address().port}/api/health`).then(r => r.json());
    assert.deepEqual(health, { status: 'ok', product: 'tazzio-iphone-camera', version: '1.0.2' });
  } finally { await app.close(); }
});

test('camera page exposes separate LAN and VPS modes with restored quality profiles', async () => {
  const app = createCameraServer({ port: 0, host: '127.0.0.1' });
  await app.listen();
  const base = `http://127.0.0.1:${app.server.address().port}`;
  try {
    const html = await fetch(`${base}/camera/test-token`).then(response => response.text());
    const script = await fetch(`${base}/camera.js`).then(response => response.text());
    assert.match(html, /Wi‑Fi \/ LAN — bezpośrednio/);
    assert.match(html, /Sieć komórkowa \/ Internet — przez VPS/);
    assert.match(html, /BALANCED · 1080p \/ 12 Mb\/s/);
    assert.match(html, /HIGH QUALITY · 1080p \/ 16 Mb\/s/);
    assert.match(script, /iceTransportPolicy='relay'/);
    assert.match(script, /degradationPreference='maintain-resolution'/);
  } finally { await app.close(); }
});
