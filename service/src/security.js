import crypto from 'node:crypto';

const b64u = value => Buffer.from(value).toString('base64url');
const fromB64u = value => Buffer.from(value, 'base64url');

export function hashPassword(password) {
  const salt = crypto.randomBytes(16);
  const derived = crypto.scryptSync(password, salt, 32, { N: 16384, r: 8, p: 1 });
  return `scrypt$16384$8$1$${b64u(salt)}$${b64u(derived)}`;
}

export function verifyPassword(password, encoded) {
  try {
    const [kind, n, r, p, salt, expected] = encoded.split('$');
    if (kind !== 'scrypt') return false;
    const actual = crypto.scryptSync(password, fromB64u(salt), fromB64u(expected).length, {
      N: Number(n), r: Number(r), p: Number(p)
    });
    return crypto.timingSafeEqual(actual, fromB64u(expected));
  } catch { return false; }
}

export function signToken(payload, secret, ttlSeconds) {
  const header = b64u(JSON.stringify({ alg: 'HS256', typ: 'JWT' }));
  const now = Math.floor(Date.now() / 1000);
  const body = b64u(JSON.stringify({ ...payload, iat: now, exp: now + ttlSeconds }));
  const signature = crypto.createHmac('sha256', secret).update(`${header}.${body}`).digest('base64url');
  return `${header}.${body}.${signature}`;
}

export function verifyToken(token, secret, expectedType) {
  if (typeof token !== 'string') throw new Error('missing token');
  const parts = token.split('.');
  if (parts.length !== 3) throw new Error('invalid token');
  const expected = crypto.createHmac('sha256', secret).update(`${parts[0]}.${parts[1]}`).digest();
  const actual = fromB64u(parts[2]);
  if (actual.length !== expected.length || !crypto.timingSafeEqual(actual, expected)) throw new Error('invalid token');
  const payload = JSON.parse(fromB64u(parts[1]).toString('utf8'));
  if (payload.exp <= Math.floor(Date.now() / 1000)) throw new Error('expired token');
  if (expectedType && payload.type !== expectedType) throw new Error('wrong token type');
  return payload;
}

export function turnCredentials(userId, secret, ttlSeconds) {
  const username = `${Math.floor(Date.now() / 1000) + ttlSeconds}:${userId}`;
  const credential = crypto.createHmac('sha1', secret).update(username).digest('base64');
  return { username, credential, ttl_seconds: ttlSeconds };
}

export const randomId = () => crypto.randomUUID();
export const inviteCode = () => crypto.randomBytes(5).toString('base64url').replace(/[-_]/g, '').slice(0, 6).toUpperCase();

