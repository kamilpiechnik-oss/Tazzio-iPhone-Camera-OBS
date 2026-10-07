import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { acceptWebSocket } from './websocket.js';
import { randomId, signToken, turnCredentials, verifyToken } from './security.js';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const frontend = path.join(root, 'frontend');
const downloads = path.join(frontend, 'downloads');
const json = (res, status, value) => { const body = JSON.stringify(value); res.writeHead(status, { 'content-type': 'application/json; charset=utf-8', 'content-length': Buffer.byteLength(body), 'cache-control': 'no-store' }); res.end(body); };
const fail = (res, status, code, message) => json(res, status, { error: code, message });
const readBody = req => new Promise((resolve, reject) => { let body = ''; req.on('data', chunk => { body += chunk; if (body.length > 32768) reject(new Error('body_too_large')); }); req.on('end', () => { try { resolve(body ? JSON.parse(body) : {}); } catch { reject(new Error('invalid_json')); } }); req.on('error', reject); });
const contentType = file => file.endsWith('.html') ? 'text/html; charset=utf-8' : file.endsWith('.js') ? 'text/javascript; charset=utf-8' : file.endsWith('.css') ? 'text/css; charset=utf-8' : 'application/octet-stream';

export function createCameraServer(config = {}) {
  const cfg = {
    port: Number(config.port ?? 8790),
    host: config.host ?? '127.0.0.1',
    publicBaseUrl: String(config.publicBaseUrl ?? 'https://iphone-camera.tazzio.pl').replace(/\/$/, ''),
    tokenSecret: config.tokenSecret ?? 'development-only-change-me',
    turnSecret: config.turnSecret ?? '',
    turnHost: config.turnHost ?? '185.238.72.154',
    turnTtl: Number(config.turnTtl ?? 3600),
    pairTtl: Number(config.pairTtl ?? 120),
    sessionTtl: Number(config.sessionTtl ?? 28800),
  };
  const sessions = new Map();
  const sockets = new Map();
  let closing = false;

  const iceServers = id => {
    const credential = turnCredentials(`camera-${id}`, cfg.turnSecret, cfg.turnTtl);
    return [
      { urls: [`stun:${cfg.turnHost}:3478`] },
      { urls: [`turn:${cfg.turnHost}:3478?transport=udp`, `turn:${cfg.turnHost}:3478?transport=tcp`], username: credential.username, credential: credential.credential },
    ];
  };
  const getSession = id => {
    const session = sessions.get(id);
    if (!session || session.expiresAt <= Date.now()) { sessions.delete(id); sockets.get(id)?.receiver?.close(1000, 'expired'); sockets.get(id)?.sender?.close(1000, 'expired'); sockets.delete(id); return null; }
    return session;
  };
  const headers = {
    'x-content-type-options': 'nosniff',
    'referrer-policy': 'no-referrer',
    'permissions-policy': 'camera=(self), microphone=(self)',
    'content-security-policy': "default-src 'self'; connect-src 'self' wss:; img-src 'self' data:; media-src 'self' blob:; style-src 'self'; script-src 'self'; base-uri 'none'; frame-ancestors 'none'",
  };

  const server = http.createServer(async (req, res) => {
    Object.entries(headers).forEach(([key, value]) => res.setHeader(key, value));
    const url = new URL(req.url, 'http://localhost');
    try {
      if (req.method === 'GET' && url.pathname === '/api/health') return json(res, 200, { status: 'ok', product: 'tazzio-iphone-camera', version: '1.0.0' });
      if (req.method === 'POST' && url.pathname === '/api/camera/sessions') {
        if (!cfg.turnSecret) return fail(res, 503, 'turn_unconfigured', 'TURN nie jest skonfigurowany.');
        const id = randomId();
        sessions.set(id, { expiresAt: Date.now() + cfg.sessionTtl * 1000, paired: false });
        const pairToken = signToken({ type: 'camera-pair', sub: id }, cfg.tokenSecret, cfg.pairTtl);
        const socketToken = signToken({ type: 'camera-socket', sub: id, role: 'receiver' }, cfg.tokenSecret, cfg.sessionTtl);
        return json(res, 201, { pairing_url: `${cfg.publicBaseUrl}/camera/${encodeURIComponent(pairToken)}`, socket_token: socketToken, pair_expires_in: cfg.pairTtl, session_expires_in: cfg.sessionTtl, ice_servers: iceServers(id) });
      }
      if (req.method === 'POST' && url.pathname === '/api/camera/pair') {
        const body = await readBody(req);
        let token;
        try { token = verifyToken(body.pair_token, cfg.tokenSecret, 'camera-pair'); } catch { return fail(res, 401, 'invalid_pair_token', 'Kod QR jest nieprawidłowy albo wygasł.'); }
        const session = getSession(token.sub);
        if (!session || session.paired) return fail(res, 410, 'pair_unavailable', 'Kod QR wygasł albo został już użyty.');
        session.paired = true;
        const socketToken = signToken({ type: 'camera-socket', sub: token.sub, role: 'sender' }, cfg.tokenSecret, cfg.sessionTtl);
        return json(res, 200, { socket_token: socketToken, session_expires_in: cfg.sessionTtl, ice_servers: iceServers(token.sub) });
      }
      const download = url.pathname.match(/^\/downloads\/(Tazzio-iPhone-Camera-(?:1\.0\.0-windows-x64\.zip|Setup-1\.0\.0\.exe))$/);
      if (req.method === 'GET' && download) {
        const filePath = path.join(downloads, download[1]);
        if (!fs.existsSync(filePath)) return fail(res, 404, 'not_found', 'Plik nie jest jeszcze dostępny.');
        const stat = fs.statSync(filePath);
        res.writeHead(200, { 'content-type': filePath.endsWith('.exe') ? 'application/vnd.microsoft.portable-executable' : 'application/zip', 'content-length': stat.size, 'content-disposition': `attachment; filename="${download[1]}"`, 'cache-control': 'public, max-age=3600' });
        return fs.createReadStream(filePath).pipe(res);
      }
      let file = url.pathname === '/' ? 'index.html' : url.pathname.slice(1);
      if (/^camera\/[^/]+$/.test(file)) file = 'camera.html';
      if (!['index.html', 'styles.css', 'camera.html', 'camera.css', 'camera.js'].includes(file)) return fail(res, 404, 'not_found', 'Nie znaleziono strony.');
      const filePath = path.join(frontend, file);
      if (!fs.existsSync(filePath)) return fail(res, 404, 'not_found', 'Nie znaleziono strony.');
      const stat = fs.statSync(filePath);
      res.writeHead(200, { 'content-type': contentType(file), 'content-length': stat.size, 'cache-control': file.endsWith('.html') ? 'no-store' : 'public, max-age=300' });
      return fs.createReadStream(filePath).pipe(res);
    } catch (error) {
      console.error(JSON.stringify({ level: 'error', event: 'request_failed', message: error.message }));
      if (!res.headersSent) fail(res, error.message === 'body_too_large' ? 413 : 400, 'bad_request', 'Nieprawidłowe żądanie.');
      else res.end();
    }
  });

  server.on('upgrade', (req, socket, head) => {
    const url = new URL(req.url, 'http://localhost');
    if (url.pathname !== '/camera-ws') return socket.destroy();
    let camera;
    try { camera = verifyToken(url.searchParams.get('token'), cfg.tokenSecret, 'camera-socket'); } catch { return socket.destroy(); }
    if (!['sender', 'receiver'].includes(camera.role) || !getSession(camera.sub)) return socket.destroy();
    acceptWebSocket(req, socket, head, ws => {
      const peers = sockets.get(camera.sub) ?? {};
      peers[camera.role]?.close(1000, 'replaced');
      peers[camera.role] = ws;
      sockets.set(camera.sub, peers);
      const notify = () => { peers.receiver?.send({ type: 'peer', ready: Boolean(peers.sender) }); peers.sender?.send({ type: 'peer', ready: Boolean(peers.receiver) }); };
      ws.onMessage = message => {
        const allowed = camera.role === 'sender' ? ['offer', 'ice-candidate'] : ['answer', 'ice-candidate'];
        if (!allowed.includes(message.type)) return ws.send({ type: 'error', code: 'invalid_camera_message' });
        const other = camera.role === 'sender' ? peers.receiver : peers.sender;
        other?.send({ type: message.type, payload: message.payload });
      };
      ws.onClose = () => { if (peers[camera.role] === ws) delete peers[camera.role]; if (!peers.sender && !peers.receiver) sockets.delete(camera.sub); else notify(); };
      ws.send({ type: 'hello', role: camera.role });
      notify();
    });
  });

  return {
    server,
    listen: () => new Promise(resolve => server.listen(cfg.port, cfg.host, resolve)),
    close: () => new Promise(resolve => { closing = true; for (const peers of sockets.values()) { peers.sender?.close(1001, 'shutdown'); peers.receiver?.close(1001, 'shutdown'); } sockets.clear(); sessions.clear(); server.close(resolve); }),
  };
}
