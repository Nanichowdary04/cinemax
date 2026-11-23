import { useState, type ReactNode } from 'react';
import type { AppState, EngineInfo } from '../bridge/types';
import type { ConnectionStatus, ToastNotice } from '../bridge/store';
import { Button } from './Controls';
import { IconClose, IconInfo, IconRefresh, IconShield, IconWarning } from './Icons';
import { LogoMark } from './Logo';

export function ScreenHeader({ eyebrow, title, children }: { eyebrow?: string; title: string; children?: ReactNode }) {
  return (
    <header className="screen-head">
      <div>
        {eyebrow && <div className="eyebrow">{eyebrow}</div>}
        <h1 className="screen-title">{title}</h1>
      </div>
      {children && <div className="screen-head__actions">{children}</div>}
    </header>
  );
}

export function engineStatusText(e: EngineInfo): { label: string; tone: 'on' | 'off' | 'warn' | 'busy' } {
  switch (e.status) {
    case 'running':
      return { label: e.bypass ? 'Running · bypassed' : 'Processing', tone: e.bypass ? 'warn' : 'on' };
    case 'starting':
      return { label: 'Starting…', tone: 'busy' };
    case 'no-virtual-device':
      return { label: 'Virtual device needed', tone: 'warn' };
    case 'no-output-device':
      return { label: 'No output device', tone: 'warn' };
    case 'error':
      return { label: 'Engine error', tone: 'warn' };
    case 'passthrough':
      return { label: 'TV decodes Dolby · CINEMAX resting', tone: 'busy' };
    default:
      return { label: 'Off · Windows audio untouched', tone: 'off' };
  }
}

// ---------------------------------------------------------------------------
export function Toasts({ notices, onDismiss }: { notices: ToastNotice[]; onDismiss: (id: number) => void }) {
  return (
    <div className="toasts" role="status" aria-live="polite">
      {notices.map((n) => (
        <div key={n.id} className={`toast toast--${n.level}`}>
          <span className="toast__icon">{n.level === 'info' ? <IconInfo size={18} /> : <IconWarning size={18} />}</span>
          <span className="toast__msg">{n.message}</span>
          <button type="button" className="toast__close" aria-label="Dismiss" onClick={() => onDismiss(n.id)}>
            <IconClose size={14} />
          </button>
        </div>
      ))}
    </div>
  );
}

// ---------------------------------------------------------------------------
export function StatusBanner({
  state,
  onGoDevices,
  onRetry,
  onRestore,
}: {
  state: AppState;
  onGoDevices: () => void;
  onRetry: () => void;
  onRestore: () => void;
}) {
  const e = state.engine;
  const problem = e.status === 'error' || e.status === 'no-virtual-device' || e.status === 'no-output-device';
  if (!problem) return null;

  let title = 'The audio engine stopped';
  let action: ReactNode = (
    <>
      <Button variant="ghost" icon={<IconRefresh size={16} />} onClick={onRetry}>
        Try again
      </Button>
      <Button variant="ghost" icon={<IconShield size={16} />} onClick={onRestore}>
        Restore audio routing
      </Button>
    </>
  );
  if (e.status === 'no-virtual-device') {
    title = 'A virtual audio device is required for system-wide processing';
    action = (
      <Button variant="primary" onClick={onGoDevices}>
        How to set it up
      </Button>
    );
  } else if (e.status === 'no-output-device') {
    title = 'No usable output device is connected';
    action = (
      <Button variant="ghost" onClick={onGoDevices}>
        Choose a device
      </Button>
    );
  }

  return (
    <div className={`banner${e.status === 'error' ? ' banner--error' : ''}`} role="alert">
      <span className="banner__icon">
        <IconWarning size={18} />
      </span>
      <div className="banner__text">
        <div className="banner__title">{title}</div>
        {e.message && <div className="banner__msg">{e.message}</div>}
      </div>
      <div className="banner__actions">{action}</div>
    </div>
  );
}

// ---------------------------------------------------------------------------
export function NotConnected({ connection, error, onRetry }: { connection: ConnectionStatus; error: string | null; onRetry: () => void }) {
  const connecting = connection === 'connecting';
  return (
    <div className="empty">
      <div className={`empty__mark${connecting ? ' is-pulsing' : ''}`}>
        <LogoMark size={56} />
      </div>
      {connection === 'disconnected' && (
        <>
          <h1 className="empty__title">Engine not connected</h1>
          <p className="empty__text">
            This interface talks to the CINEMAX audio engine running in the desktop app. It was opened outside the app, so there is no
            engine, device or audio information to show.
          </p>
          <p className="empty__hint">Start CINEMAX from the Start menu or the system tray to use it.</p>
        </>
      )}
      {connecting && (
        <>
          <h1 className="empty__title">Connecting to the audio engine…</h1>
          <p className="empty__text">Reading devices and your saved profile.</p>
        </>
      )}
      {connection === 'failed' && (
        <>
          <h1 className="empty__title">The engine did not respond</h1>
          <p className="empty__text">{error ?? 'No response from the CINEMAX host.'}</p>
          <div className="empty__actions">
            <Button variant="primary" icon={<IconRefresh size={16} />} onClick={onRetry}>
              Retry
            </Button>
          </div>
        </>
      )}
    </div>
  );
}

// ---------------------------------------------------------------------------
/** Two-step button for destructive / disruptive actions. */
export function ConfirmButton({
  children,
  confirmLabel,
  onConfirm,
  variant = 'ghost',
  icon,
  disabled,
}: {
  children: ReactNode;
  confirmLabel: string;
  onConfirm: () => void;
  variant?: 'ghost' | 'danger' | 'primary' | 'quiet';
  icon?: ReactNode;
  disabled?: boolean;
}) {
  const [armed, setArmed] = useState(false);
  if (!armed)
    return (
      <Button variant={variant} icon={icon} onClick={() => setArmed(true)} disabled={disabled}>
        {children}
      </Button>
    );
  return (
    <span className="confirm">
      <Button
        variant="danger"
        onClick={() => {
          setArmed(false);
          onConfirm();
        }}
      >
        {confirmLabel}
      </Button>
      <Button variant="quiet" onClick={() => setArmed(false)}>
        Cancel
      </Button>
    </span>
  );
}
