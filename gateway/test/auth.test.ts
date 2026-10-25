import { describe as suite, expect, test } from 'vitest';

import { checkLogin, createSession, createUser, endSession, roleAllows, sessionUser, userCount } from '../server/auth.ts';
import { openDb } from '../server/db.ts';

suite('accounts and sessions', () => {
  test('passwords are checked, never stored in the clear', () => {
    const db = openDb(':memory:');
    createUser(db, 'warden', 'correct horse battery', 'operator');
    expect(userCount(db)).toBe(1);
    expect(checkLogin(db, 'warden', 'correct horse battery')).toBe('operator');
    expect(checkLogin(db, 'warden', 'correct horse batterY')).toBeNull();
    expect(checkLogin(db, 'nobody', 'correct horse battery')).toBeNull();
    const row = db.prepare('SELECT hash FROM users').get() as { hash: string };
    expect(row.hash).not.toContain('horse');
  });

  test('weak or malformed accounts are refused', () => {
    const db = openDb(':memory:');
    expect(() => createUser(db, 'admin', 'short', 'admin')).toThrow(/12 characters/);
    expect(() => createUser(db, 'Bad Name', 'long enough password', 'admin')).toThrow(/username/);
  });

  test('sessions expire and can be ended', () => {
    const db = openDb(':memory:');
    createUser(db, 'viewer1', 'long enough password', 'viewer');
    const t0 = 1_800_000_000_000;
    const token = createSession(db, 'viewer1', t0);
    expect(sessionUser(db, token, t0 + 1000)).toEqual({ username: 'viewer1', role: 'viewer' });
    expect(sessionUser(db, token, t0 + 13 * 3600 * 1000)).toBeNull();
    const second = createSession(db, 'viewer1', t0);
    endSession(db, second);
    expect(sessionUser(db, second, t0 + 1000)).toBeNull();
    expect(sessionUser(db, undefined)).toBeNull();
  });

  test('roles are ordered', () => {
    expect(roleAllows('admin', 'operator')).toBe(true);
    expect(roleAllows('operator', 'operator')).toBe(true);
    expect(roleAllows('viewer', 'operator')).toBe(false);
  });
});
