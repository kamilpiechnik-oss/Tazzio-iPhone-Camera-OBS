import assert from 'node:assert/strict';
import test from 'node:test';
import { createCameraServer } from '../src/app.js';

test('standalone camera service pairs once and routes signaling', async () => {
  const app = createCameraServer({ port: 0, host: '127.0.0.1', publicBaseUrl: 'https://iphone-camera.test', tokenSecret: 'test-token-secret-123456', turnSecret: 'test-turn-secret' });
  await app.listen();
  const base = `http://127.0.0.1:${app.server.address().port}`;
  try {
    const created = await fetch(`${base}/api/camera/sessions`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ client_version: '1.0.3' }) }).then(r => r.json());
    assert.match(created.pairing_url, /^https:\/\/iphone-camera\.test\/camera\//);
    assert.equal(created.mode, 'lan');
    assert.match(created.receiver_url, /^https:\/\/iphone-camera\.test\/camera-receiver\//);
    const pairToken = new URL(created.pairing_url).pathname.split('/').pop();
    const pairResponse = await fetch(`${base}/api/camera/pair`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pair_token: pairToken }) });
    assert.equal(pairResponse.status, 200);
    const senderToken = (await pairResponse.json()).socket_token;
    const secondPair = await fetch(`${base}/api/camera/pair`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pair_token: pairToken }) });
    assert.equal(secondPair.status, 410);
    const connect = token => new Promise((resolve, reject) => { const ws = new WebSocket(`ws://127.0.0.1:${app.server.address().port}/camera-ws?token=${encodeURIComponent(token)}`); ws.addEventListener('message', event => { if (JSON.parse(event.data).type === 'hello') resolve(ws); }); ws.addEventListener('error', reject); });
    const receiverToken = new URL(created.receiver_url).pathname.split('/').pop();
    const receiverConfig = await fetch(`${base}/api/camera/config`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ socket_token: receiverToken }) }).then(r => r.json());
    assert.deepEqual(receiverConfig, { mode: 'lan', ice_servers: [] });
    const receiver = await connect(receiverToken);
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
    assert.deepEqual(health, { status: 'ok', product: 'tazzio-iphone-camera', version: '1.0.6' });
  } finally { await app.close(); }
});

test('update endpoint returns a verified direct installer manifest', async () => {
  const update = { updateDownloadUrl: 'https://tazzio.pl/pobierz/8/', updateSha256: 'a'.repeat(64), updateSize: 3900000 };
  const app = createCameraServer({ port: 0, host: '127.0.0.1', ...update });
  await app.listen();
  const base = `http://127.0.0.1:${app.server.address().port}`;
  try {
    const oldVersion = await fetch(`${base}/api/camera/update?current=1.0.3`).then(response => response.json());
    assert.deepEqual(oldVersion, { latest_version: '1.0.6', update_available: true, download_url: update.updateDownloadUrl, sha256: update.updateSha256, size: update.updateSize });
    const currentVersion = await fetch(`${base}/api/camera/update?current=1.0.6`).then(response => response.json());
    assert.equal(currentVersion.update_available, false);
    const futureVersion = await fetch(`${base}/api/camera/update?current=2.0.0`).then(response => response.json());
    assert.equal(futureVersion.update_available, false);
  } finally { await app.close(); }
});

test('update endpoint fails closed when installer verification metadata is missing', async () => {
  const app = createCameraServer({ port: 0, host: '127.0.0.1' });
  await app.listen();
  try {
    const response = await fetch(`http://127.0.0.1:${app.server.address().port}/api/camera/update?current=1.0.5`);
    assert.equal(response.status, 503);
    assert.equal((await response.json()).error, 'update_unavailable');
  } finally { await app.close(); }
});

test('dock-selected mode reaches camera and browser receiver with restored quality profiles', async () => {
  const app = createCameraServer({ port: 0, host: '127.0.0.1' });
  await app.listen();
  const base = `http://127.0.0.1:${app.server.address().port}`;
  try {
    const html = await fetch(`${base}/camera/test-token`).then(response => response.text());
    const script = await fetch(`${base}/camera.js`).then(response => response.text());
    const receiver = await fetch(`${base}/receiver.js`).then(response => response.text());
    const receiverHtml = await fetch(`${base}/camera-receiver/test-token`).then(response => response.text());
    assert.doesNotMatch(html, /id="connection"/);
    assert.match(html, /WYSOKA · 1080p \/ bitrate zależny od FPS/);
    assert.match(html, /MAKSYMALNA · 1080p \/ bitrate zależny od FPS/);
    assert.match(script, /iceTransportPolicy\s*=\s*'relay'/);
    assert.match(script, /degradationPreference\s*=\s*'maintain-resolution'/);
    assert.match(script, /applyConstraints\(\{ frameRate:/);
    assert.match(script, /video: \{ width, height, frameRate:/);
    assert.match(script, /controls\.quality/);
    assert.match(script, /bitrateAt30 \* Math\.max\(1, requestedFps\(\) \/ 30\)/);
    assert.match(script, /else if \(control === 'quality'\) await setQualityProfile\(value\)/);
    assert.match(script, /await applyQuality\(true\)/);
    assert.match(receiver, /iceTransportPolicy='relay'/);
    assert.match(receiverHtml, /body\{position:fixed;inset:0\}/);
    assert.match(receiverHtml, /video\{display:block;width:100vw;height:100vh/);
    assert.match(receiverHtml, /::-webkit-scrollbar\{display:none/);
  } finally { await app.close(); }
});

test('internet session returns TURN-only configuration to both peers', async () => {
  const app = createCameraServer({ port: 0, host: '127.0.0.1', publicBaseUrl: 'https://iphone-camera.test', tokenSecret: 'test-token-secret-123456', turnSecret: 'test-turn-secret' });
  await app.listen();
  const base = `http://127.0.0.1:${app.server.address().port}`;
  try {
    const created = await fetch(`${base}/api/camera/sessions`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ mode: 'internet', client_version: '1.0.3' }) }).then(r => r.json());
    const receiverToken = new URL(created.receiver_url).pathname.split('/').pop();
    const config = await fetch(`${base}/api/camera/config`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ socket_token: receiverToken }) }).then(r => r.json());
    assert.equal(config.mode, 'internet');
    assert.ok(config.ice_servers.length > 0);
    assert.ok(config.ice_servers.every(server => server.urls.every(url => url.startsWith('turn:'))));
  } finally { await app.close(); }
});

test('legacy 1.0.2 session response remains compatible during upgrade', async () => {
  const app = createCameraServer({ port: 0, host: '127.0.0.1', publicBaseUrl: 'https://iphone-camera.test', tokenSecret: 'test-token-secret-123456', turnSecret: 'test-turn-secret' });
  await app.listen();
  try {
    const base = `http://127.0.0.1:${app.server.address().port}`;
    const created = await fetch(`${base}/api/camera/sessions`, { method: 'POST' }).then(r => r.json());
    assert.ok(created.socket_token);
    assert.ok(created.ice_servers.some(server => server.urls.some(url => url.startsWith('stun:'))));
    assert.ok(created.ice_servers.some(server => server.urls.some(url => url.startsWith('turn:'))));
  } finally { await app.close(); }
});

test('authenticated controller routes camera capabilities and commands only inside its session', async () => {
  const app = createCameraServer({ port: 0, host: '127.0.0.1', publicBaseUrl: 'https://iphone-camera.test', tokenSecret: 'test-token-secret-123456', turnSecret: 'test-turn-secret' });
  await app.listen();
  const port = app.server.address().port;
  const base = `http://127.0.0.1:${port}`;
  const connect = token => new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}/camera-ws?token=${encodeURIComponent(token)}`);
    ws.addEventListener('message', event => { if (JSON.parse(event.data).type === 'hello') resolve(ws); });
    ws.addEventListener('error', reject);
  });
  const next = (ws, type) => new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error(`timeout waiting for ${type}`)), 1000);
    ws.addEventListener('message', event => { const message = JSON.parse(event.data); if (message.type === type) { clearTimeout(timeout); resolve(message); } });
  });
  try {
    const created = await fetch(`${base}/api/camera/sessions`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ client_version: '1.0.6' }) }).then(response => response.json());
    assert.ok(created.control_token);
    const pairToken = new URL(created.pairing_url).pathname.split('/').pop();
    const paired = await fetch(`${base}/api/camera/pair`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pair_token: pairToken }) }).then(response => response.json());
    const controller = await connect(created.control_token);
    const sender = await connect(paired.socket_token);
    const capabilities = next(controller, 'camera-capabilities');
    sender.send(JSON.stringify({ type: 'camera-capabilities', payload: { controls: { zoom: { min: 1, max: 3, value: 1 } } } }));
    assert.equal((await capabilities).payload.controls.zoom.max, 3);
    const command = next(sender, 'camera-command');
    controller.send(JSON.stringify({ type: 'camera-command', payload: { id: '1', control: 'frameRate', value: 60 } }));
    assert.deepEqual((await command).payload, { id: '1', control: 'frameRate', value: 60 });
    sender.close(); controller.close();
  } finally { await app.close(); }
});
