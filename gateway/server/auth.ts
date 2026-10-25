/**
 * Local accounts and sessions. No default or universal passwords (TEC 31318):
 * the first admin is created on the command line, and every password is at
 * least 12 characters, stored as scrypt.
 *
 *   viewer    sees everything
 *   operator  also labels incidents and adds pairing codes
 *   admin     also manages users
 */
import { createHash, randomBytes, scryptSync, timingSafeEqual } from 'node:crypto';

import type { Db, Row } from './db.ts';

export type Role = 'viewer' | 'operator' | 'admin';
export const ROLES: Role[] = ['viewer', 'operator', 'admin'];
const RANK: Record<Role, number> = { viewer: 0, operator: 1, admin: 2 };
const SESSION_MS = 12 * 3600 * 1000;
const SCRYPT = { N: 16384, r: 8, p: 1 };

export function roleAllows(role: Role, needed: Role): boolean {
  return RANK[role] >= RANK[needed];
}

function hashPassword(password: string, salt: Buffer): Buffer {
  return scryptSync(password, salt, 32, SCRYPT);
}

export function createUser(db: Db, username: string, password: string, role: Role, now = Date.now()): void {
  if (!/^[a-z0-9._-]{3,32}$/.test(username)) throw new Error('username: 3-32 of a-z 0-9 . _ -');
  if (password.length < 12) throw new Error('password must be at least 12 characters');
  if (!ROLES.includes(role)) throw new Error('unknown role');
  const salt = randomBytes(16);
  db.prepare('INSERT INTO users (username, role, salt, hash, created_at) VALUES (?, ?, ?, ?, ?)')
    .run(username, role, salt.toString('hex'), hashPassword(password, salt).toString('hex'), now);
}

export function userCount(db: Db): number {
  return Number((db.prepare('SELECT COUNT(*) AS n FROM users').get() as Row).n);
}

export function checkLogin(db: Db, username: string, password: string): Role | null {
  const row = db.prepare('SELECT role, salt, hash FROM users WHERE username = ?').get(username) as Row | undefined;
  /* Hash even for an unknown user, so response time does not reveal which names exist. */
  const salt = row ? Buffer.from(String(row.salt), 'hex') : randomBytes(16);
  const actual = hashPassword(password, salt);
  const expected = row ? Buffer.from(String(row.hash), 'hex') : randomBytes(32);
  return row && timingSafeEqual(actual, expected) ? (row.role as Role) : null;
}

const tokenHash = (token: string) => createHash('sha256').update(token).digest('hex');

export function createSession(db: Db, username: string, now = Date.now()): string {
  const token = randomBytes(32).toString('base64url');
  db.prepare('DELETE FROM sessions WHERE expires_at < ?').run(now);
  db.prepare('INSERT INTO sessions (token_hash, username, created_at, expires_at) VALUES (?, ?, ?, ?)')
    .run(tokenHash(token), username, now, now + SESSION_MS);
  return token;
}

export function sessionUser(db: Db, token: string | undefined, now = Date.now()): { username: string; role: Role } | null {
  if (!token) return null;
  const row = db.prepare(
    'SELECT u.username, u.role FROM sessions s JOIN users u ON u.username = s.username WHERE s.token_hash = ? AND s.expires_at > ?',
  ).get(tokenHash(token), now) as Row | undefined;
  return row ? { username: String(row.username), role: row.role as Role } : null;
}

export function endSession(db: Db, token: string | undefined): void {
  if (token) db.prepare('DELETE FROM sessions WHERE token_hash = ?').run(tokenHash(token));
}
