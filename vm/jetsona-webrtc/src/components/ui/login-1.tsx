'use client';

// Sign-in card, adapted from the shadcnblocks signup-1 block.
//
// Trimmed to a single credential form: no account creation, no OAuth provider
// and no "already have an account" footer — this console has exactly one
// operator account, provisioned server-side via AUTH_USER / AUTH_PASS.
// Presentational only; the parent owns submission and error state.

import type { LucideIcon } from 'lucide-react';
import { AlertCircle, Loader2, MonitorSmartphone } from 'lucide-react';

import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';

interface Login1Props {
  heading?: string;
  subheading?: string;
  logo?: { icon: LucideIcon; title: string };
  loginText?: string;
  usernameLabel?: string;
  passwordLabel?: string;
  error?: string | null;
  pending?: boolean;
  onSubmit: (credentials: { username: string; password: string }) => void;
}

const Login1 = ({
  heading = 'Đăng nhập',
  subheading = 'Truy cập bảng điều khiển từ xa.',
  logo = { icon: MonitorSmartphone, title: 'Jetsona Remote' },
  loginText = 'Đăng nhập',
  usernameLabel = 'Tài khoản',
  passwordLabel = 'Mật khẩu',
  error,
  pending = false,
  onSubmit,
}: Login1Props) => {
  const LogoIcon = logo.icon;

  return (
    <section className="grid h-dvh place-items-center px-4">
      <form
        onSubmit={(event) => {
          event.preventDefault();
          const data = new FormData(event.currentTarget);
          onSubmit({
            username: String(data.get('username') ?? ''),
            password: String(data.get('password') ?? ''),
          });
        }}
        className="animate-card-in flex w-full max-w-sm flex-col items-center gap-y-8 rounded-2xl border border-white/10 bg-card/80 px-6 py-12 shadow-2xl shadow-black/50 backdrop-blur-2xl"
      >
        <div className="flex flex-col items-center gap-y-3 text-center">
          <div className="grid size-12 place-items-center rounded-2xl bg-primary/15 ring-1 ring-primary/30">
            <LogoIcon className="size-6 text-primary" aria-hidden />
          </div>
          <div className="space-y-1">
            <p className="text-sm font-medium text-muted-foreground">{logo.title}</p>
            <h1 className="text-2xl font-semibold text-foreground">{heading}</h1>
            <p className="text-sm text-muted-foreground">{subheading}</p>
          </div>
        </div>

        <div className="flex w-full flex-col gap-4">
          <div className="flex flex-col gap-2">
            <label htmlFor="username" className="text-sm font-medium text-foreground">
              {usernameLabel}
            </label>
            <Input
              id="username"
              name="username"
              type="text"
              autoComplete="username"
              autoFocus
              required
              disabled={pending}
            />
          </div>
          <div className="flex flex-col gap-2">
            <label htmlFor="password" className="text-sm font-medium text-foreground">
              {passwordLabel}
            </label>
            <Input
              id="password"
              name="password"
              type="password"
              autoComplete="current-password"
              required
              disabled={pending}
            />
          </div>

          {error && (
            <p
              role="alert"
              className="flex items-start gap-2 rounded-md border border-destructive/30 bg-destructive/10 px-3 py-2 text-sm text-destructive"
            >
              <AlertCircle className="mt-0.5 size-4 shrink-0" aria-hidden />
              <span>{error}</span>
            </p>
          )}

          <Button type="submit" className="mt-2 w-full" disabled={pending}>
            {pending && <Loader2 className="mr-2 size-4 animate-spin" aria-hidden />}
            {pending ? 'Đang đăng nhập…' : loginText}
          </Button>
        </div>
      </form>
    </section>
  );
};

export { Login1 };
