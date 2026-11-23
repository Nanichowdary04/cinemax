import type { ReactNode } from 'react';
import type { AppState } from '../bridge/types';
import type { BusyKey } from '../bridge/store';
import { actions, useMeterSnapshot } from '../bridge/useCinemax';
import type { ScreenId } from '../App';
import { Button, Knob, Panel, Toggle } from '../components/Controls';
import {
  DeviceIcon,
  IconApps,
  IconArrowRight,
  IconBass,
  IconCompare,
  IconDevices,
  IconEqualizer,
  IconPower,
  IconSettings,
  IconSurround,
  IconVirtual,
} from '../components/Icons';
import { BassPulse, LevelMeter, ProtectionIndicators, SpectrumView, WaveformView } from '../components/Visuals';
import { fmtChannels, fmtLevel, fmtMs, fmtRate } from '../lib/format';
import { SPECS } from '../lib/params';

const fmtSignedDb = (v: number) => `${v >= 0.05 ? '+' : v <= -0.05 ? '−' : ''}${Math.abs(v).toFixed(1)} dB`;

/** What Music / Movies / Auto and Smart tone are doing right now (real analyser data). */
function SmartLine({ state, live, m }: { state: AppState; live: boolean; m: ReturnType<typeof useMeterSnapshot> }) {
  const s = state.settings;
  const mode = s.listeningMode;
  const detected = state.engine.detectedContent;
  const via = state.engine.detectedVia ? ` (${state.engine.detectedVia})` : '';
  const modeText =
    mode === 'auto'
      ? detected === 'movie'
        ? `Auto · film / video${via}`
        : detected === 'music'
          ? `Auto · music${via}`
          : 'Auto · listening…'
      : mode === 'music'
        ? 'Music mode'
        : mode === 'movies'
          ? 'Movies mode'
          : 'Manual preset';

  let toneText: string | null = null;
  if (s.smartTone) {
    toneText = live && m ? `Smart tone: bass ${fmtSignedDb(m.smartBassDb)} · treble ${fmtSignedDb(m.smartTrebleDb)}` : 'Smart tone on';
  }

  return (
    <div className="hero__smart">
      <span className={`hero__smart-mode${mode === 'manual' ? '' : ' is-auto'}`}>{modeText}</span>
      {toneText && <span className="hero__smart-tone">{toneText}</span>}
    </div>
  );
}

export function Dashboard({ state, busy, onGo }:{ state: AppState; busy: Partial<Record<BusyKey, boolean>>; onGo: (s: ScreenId) => void }) {
  const e = state.engine;
  const p = state.params;
  const running = e.status === 'running';
  const preset = state.presets.find((x) => x.id === p.preset);
  const m = useMeterSnapshot(6);
  const live = !!m && m.active;

  const binaural = p.surroundMode === 'headphones' || (p.surroundMode === 'auto' && !!e.output?.headphoneLike);
  const surroundResolved = p.surroundMode === 'auto' && !e.output ? 'Automatic' : binaural ? 'Binaural HRTF' : 'Speaker widening';

  return (
    <div className="dashboard">
      <Panel className="hero">
        <div className="hero__head">
          <div>
            <div className="eyebrow">{running ? (e.bypass ? 'Live output · bypassed' : 'Live output') : 'Output'}</div>
            <div className="hero__preset">{preset ? preset.name : p.preset}</div>
            {preset && <div className="hero__tagline">{preset.tagline}</div>}
            <SmartLine state={state} live={live} m={m} />
          </div>
          <div className="hero__head-right">
            <ProtectionIndicators />
            <label className="bypass" title="Compare processed and original sound (audio keeps flowing)">
              <IconCompare size={18} />
              <span>Bypass</span>
              <Toggle size="sm" checked={e.bypass} onChange={actions.setBypass} label="Bypass processing" disabled={!running || !!busy.bypass} />
            </label>
          </div>
        </div>

        <div className="hero__body">
          <div className="hero__viz">
            <div className="hero__spectrum">
              <SpectrumView />
            </div>
            <div className="hero__wave">
              <WaveformView />
            </div>
            {!running && (
              <div className="hero__overlay">
                <div className="hero__overlay-inner">
                  <IconPower size={22} />
                  <div>
                    <div className="hero__overlay-title">{e.enabled ? engineHint(state) : 'CINEMAX is off'}</div>
                    <div className="hero__overlay-text">
                      {e.enabled
                        ? e.message || 'Waiting for the audio engine.'
                        : 'Windows audio plays normally. Turn the engine on to route system audio through CINEMAX.'}
                    </div>
                  </div>
                  {!e.enabled && (
                    <Button variant="primary" onClick={() => actions.setEnabled(true)} disabled={!!busy.enabled}>
                      Turn on
                    </Button>
                  )}
                </div>
              </div>
            )}
            {running && !live && <div className="hero__quiet">No audio playing</div>}
          </div>
          <div className="hero__meters">
            <div className="eyebrow eyebrow--center">L&nbsp;&nbsp;R</div>
            <div className="hero__meter-canvas">
              <LevelMeter />
            </div>
            <div className="hero__meter-read">
              <span>{live ? fmtLevel(m!.peakDb[0]) : '—'}</span>
              <span>{live ? fmtLevel(m!.peakDb[1]) : '—'}</span>
            </div>
          </div>
        </div>

        <div className="hero__foot">
          <FormatPath state={state} />
          <div className="hero__stats">
            <Stat label="Loudness" value={live ? `${fmtLevel(m!.loudnessLufs, -70)}` : '—'} unit="LUFS" />
            <Stat label="Limiter" value={live ? (m!.limiterGrDb > 0.05 ? `−${m!.limiterGrDb.toFixed(1)}` : '0.0') : '—'} unit="dB" />
            <Stat label="Latency" value={running && e.latencyMs > 0 ? e.latencyMs.toFixed(0) : '—'} unit="ms" />
          </div>
        </div>
      </Panel>

      <div className="dash-grid">
        <Panel
          className="dial-card"
          eyebrow="Bass intensity"
          icon={<IconBass size={18} />}
          title="Bass"
          actions={<Toggle size="sm" checked={p.bassEnabled} onChange={(v) => actions.setParams({ bassEnabled: v })} label="Bass enhancement" />}
        >
          <div className="dial-card__body">
            <div className="dial-wrap">
              <BassPulse />
              <Knob
                label="Boost"
                value={p.bassBoostDb}
                min={SPECS.bassBoostDb.min}
                max={SPECS.bassBoostDb.max}
                step={SPECS.bassBoostDb.step}
                reset={SPECS.bassBoostDb.reset}
                format={SPECS.bassBoostDb.format}
                size={112}
                accent={p.bassEnabled}
                disabled={!p.bassEnabled}
                onChange={(v) => actions.setParams({ bassBoostDb: v })}
              />
            </div>
            <div className="dial-card__side">
              <MiniStat label="Sub-bass" value={SPECS.subBassDb.format(p.subBassDb)} />
              <MiniStat label="Harmonic bass" value={SPECS.bassHarmonics.format(p.bassHarmonics)} />
              <MiniStat label="Protection" value={p.bassProtect ? 'On' : 'Off'} />
              <MiniStat label="Bass activity" value={live ? `${Math.round(m!.bass * 100)}%` : '—'} />
            </div>
          </div>
        </Panel>

        <Panel
          className="dial-card"
          eyebrow="Surround intensity"
          icon={<IconSurround size={18} />}
          title="Spatial"
          actions={
            <Toggle size="sm" checked={p.surroundEnabled} onChange={(v) => actions.setParams({ surroundEnabled: v })} label="Virtual surround" />
          }
        >
          <div className="dial-card__body">
            <div className="dial-wrap">
              <Knob
                label="Intensity"
                value={p.surroundIntensity}
                min={SPECS.surroundIntensity.min}
                max={SPECS.surroundIntensity.max}
                step={SPECS.surroundIntensity.step}
                reset={SPECS.surroundIntensity.reset}
                format={SPECS.surroundIntensity.format}
                size={112}
                accent={p.surroundEnabled}
                disabled={!p.surroundEnabled}
                onChange={(v) => actions.setParams({ surroundIntensity: v })}
              />
            </div>
            <div className="dial-card__side">
              <MiniStat label="Mode" value={surroundResolved} />
              <MiniStat label="Width" value={SPECS.stereoWidth.format(p.stereoWidth)} />
              <MiniStat label="Room" value={SPECS.roomAmount.format(p.roomAmount)} />
              <MiniStat label="3D height" value={p.heightAmount > 0 ? SPECS.heightAmount.format(p.heightAmount) : 'Off'} />
            </div>
          </div>
        </Panel>

        <Panel className="path-card" eyebrow="Signal path" title="Routing">
          <div className="path">
            <PathNode icon={<IconApps size={18} />} label="Windows apps" sub="All system audio" active={running} />
            <PathArrow active={running} />
            <PathNode icon={<IconVirtual size={18} />} label={e.source ? e.source.name : 'Virtual device'} sub="Capture" active={running} muted={!e.source} />
            <PathArrow active={running} />
            <PathNode icon={<span className="path__cmx">C</span>} label="CINEMAX" sub={running ? (e.bypass ? 'Bypassed' : 'Processing') : 'Idle'} active={running} accent />
            <PathArrow active={running} />
            <PathNode
              icon={e.output ? <DeviceIcon kind={e.output.kind} size={18} /> : <IconDevices size={18} />}
              label={e.output ? e.output.name : 'No output'}
              sub={e.output ? e.output.kindLabel : 'Connect a device'}
              active={running}
              muted={!e.output}
            />
          </div>
          <div className="quick-links">
            <Button variant="quiet" icon={<IconEqualizer size={16} />} onClick={() => onGo('equalizer')}>
              Equalizer
            </Button>
            <Button variant="quiet" icon={<IconDevices size={16} />} onClick={() => onGo('devices')}>
              Devices
            </Button>
            <Button variant="quiet" icon={<IconSettings size={16} />} onClick={() => onGo('settings')}>
              Settings
            </Button>
          </div>
        </Panel>
      </div>

      <div className="stats-strip">
        <Stat label="Compressor" value={live ? (m!.compressorGrDb > 0.05 ? `−${m!.compressorGrDb.toFixed(1)}` : '0.0') : '—'} unit="dB" />
        <Stat label="Input peak" value={live ? fmtLevel(m!.inputPeakDb) : '—'} unit="dBFS" />
        <Stat label="DSP load" value={m ? m.cpuPercent.toFixed(1) : '—'} unit="%" />
        <Stat label="Buffer" value={m ? m.bufferFillMs.toFixed(1) : '—'} unit="ms" />
        <Stat label="Clock drift" value={m ? `${m.driftPpm >= 0 ? '+' : '−'}${Math.abs(m.driftPpm).toFixed(0)}` : '—'} unit="ppm" />
        <Stat label="Dropouts" value={m ? String(m.xruns) : '—'} unit="" />
      </div>
    </div>
  );
}

function engineHint(state: AppState) {
  switch (state.engine.status) {
    case 'starting':
      return 'Starting the audio engine…';
    case 'no-virtual-device':
      return 'Virtual audio device needed';
    case 'no-output-device':
      return 'No output device available';
    case 'error':
      return 'The audio engine stopped';
    default:
      return 'Engine idle';
  }
}

function FormatPath({ state }: { state: AppState }) {
  const e = state.engine;
  if (e.status !== 'running') {
    return <div className="format-path format-path--idle">Audio format appears here while the engine is running.</div>;
  }
  return (
    <div className="format-path">
      <span>
        {fmtRate(e.inputSampleRate)} · {fmtChannels(e.inputChannels)}
      </span>
      <IconArrowRight size={14} />
      <span>
        {fmtRate(e.outputSampleRate)} · {fmtChannels(e.outputChannels)}
      </span>
      <span className="format-path__sep" />
      <span>buffer {fmtMs(e.bufferMs, 0)}</span>
    </div>
  );
}

function Stat({ label, value, unit }: { label: string; value: string; unit: string }) {
  return (
    <div className="stat">
      <div className="stat__label">{label}</div>
      <div className="stat__value">
        {value}
        {unit && value !== '—' && <span className="stat__unit">{unit}</span>}
      </div>
    </div>
  );
}

function MiniStat({ label, value }: { label: string; value: string }) {
  return (
    <div className="mini">
      <span className="mini__label">{label}</span>
      <span className="mini__value">{value}</span>
    </div>
  );
}

function PathNode({
  icon,
  label,
  sub,
  active,
  muted,
  accent,
}: {
  icon: ReactNode;
  label: string;
  sub: string;
  active: boolean;
  muted?: boolean;
  accent?: boolean;
}) {
  return (
    <div className={`path__node${active ? ' is-active' : ''}${muted ? ' is-muted' : ''}${accent ? ' is-accent' : ''}`}>
      <span className="path__icon">{icon}</span>
      <span className="path__text">
        <span className="path__label" title={label}>
          {label}
        </span>
        <span className="path__sub">{sub}</span>
      </span>
    </div>
  );
}

function PathArrow({ active }: { active: boolean }) {
  return <span className={`path__arrow${active ? ' is-active' : ''}`} aria-hidden="true" />;
}

