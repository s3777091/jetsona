'use client';

// Minimal toast stack. Deliberately dependency-free: the console only ever
// raises a handful of transient errors, so a full toast library (and its
// portal/animation machinery) would outweigh the feature.

import { useCallback, useRef, useState } from 'react';
import { AlertTriangle, CheckCircle2, Info, X } from 'lucide-react';
import { cn } from '@/lib/utils';

export type ToastTone = 'error' | 'success' | 'info';

export type Toast = {
  id: number;
  tone: ToastTone;
  title: string;
  detail?: string;
};

const TONE_STYLE: Record<ToastTone, { icon: typeof Info; accent: string }> = {
  error: { icon: AlertTriangle, accent: 'text-state-offline' },
  success: { icon: CheckCircle2, accent: 'text-state-online' },
  info: { icon: Info, accent: 'text-white/70' },
};

/** Owns the toast stack and auto-dismisses each entry after `ttl` ms. */
export function useToasts(ttl = 6000) {
  const [toasts, setToasts] = useState<Toast[]>([]);
  const nextId = useRef(1);

  const dismiss = useCallback((id: number) => {
    setToasts((current) => current.filter((toast) => toast.id !== id));
  }, []);

  const push = useCallback(
    (tone: ToastTone, title: string, detail?: string) => {
      const id = nextId.current++;
      setToasts((current) => [...current.slice(-2), { id, tone, title, detail }]);
      setTimeout(() => dismiss(id), ttl);
      return id;
    },
    [dismiss, ttl],
  );

  return { toasts, push, dismiss };
}

export function Toaster({
  toasts,
  onDismiss,
}: {
  toasts: Toast[];
  onDismiss: (id: number) => void;
}) {
  if (!toasts.length) return null;
  return (
    <div className="pointer-events-none fixed bottom-6 right-6 z-50 flex w-[min(22rem,calc(100vw-3rem))] flex-col gap-2">
      {toasts.map((toast) => {
        const { icon: Icon, accent } = TONE_STYLE[toast.tone];
        return (
          <div
            key={toast.id}
            role="status"
            className="animate-toast-in pointer-events-auto flex items-start gap-3 rounded-2xl border border-white/10 bg-black/70 p-3.5 text-sm text-white shadow-2xl shadow-black/50 backdrop-blur-xl"
          >
            <Icon className={cn('mt-0.5 size-4 shrink-0', accent)} aria-hidden />
            <div className="min-w-0 flex-1">
              <p className="font-medium leading-snug">{toast.title}</p>
              {toast.detail && (
                <p className="mt-0.5 break-words text-xs leading-snug text-white/55">
                  {toast.detail}
                </p>
              )}
            </div>
            <button
              type="button"
              onClick={() => onDismiss(toast.id)}
              aria-label="Đóng thông báo"
              className="-m-1 rounded-lg p-1 text-white/40 transition-colors hover:bg-white/10 hover:text-white"
            >
              <X className="size-3.5" aria-hidden />
            </button>
          </div>
        );
      })}
    </div>
  );
}
