import crypto from 'node:crypto';

function frame(opcode, payload) {
  const body = Buffer.from(payload);
  let head;
  if (body.length < 126) head = Buffer.from([0x80 | opcode, body.length]);
  else if (body.length < 65536) { head = Buffer.alloc(4); head[0] = 0x80 | opcode; head[1] = 126; head.writeUInt16BE(body.length, 2); }
  else { head = Buffer.alloc(10); head[0] = 0x80 | opcode; head[1] = 127; head.writeBigUInt64BE(BigInt(body.length), 2); }
  return Buffer.concat([head, body]);
}

export function acceptWebSocket(req, socket, head, onConnection) {
  const key = req.headers['sec-websocket-key'];
  if (!key || req.headers.upgrade?.toLowerCase() !== 'websocket') { socket.destroy(); return; }
  const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n');
  let pending = Buffer.alloc(0);
  const client = {
    send(value) { if (!socket.destroyed) socket.write(frame(1, JSON.stringify(value))); },
    close(code = 1000, reason = '') { const b = Buffer.alloc(2 + Buffer.byteLength(reason)); b.writeUInt16BE(code); b.write(reason, 2); socket.end(frame(8, b)); },
    onMessage: null, onClose: null
  };
  socket.on('data', chunk => {
    pending = Buffer.concat([pending, chunk]);
    while (pending.length >= 2) {
      const opcode = pending[0] & 0x0f; let len = pending[1] & 0x7f; const masked = !!(pending[1] & 0x80); let offset = 2;
      if (!masked) { client.close(1002, 'mask required'); return; }
      if (len === 126) { if (pending.length < 4) return; len = pending.readUInt16BE(2); offset = 4; }
      else if (len === 127) { if (pending.length < 10) return; const n = pending.readBigUInt64BE(2); if (n > 1048576n) { client.close(1009); return; } len = Number(n); offset = 10; }
      if (pending.length < offset + 4 + len) return;
      const mask = pending.subarray(offset, offset + 4); offset += 4;
      const body = Buffer.from(pending.subarray(offset, offset + len)); pending = pending.subarray(offset + len);
      for (let i = 0; i < body.length; i++) body[i] ^= mask[i % 4];
      if (opcode === 8) { socket.end(frame(8, body)); return; }
      if (opcode === 9) { socket.write(frame(10, body)); continue; }
      if (opcode !== 1 || body.length > 1048576) { client.close(1003); return; }
      try { client.onMessage?.(JSON.parse(body.toString('utf8'))); } catch { client.send({ type: 'error', code: 'invalid_json' }); }
    }
  });
  socket.on('close', () => client.onClose?.());
  socket.on('error', () => client.onClose?.());
  if (head?.length) socket.emit('data', head);
  onConnection(client);
}

