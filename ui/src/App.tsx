import { useEffect, useState, type ComponentType } from 'react';
import { startStore } from './bridge/store';
import { actions as cinemaxActions, useCinemax, useVisualizer } from './bridge/useCinemax';
import type { AppState, DetectedContent, ListeningMode } from './bridge/types';
import { MasterSwitch } from './components/Controls';
import { NotConnected, StatusBanner, Toasts, engineStatusText } from './components/Chrome';
import {
  DeviceIcon,
  IconDashboard,
  IconDevices,
  IconEffects,
  IconEqualizer,
  IconPicture,
  IconTv,
  IconPresets,
  IconSettings,
} from './components/Icons';
import { LogoMark, Wordmark } from './components/Logo';
import { fmtRate } from './lib/format';
import { Dashboard } from './screens/Dashboard';
import { Devices } from './screens/Devices';
import { Effects } from './screens/Effects';
import { Equalizer } from './screens/Equalizer';
import { Picture } from './screens/Picture';
import { Presets } from './screens/Presets';
import { SettingsScreen } from './screens/Settings';
import { TvScreen } from './screens/Tv';

export type ScreenId = 'dashboard' | 'equalizer' | 'effects' | 'devices' | 'presets' | 'picture' | 'tv' | 'settings';

const NAV: { id: ScreenId; label: string; Icon: ComponentType<{ size?: number }> }[] = [
  { id: 'dashboard', label: 'Dashboard', Icon: IconDashboard },
  { id: 'equalizer', label: 'Equalizer', Icon: IconEqualizer },
  { id: 'effects', label: 'Sound Effects', Icon: IconEffects },
  { id: 'devices', label: 'Audio Devices', Icon: IconDevices },
  { id: 'presets', label: 'Presets', Icon: IconPresets },
  { id: 'picture', label: 'Picture', Icon: IconPicture },
  { id: 'tv', label: 'TV', Icon: IconTv },
  { id: 'settings', label: 'Settings', Icon: IconSettings },
];

/** Screens that show live engine visualisation (meters streamed only then). */
const LIVE_SCREENS: ScreenId[] = ['dashboard', 'equalizer'];

export default function App() {
  const { connected, connection, error, state, notices, busy, actions } = useCinemax();
  const [screen, setScreen] = useState<ScreenId>('dashboard');

  useEffect(() => {
    startStore();
  }, []);

  useVisualizer(connection === 'ready' && LIVE_SCREENS.includes(screen));

  const ready = connection === 'ready' && state !== null;

  return (
    <div className="app">
      <nav className="rail" aria-label="Main">
        <div className="brand">
          <LogoMark size={30} />
          <Wordmark />
        </div>
        <ul className="nav">
          {NAV.map(({ id, label, Icon }) => (
            <li key={id}>
              <button
                type="button"
                className={`nav__item${screen === id ? ' is-active' : ''}`}
                aria-current={screen === id ? 'page' : undefined}
                onClick={() => setScreen(id)}
                title={label}
                disabled={!ready}
              >
                <Icon size={20} />
                <span className="nav__label">{label}</span>
              </button>
            </li>
          ))}
        </ul>
        <div className="rail__foot">
          <span className={`conn-dot${connected && connection === 'ready' ? ' is-on' : ''}`} />
          <span className="rail__foot-text">
            {connected ? (connection === 'ready' ? 'Engine connected' : connection === 'failed' ? 'Engine not responding' : 'Connecting…') : 'Not connected'}
            {state?.version && <span className="rail__version">v{state.version}</span>}
          </span>
        </div>
      </nav>

      <div className="main">
        <TopBar state={ready ? state : null} enabledBusy={!!busy.enabled} onToggle={actions.setEnabled} onGo={setScreen} />
        {ready && (
          <StatusBanner
            state={state}
            onGoDevices={() => setScreen('devices')}
            onRetry={() => actions.setEnabled(true)}
            onRestore={actions.restoreDefaultRouting}
          />
        )}
        <main className="content" key={ready ? screen : 'empty'}>
          {!ready ? (
            <NotConnected connection={connection} error={error} onRetry={actions.retryConnection} />
          ) : (
            <div className="screen-enter">
              {screen === 'dashboard' && <Dashboard state={state} busy={busy} onGo={setScreen} />}
              {screen === 'equalizer' && <Equalizer state={state} />}
              {screen === 'effects' && <Effects state={state} />}
              {screen === 'devices' && <Devices state={state} busy={busy} />}
              {screen === 'presets' && <Presets state={state} busy={busy} />}
              {screen === 'picture' && <Picture state={state} busy={busy} />}
              {screen === 'tv' && <TvScreen state={state} onGo={setScreen} />}
              {screen === 'settings' && <SettingsScreen state={state} busy={busy} />}
            </div>
          )}
        </main>
      </div>

      <Toasts notices={notices} onDismiss={actions.dismissNotice} />
    </div>
  );
}

const MODES: { id: ListeningMode; label: string; title: string }[] = [
  { id: 'music', label: 'Music', title: 'Music mode: uses your music preset (Settings)' },
  { id: 'movies', label: 'Movies', title: 'Movies mode: uses your movie preset (Settings)' },
  { id: 'auto', label: 'Auto', title: 'Auto: detects music, films/videos and games and switches by itself' },
];

/** Quick Music / Movies / Auto switch. Nothing is highlighted in manual mode. */
function ModeSwitch({ mode, detected, disabled }: { mode: ListeningMode; detected: DetectedContent; disabled: boolean }) {
  return (
    <div className="mode-switch" role="radiogroup" aria-label="Listening mode">
      {MODES.map((m) => {
        const active = mode === m.id;
        return (
          <button
            key={m.id}
            type="button"
            role="radio"
            aria-checked={active}
            className={`mode-switch__btn${active ? ' is-active' : ''}`}
            title={m.title}
            disabled={disabled}
            onClick={() => cinemaxActions.setListeningMode(m.id)}
          >
            {m.label}
            {m.id === 'auto' && active && detected !== 'unknown' && (
              <span className="mode-switch__hint">{detected}</span>
            )}
          </button>
        );
      })}
    </div>
  );
}

function TopBar({
  state,
  enabledBusy,
  onToggle,
  onGo,
}: {
  state: AppState | null;
  enabledBusy: boolean;
  onToggle: (on: boolean) => void;
  onGo: (s: ScreenId) => void;
}) {
  if (!state) {
    return (
      <header className="topbar">
        <div className="topbar__left" />
        <div className="topbar__right">
          <span className="status status--off">
            <span className="status__dot" />
            Not connected
          </span>
          <MasterSwitch on={false} disabled onChange={() => {}} />
        </div>
      </header>
    );
  }
  const e = state.engine;
  const out = e.output;
  const preset = state.presets.find((p) => p.id === state.params.preset);
  const st = engineStatusText(e);
  const rateText = e.status === 'running' && e.outputSampleRate ? fmtRate(e.outputSampleRate) : null;

  return (
    <header className="topbar">
      <div className="topbar__left">
        <button type="button" className="chip" onClick={() => onGo('devices')} title="Audio devices">
          <span className="chip__icon">{out ? <DeviceIcon kind={out.kind} size={18} /> : <IconDevices size={18} />}</span>
          <span className="chip__text">
            <span className="chip__eyebrow">Output</span>
            <span className="chip__value">{out ? out.name : 'No output device'}</span>
          </span>
        </button>
        <button type="button" className="chip" onClick={() => onGo('presets')} title="Presets">
          <span className="chip__text">
            <span className="chip__eyebrow">Preset</span>
            <span className="chip__value chip__value--caps">{preset ? preset.name : state.params.preset}</span>
          </span>
        </button>
        <ModeSwitch mode={state.settings.listeningMode} detected={e.detectedContent} disabled={!e.enabled} />
      </div>
      <div className="topbar__right">
        <span className={`status status--${st.tone}`} title={e.message || undefined}>
          <span className="status__dot" />
          <span className="status__label">{st.label}</span>
          {rateText && <span className="status__meta">{rateText}</span>}
        </span>
        <MasterSwitch on={e.enabled} pending={enabledBusy || e.status === 'starting'} onChange={onToggle} />
      </div>
    </header>
  );
}
