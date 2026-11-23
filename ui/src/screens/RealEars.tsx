import { useEffect, useRef, useState } from 'react';
import { api } from '../bridge/bridge';
import type { AppState, EarProfile, HeadTrackingState } from '../bridge/types';
import { actions } from '../bridge/useCinemax';
import { Badge, Button, ToggleRow } from '../components/Controls';

// The engine's test walk: front, right, behind, left, above - 0.83 s each (Spatializer::renderAudition).
const STEPS = [
  { label: 'In front', x: 50, y: 14 },
  { label: 'Right', x: 86, y: 50 },
  { label: 'Behind', x: 50, y: 86 },
  { label: 'Left', x: 14, y: 50 },
  { label: 'Above', x: 50, y: 50 },
];
const STEP_MS = 830;

function HeadMap({ step }: { step: number }) {
  return (
    <svg className="earmap" viewBox="0 0 100 100" role="img" aria-label={step >= 0 ? `Sound ${STEPS[step].label}` : 'Head'}>
      <circle cx="50" cy="50" r="17" className="earmap__head" />
      <path d="M46 34 L50 29 L54 34" className="earmap__nose" />
      <ellipse cx="32" cy="50" rx="2.5" ry="5" className="earmap__ear" />
      <ellipse cx="68" cy="50" rx="2.5" ry="5" className="earmap__ear" />
      {STEPS.map((s, i) => (
        <g key={s.label} className={`earmap__spot${i === step ? ' is-on' : ''}`}>
          <circle cx={s.x} cy={s.y} r={i === 4 ? 5 : 6} />
          {i === step && <circle cx={s.x} cy={s.y} r={11} className="earmap__ring" />}
        </g>
      ))}
    </svg>
  );
}

function trackingText(t: HeadTrackingState | undefined, on: boolean): { text: string; tone: 'accent' | 'warn' | 'muted' } {
  if (!on) return { text: 'Off', tone: 'muted' };
  if (!t) return { text: 'Starting…', tone: 'muted' };
  switch (t.status) {
    case 'off':
      return { text: t.message || 'Waiting', tone: 'muted' };
    case 'tracking':
      return { text: `Tracking · turned ${Math.abs(Math.round(t.yaw))}° ${t.yaw >= 0 ? 'right' : 'left'}`, tone: 'accent' };
    case 'starting':
      return { text: 'Starting the camera…', tone: 'muted' };
    case 'no-face':
      return { text: 'Looking for your face…', tone: 'warn' };
    default:
      return { text: t.message || 'Camera unavailable', tone: 'warn' };
  }
}

export function RealEars({ state }: { state: AppState }) {
  const sp = state.spatial;
  const settings = state.settings;
  const [testOpen, setTestOpen] = useState(false);
  const [playing, setPlaying] = useState<string | null>(null);
  const [step, setStep] = useState(-1);
  const timer = useRef<number | null>(null);
  const [live, setLive] = useState<HeadTrackingState | undefined>(undefined);

  // Live head-tracking status while it is switched on.
  const trackingOn = state.settings.headTracking;
  useEffect(() => {
    if (!trackingOn) {
      setLive(undefined);
      return;
    }
    let alive = true;
    const poll = () => {
      api
        .getHeadTracking()
        .then((t) => alive && setLive(t))
        .catch(() => undefined);
    };
    poll();
    const id = window.setInterval(poll, 500);
    return () => {
      alive = false;
      window.clearInterval(id);
    };
  }, [trackingOn]);

  const stopTimer = () => {
    if (timer.current !== null) window.clearInterval(timer.current);
    timer.current = null;
  };
  useEffect(() => () => {
    stopTimer();
    void api.stopEarTest().catch(() => undefined);
  }, []);

  if (!sp) return null;
  const profiles: EarProfile[] = sp.earProfiles ?? [];
  const measured = profiles.filter((p) => p.key !== 'model');
  const active = profiles.find((p) => p.key === sp.activeEarProfile);
  const engineOn = state.engine.enabled && state.engine.status === 'running';

  const play = async (key: string) => {
    stopTimer();
    const ok = await api.startEarTest(key).catch(() => false);
    if (!ok) return;
    setPlaying(key);
    const started = performance.now();
    setStep(0);
    timer.current = window.setInterval(() => {
      const s = Math.floor((performance.now() - started) / STEP_MS);
      if (s >= STEPS.length) {
        stopTimer();
        setStep(-1);
        setPlaying(null);
      } else {
        setStep(s);
      }
    }, 60);
  };

  const choose = (key: string) => {
    void actions.setSettings({ earProfile: key });
  };

  const tracking = trackingText(live ?? sp.headTracking, settings.headTracking);

  return (
    <div className="realears">
      <div className="field">
        <div className="field__label">
          3D ears <Badge tone={active?.key === 'model' ? 'muted' : 'accent'}>{active?.key === 'model' ? 'Head model' : 'Real ears'}</Badge>
        </div>
        {measured.length === 0 ? (
          <div className="field__note">
            No real-ear recordings are installed (the <code>hrtf</code> folder next to CINEMAX.exe is empty), so the
            classic head model is used.
          </div>
        ) : (
          <>
            <select
              className="pic-select"
              aria-label="Ear profile"
              value={settings.earProfile === 'auto' ? sp.activeEarProfile : settings.earProfile}
              onChange={(e) => choose(e.target.value)}
            >
              {profiles.map((p) => (
                <option key={p.key} value={p.key}>
                  {p.name}
                </option>
              ))}
            </select>
            {active && <div className="field__note">{active.info}</div>}
            <Button variant="ghost" onClick={() => setTestOpen((v) => !v)}>
              {testOpen ? 'Close the ear test' : 'Find my ears (1 minute)'}
            </Button>
          </>
        )}
      </div>

      {testOpen && (
        <div className="eartest">
          <p className="eartest__how">
            Close your eyes and play each one. Choose the one where the sound is <b>outside your head</b> and lands in the
            right place: <b>in front</b>, right, <b>behind</b>, left and <b>above</b>. Front and behind are the hardest - that's
            what to listen for.
          </p>
          {!engineOn && <div className="field__note is-warn">Turn CINEMAX on to hear the test.</div>}
          <div className="eartest__body">
            <HeadMap step={step} />
            <ul className="eartest__list">
              {profiles.map((p) => (
                <li key={p.key} className={p.key === sp.activeEarProfile ? 'is-active' : ''}>
                  <Button variant={playing === p.key ? 'primary' : 'ghost'} disabled={!engineOn} onClick={() => void play(p.key)}>
                    {playing === p.key ? 'Playing…' : 'Play'}
                  </Button>
                  <span className="eartest__name" title={p.info}>
                    {p.name}
                  </span>
                  {p.key === sp.activeEarProfile ? (
                    <Badge tone="accent">In use</Badge>
                  ) : (
                    <Button variant="quiet" onClick={() => choose(p.key)}>
                      Use these
                    </Button>
                  )}
                </li>
              ))}
            </ul>
          </div>
        </div>
      )}

      <ToggleRow
        title="Head tracking (webcam)"
        description="Uses the laptop camera to follow your head, so the sound stays on the screen when you turn - like AirPods. The camera light is on while it runs; nothing is recorded or saved."
        checked={settings.headTracking}
        onChange={(v) => void actions.setSettings({ headTracking: v })}
      />
      {settings.headTracking && (
        <>
          <div className="field__note">
            <Badge tone={tracking.tone}>{tracking.text}</Badge>
          </div>
          <ToggleRow
            title="Turns the wrong way? Flip it"
            description="Some cameras send a mirrored picture. If the sound follows your head instead of staying on the screen, switch this on."
            checked={settings.headTrackingFlip}
            onChange={(v) => void actions.setSettings({ headTrackingFlip: v })}
          />
        </>
      )}
    </div>
  );
}
