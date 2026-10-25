import { useState, type FormEvent } from 'react';

import { api, type Me } from '../api.ts';

export function SignIn({ onSignedIn }: { onSignedIn: (me: Me) => void }) {
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    setBusy(true);
    setError(null);
    try {
      onSignedIn(await api<Me>('/api/login', { method: 'POST', body: { username, password } }));
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="signin">
      <form className="panel" onSubmit={submit}>
        <h1 style={{ padding: '0.9rem', borderBottom: '1.5px solid var(--ink)', fontSize: 'var(--t-h3)' }}>FireX site gateway</h1>
        <div className="panel-body form">
          <p className="muted">Sign in with the account your site administrator created for you.</p>
          <label className="field">
            Username
            <input autoComplete="username" value={username} onChange={(e) => setUsername(e.target.value)} required />
          </label>
          <label className="field">
            Password
            <input type="password" autoComplete="current-password" value={password} onChange={(e) => setPassword(e.target.value)} required />
          </label>
          {error && <p className="error" role="alert">{error}</p>}
          <div>
            <button className="button" type="submit" disabled={busy}>
              {busy ? 'Signing in' : 'Sign in'}
            </button>
          </div>
        </div>
      </form>
    </div>
  );
}
