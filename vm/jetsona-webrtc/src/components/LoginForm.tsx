'use client';

// Submission wrapper for the sign-in card. The session cookie is set by the
// server on POST /api/auth/login (handled in server.mjs, ahead of Next), so a
// full navigation is used afterwards rather than a client-side route change —
// the cookie must be attached to the document request for the console to load.

import { useState } from 'react';
import { Login1 } from '@/components/ui/login-1';

/** Only allow same-site relative paths back, so ?next= cannot become an open redirect. */
function safeNext(raw: string | null): string {
  if (!raw) return '/';
  if (!raw.startsWith('/') || raw.startsWith('//')) return '/';
  return raw;
}

export default function LoginForm() {
  const [error, setError] = useState<string | null>(null);
  const [pending, setPending] = useState(false);

  const onSubmit = async ({ username, password }: { username: string; password: string }) => {
    if (pending) return;
    setPending(true);
    setError(null);
    try {
      const res = await fetch('/api/auth/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username, password }),
      });
      const data = await res.json().catch(() => null);
      if (!res.ok || !data?.ok) {
        setError(data?.error || `Đăng nhập thất bại (${res.status}).`);
        setPending(false);
        return;
      }
      const params = new URLSearchParams(window.location.search);
      window.location.replace(safeNext(params.get('next')));
    } catch (err) {
      setError((err as Error).message);
      setPending(false);
    }
  };

  return <Login1 error={error} pending={pending} onSubmit={onSubmit} />;
}
