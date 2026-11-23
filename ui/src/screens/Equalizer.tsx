import { useRef, useState, type KeyboardEvent } from 'react';
import type { AppState, DspParams, EqBand } from '../bridge/types';
import { getSnapshot } from '../bridge/store';
import { actions } from '../bridge/useCinemax';
import { ScreenHeader } from '../components/Chrome';
import { Button, Knob, Panel, Slider, Toggle } from '../components/Controls';
import { EqCurve } from '../components/EqCurve';
import { IconRefresh } from '../components/Icons';
import { clamp, fmtDb, fmtHz, fmtHzShort } from '../lib/format';
import { EQ_LIMITS, SPECS, STANDARD_BANDS } from '../lib/params';

const LOG_STEPS = 1000;
const freqToPos = (f: number) => Math.round((Math.log(clamp(f, 20, 20000) / 20) / Math.log(1000)) * LOG_STEPS);
const posToFreq = (p: number) => {
  const f = 20 * Math.pow(1000, p / LOG_STEPS);
  if (f < 100) return Math.round(f);
  if (f < 1000) return Math.round(f / 5) * 5;
  if (f < 10000) return Math.round(f / 50) * 50;
  return Math.round(f / 100) * 100;
};

export function Equalizer({ state }: { state: AppState }) {
  const p = state.params;
  const [selected, setSelected] = useState(4);
  const sel = p.eqBands[selected] ?? p.eqBands[0];
  const setBand = (i: number, b: Partial<EqBand>) => actions.setEqBand(i, b);
  const enabled = p.eqEnabled;

  return (
    <div className="equalizer">
      <ScreenHeader eyebrow="10-band parametric" title="Equalizer">
        <span className="inline-toggle">
          <span>{enabled ? 'Enabled' : 'Disabled'}</span>
          <Toggle checked={enabled} onChange={(v) => actions.setParams({ eqEnabled: v })} label="Equalizer enabled" />
        </span>
        <Button
          variant="ghost"
          onClick={() => actions.setParams({ eqBands: p.eqBands.map((b) => ({ ...b, gainDb: 0 })), bassDb: 0, midDb: 0, trebleDb: 0, preampDb: 0 })}
          title="Set every band, tone control and the preamp to 0 dB"
        >
          Flatten
        </Button>
        <Button
          variant="ghost"
          icon={<IconRefresh size={16} />}
          onClick={() => actions.setParams({ eqBands: STANDARD_BANDS.map((b) => ({ ...b })) })}
          title="Restore standard octave frequencies, Q 1.41 and 0 dB"
        >
          Reset bands
        </Button>
      </ScreenHeader>

      <Panel className="eq-graph">
        <EqCurve params={p} selected={selected} onSelect={setSelected} onBandChange={setBand} />
        <div className="eq-graph__hint">Drag handles · scroll to change Q · double-click to zero · keys 1–0 select a band</div>
      </Panel>

      <div className="eq-lower">
        <Panel className="eq-bands" eyebrow="Gain per band">
          <div className="faders">
            {p.eqBands.map((b, i) => (
              <Fader
                key={i}
                index={i}
                band={b}
                selected={i === selected}
                disabled={!enabled}
                onSelect={() => setSelected(i)}
                onChange={(gainDb) => setBand(i, { gainDb })}
              />
            ))}
          </div>
        </Panel>

        <Panel className="eq-inspector" eyebrow={`Band ${selected + 1}`} title={fmtHz(sel.freq)}>
          <div className="stack">
            <Slider
              label="Frequency"
              value={freqToPos(sel.freq)}
              min={0}
              max={LOG_STEPS}
              step={1}
              format={(v) => fmtHz(posToFreq(v))}
              onChange={(v) => setBand(selected, { freq: posToFreq(v) })}
              reset={freqToPos(STANDARD_BANDS[selected]?.freq ?? 1000)}
              disabled={!enabled}
            />
            <Slider
              label="Gain"
              value={sel.gainDb}
              min={EQ_LIMITS.gain.min}
              max={EQ_LIMITS.gain.max}
              step={EQ_LIMITS.gain.step}
              bipolar
              reset={0}
              format={(v) => fmtDb(v)}
              onChange={(v) => setBand(selected, { gainDb: v })}
              disabled={!enabled}
            />
            <Slider
              label="Q (bandwidth)"
              value={sel.q}
              min={EQ_LIMITS.q.min}
              max={EQ_LIMITS.q.max}
              step={EQ_LIMITS.q.step}
              reset={1.41}
              format={(v) => `${v.toFixed(2)}${v < 0.8 ? ' · wide' : v > 3 ? ' · narrow' : ''}`}
              onChange={(v) => setBand(selected, { q: v })}
              disabled={!enabled}
            />
          </div>
        </Panel>

        <Panel className="eq-tone" eyebrow="Tone & level">
          <div className="knob-row">
            {(['bassDb', 'midDb', 'trebleDb', 'preampDb'] as const).map((k) => (
              <Knob
                key={k}
                label={k === 'bassDb' ? 'Bass · 100 Hz' : k === 'midDb' ? 'Mid · 1 kHz' : k === 'trebleDb' ? 'Treble · 8 kHz' : 'Preamp'}
                value={p[k]}
                min={SPECS[k].min}
                max={SPECS[k].max}
                step={SPECS[k].step}
                bipolar
                reset={0}
                format={SPECS[k].format}
                size={74}
                disabled={k !== 'preampDb' && !enabled}
                onChange={(v) => actions.setParams({ [k]: v } as Partial<DspParams>)}
              />
            ))}
          </div>
        </Panel>
      </div>
    </div>
  );
}

// ---------------------------------------------------------------------------
//  Vertical fader for one band's gain.
function Fader({
  index,
  band,
  selected,
  disabled,
  onSelect,
  onChange,
}: {
  index: number;
  band: EqBand;
  selected: boolean;
  disabled: boolean;
  onSelect: () => void;
  onChange: (gainDb: number) => void;
}) {
  const track = useRef<HTMLDivElement>(null);
  const dragging = useRef(false);
  const { min, max } = EQ_LIMITS.gain;
  const norm = (clamp(band.gainDb, min, max) - min) / (max - min);

  const fromY = (clientY: number) => {
    const r = track.current!.getBoundingClientRect();
    const t = 1 - clamp((clientY - r.top) / r.height, 0, 1);
    return Math.round((min + t * (max - min)) * 2) / 2; // 0.5 dB steps
  };

  const onKey = (e: KeyboardEvent) => {
    let v: number | null = null;
    const s = e.shiftKey ? 0.1 : 0.5;
    const g = getSnapshot().state?.params.eqBands[index]?.gainDb ?? band.gainDb;
    if (e.key === 'ArrowUp' || e.key === 'ArrowRight') v = g + s;
    else if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') v = g - s;
    else if (e.key === 'Home') v = max;
    else if (e.key === 'End') v = min;
    else if (e.key === 'Delete' || e.key === 'Backspace') v = 0;
    if (v !== null) {
      e.preventDefault();
      onChange(clamp(Math.round(v * 10) / 10, min, max));
    }
  };

  return (
    <div className={`fader${selected ? ' is-selected' : ''}${disabled ? ' is-disabled' : ''}`}>
      <div className="fader__value">{band.gainDb > 0 ? '+' : band.gainDb < 0 ? '−' : ''}{Math.abs(band.gainDb).toFixed(1)}</div>
      <div
        ref={track}
        className="fader__track"
        role="slider"
        tabIndex={disabled ? -1 : 0}
        aria-label={`Band ${index + 1}, ${fmtHz(band.freq)} gain`}
        aria-valuemin={min}
        aria-valuemax={max}
        aria-valuenow={band.gainDb}
        aria-valuetext={fmtDb(band.gainDb)}
        aria-disabled={disabled}
        onFocus={onSelect}
        onKeyDown={disabled ? undefined : onKey}
        onDoubleClick={() => !disabled && onChange(0)}
        onPointerDown={(e) => {
          onSelect();
          if (disabled) return;
          dragging.current = true;
          e.currentTarget.setPointerCapture(e.pointerId);
          onChange(fromY(e.clientY));
        }}
        onPointerMove={(e) => {
          if (dragging.current) onChange(fromY(e.clientY));
        }}
        onPointerUp={() => (dragging.current = false)}
        onPointerCancel={() => (dragging.current = false)}
      >
        <span className="fader__zero" />
        <span
          className="fader__fill"
          style={norm >= 0.5 ? { bottom: '50%', height: `${(norm - 0.5) * 100}%` } : { top: '50%', height: `${(0.5 - norm) * 100}%` }}
        />
        <span className="fader__thumb" style={{ bottom: `calc(${norm * 100}% - 7px)` }} />
      </div>
      <div className="fader__freq">{fmtHzShort(band.freq)}</div>
    </div>
  );
}
