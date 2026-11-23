// CINEMAX Vision: Smart-TV-style picture settings. Every control drives the real
// display pipeline (Intel 3D colour table + hardware sharpness, or the Windows gamma
// ramp fallback). The test patterns below are ordinary pixels, so what you see on
// them IS the processed result; "Hold to compare" bypasses the hardware processing.
import { useEffect, useMemo, useState, type CSSProperties, type KeyboardEvent } from 'react';
import type { AppState, DisplayView, PictureParams, PicturePresetId } from '../bridge/types';
import type { BusyKey } from '../bridge/store';
import { actions } from '../bridge/useCinemax';
import { ConfirmButton, ScreenHeader } from '../components/Chrome';
import { Badge, Button, Panel, Segmented, Slider, Toggle, ToggleRow } from '../components/Controls';
import { IconRefresh } from '../components/Icons';

const v = actions.vision;

const PRESET_ORDER: PicturePresetId[] = ['cinema', 'hdr', 'vivid', 'gaming', 'work', 'night', 'custom'];

const TEMPS: { value: number; label: string; hint: string }[] = [
  { value: 5000, label: 'Warm 2', hint: '5000 K' },
  { value: 5800, label: 'Warm 1', hint: '5800 K' },
  { value: 6500, label: 'Standard', hint: '6500 K (unchanged)' },
  { value: 7500, label: 'Cool', hint: '7500 K' },
];

const SCENE_TEXT: Record<string, string> = {
  movie: 'video',
  game: 'game',
  work: 'work app',
  music: 'music app',
  none: 'other app',
};

const signed = (x: number) => (x > 0 ? `+${Math.round(x)}` : `${Math.round(x)}`);

export function Picture({ state, busy }: { state: AppState; busy: Partial<Record<BusyKey, boolean>> }) {
  const vs = state.vision;
  const [selectedKey, setSelectedKey] = useState<string | null>(null);
  const display = useMemo(
    () => vs?.displays.find((d) => d.key === selectedKey) ?? vs?.displays[0],
    [vs, selectedKey],
  );

  // Displays, backlight, HDR and Night Light can change outside CINEMAX: re-read while visible.
  useEffect(() => {
    v.refresh();
    const t = window.setInterval(() => {
      if (document.visibilityState === 'visible') v.refresh();
    }, 5000);
    return () => window.clearInterval(t);
  }, []);

  if (!vs) {
    return (
      <div className="picture">
        <ScreenHeader eyebrow="CINEMAX Vision" title="Picture" />
        <div className="list-empty">The picture engine is not available in this build.</div>
      </div>
    );
  }

  const enabled = vs.settings.enabled;

  return (
    <div className="picture">
      <ScreenHeader eyebrow="CINEMAX Vision" title="Picture">
        <span className="pic-master">
          <span className="pic-master__label">Picture processing</span>
          <Toggle checked={enabled} onChange={v.setEnabled} label="Picture processing" disabled={!!busy.vision} />
        </span>
      </ScreenHeader>

      {vs.displays.length > 1 && (
        <div className="pic-displays">
          <Segmented<string>
            label="Display"
            value={display?.key ?? ''}
            onChange={setSelectedKey}
            options={vs.displays.map((d) => ({ value: d.key, label: d.name }))}
          />
        </div>
      )}

      {!display ? (
        <div className="list-empty">No active display found.</div>
      ) : (
        <>
          <Capabilities d={display} nightLight={vs.nightLight} />
          <div className="pic-grid">
            <ModePanel state={state} d={display} busy={!!busy.vision} />
            <ComparePanel d={display} comparing={vs.comparing} enabled={enabled} />
            <AdjustPanel d={display} enabled={enabled} presetBacklight={vs.settings.presetBacklight} />
            <ColourPanel d={display} enabled={enabled} />
            <AutoPanel state={state} busy={!!busy.vision} />
            <DisplayPanel d={display} nightLight={vs.nightLight} busy={!!busy.vision} />
          </div>
          <div className="pic-actions">
            <ConfirmButton variant="ghost" icon={<IconRefresh size={16} />} confirmLabel="Reset this display" onConfirm={() => v.resetDisplay(display.key)} disabled={!!busy.vision}>
              Reset to default
            </ConfirmButton>
            <Button variant="primary" onClick={v.saveProfiles} disabled={!!busy.vision}>
              Apply and save profile
            </Button>
            <ConfirmButton variant="ghost" confirmLabel="Restore original display" onConfirm={v.restoreOriginal} disabled={!!busy.vision}>
              Restore original display
            </ConfirmButton>
          </div>
        </>
      )}
    </div>
  );
}

// ---------------------------------------------------------------------------
function Capabilities({ d, nightLight }: { d: DisplayView; nightLight: boolean }) {
  const hdr = d.hdrEnabled ? 'HDR on' : d.hdrSupported ? 'HDR capable (off)' : 'SDR (no HDR)';
  return (
    <div className="pic-caps" aria-label="Display capabilities">
      <span className="pic-cap pic-cap--name">{d.name}</span>
      <span className="pic-cap">
        {d.width}×{d.height}
      </span>
      <span className="pic-cap">{d.refreshHz.toFixed(d.refreshHz % 1 > 0.05 ? 2 : 0)} Hz</span>
      <span className="pic-cap">{d.bitsPerColor}-bit</span>
      <span className={`pic-cap${d.hdrEnabled ? ' pic-cap--on' : ''}`}>{hdr}</span>
      {d.maxNits > 0 && <span className="pic-cap">~{d.maxNits} nits</span>}
      {d.gamutPercent > 0 && <span className="pic-cap" title="Area of the panel's colour triangle relative to sRGB">~{d.gamutPercent}% of sRGB area</span>}
      <span className={`pic-cap${d.backend === 'intel' ? ' pic-cap--on' : ''}`}>
        {d.backend === 'intel' ? 'Hardware display pipe' : d.backend === 'gamma' ? 'Gamma-ramp fallback' : 'No control'}
      </span>
      {nightLight && <span className="pic-cap pic-cap--warn">Windows Night Light on</span>}
    </div>
  );
}

// ---------------------------------------------------------------------------
function ModePanel({ state, d, busy }: { state: AppState; d: DisplayView; busy: boolean }) {
  const vs = state.vision;
  const byId = new Map(vs.presets.map((p) => [p.id, p]));
  const auto = d.mode === 'auto' && vs.settings.autoPicture;
  const sceneText = SCENE_TEXT[vs.scene] ?? 'app';

  return (
    <Panel
      eyebrow="Picture mode"
      title={byId.get(d.activePreset)?.name ?? d.activePreset.toUpperCase()}
      className="pic-wide"
      actions={
        auto ? (
          <Badge tone="accent" title="Auto picture follows the app in front">
            Auto
          </Badge>
        ) : (
          <Button variant="quiet" onClick={() => v.setAuto(d.key)} disabled={busy || !vs.settings.autoPicture}>
            Back to Auto
          </Button>
        )
      }
    >
      <p className="pic-note">
        {auto
          ? vs.sceneApp
            ? `Auto: ${sceneText} in front (${vs.sceneApp}).`
            : 'Auto: follows the app in front.'
          : vs.settings.autoPicture
            ? 'Manual: your choice stays until you switch back to Auto.'
            : 'Auto picture is off (see Auto picture & sound).'}
      </p>
      <div className="pic-presets" role="radiogroup" aria-label="Picture presets">
        {PRESET_ORDER.map((id) => {
          const info = byId.get(id);
          if (!info) return null;
          const active = d.activePreset === id;
          return (
            <button
              key={id}
              type="button"
              role="radio"
              aria-checked={active}
              className={`pic-preset${active ? ' is-active' : ''}`}
              onClick={() => v.selectPreset(d.key, id)}
              disabled={busy}
              title={info.tagline}
            >
              <span className={`pic-swatch pic-swatch--${id}`} aria-hidden="true" />
              <span className="pic-preset__name">{info.name}</span>
              <span className="pic-preset__tagline">{info.tagline}</span>
            </button>
          );
        })}
      </div>
      {d.activePreset === 'hdr' && !d.hdrSupported && (
        <p className="pic-note pic-note--warn">This display is SDR: HDR EXPERIENCE is an HDR-style tone curve, not real HDR.</p>
      )}
    </Panel>
  );
}

// ---------------------------------------------------------------------------
function ComparePanel({ d, comparing, enabled }: { d: DisplayView; comparing: boolean; enabled: boolean }) {
  const a = d.analysis;
  const press = (on: boolean) => enabled && v.setCompare(on);
  const onKey = (e: KeyboardEvent<HTMLButtonElement>, down: boolean) => {
    if (e.key === ' ' || e.key === 'Enter') {
      e.preventDefault();
      if (!e.repeat) press(down);
    }
  };
  const greys = Array.from({ length: 17 }, (_, i) => Math.min(255, i * 16));
  const blacks = Array.from({ length: 13 }, (_, i) => i * 2);
  const whites = Array.from({ length: 13 }, (_, i) => 231 + i * 2);
  const skins = ['rgb(255,224,196)', 'rgb(241,194,160)', 'rgb(224,172,140)', 'rgb(198,134,96)', 'rgb(141,85,56)', 'rgb(88,57,39)'];
  const bars = ['#bfbfbf', '#bfbf00', '#00bfbf', '#00bf00', '#bf00bf', '#bf0000', '#0000bf'];
  const sw = (c: string) => ({ background: c }) as CSSProperties;

  return (
    <Panel eyebrow="Before / after" title="Live check" className="pic-wide">
      <p className="pic-note">
        These patterns are plain pixels, processed by the display hardware like everything else on screen. Hold the button to see the
        original picture on the whole screen.
      </p>
      <div className="pic-patterns">
        <div className="pic-pattern">
          <span className="pic-pattern__label">Grey scale</span>
          <div className="pic-steps">
            {greys.map((g) => (
              <span key={g} style={sw(`rgb(${g},${g},${g})`)} title={String(g)} />
            ))}
          </div>
        </div>
        <div className="pic-pattern">
          <span className="pic-pattern__label">Near black 0–24 (should stay visible)</span>
          <div className="pic-steps pic-steps--dark">
            {blacks.map((g) => (
              <span key={g} style={sw(`rgb(${g},${g},${g})`)} title={String(g)} />
            ))}
          </div>
        </div>
        <div className="pic-pattern">
          <span className="pic-pattern__label">Near white 231–255</span>
          <div className="pic-steps">
            {whites.map((g) => (
              <span key={g} style={sw(`rgb(${g},${g},${g})`)} title={String(g)} />
            ))}
          </div>
        </div>
        <div className="pic-pattern pic-pattern--half">
          <span className="pic-pattern__label">Skin tones</span>
          <div className="pic-steps">
            {skins.map((c) => (
              <span key={c} style={sw(c)} />
            ))}
          </div>
        </div>
        <div className="pic-pattern pic-pattern--half">
          <span className="pic-pattern__label">75% colour bars</span>
          <div className="pic-steps">
            {bars.map((c) => (
              <span key={c} style={sw(c)} />
            ))}
          </div>
        </div>
        <div className="pic-pattern">
          <span className="pic-pattern__label">Gradients</span>
          <div className="pic-gradient pic-gradient--grey" />
          <div className="pic-gradient pic-gradient--hue" />
        </div>
      </div>

      <div className="pic-compare-row">
        <button
          type="button"
          className={`pic-compare${comparing ? ' is-on' : ''}`}
          disabled={!enabled}
          onPointerDown={(e) => {
            (e.target as HTMLElement).setPointerCapture?.(e.pointerId);
            press(true);
          }}
          onPointerUp={() => press(false)}
          onPointerCancel={() => press(false)}
          onLostPointerCapture={() => press(false)}
          onKeyDown={(e) => onKey(e, true)}
          onKeyUp={(e) => onKey(e, false)}
          onBlur={() => comparing && press(false)}
        >
          {comparing ? 'Original picture — release for CINEMAX' : 'Hold to see the original'}
        </button>
        <dl className="pic-analysis">
          <div>
            <dt>Near-black steps</dt>
            <dd className={a.shadowStepsKept < 20 ? 'is-warn' : ''}>
              {a.shadowStepsKept}/{a.shadowSteps}
            </dd>
          </div>
          <div>
            <dt>Near-white steps</dt>
            <dd className={a.highlightStepsKept < 20 ? 'is-warn' : ''}>
              {a.highlightStepsKept}/{a.highlightSteps}
            </dd>
          </div>
          <div>
            <dt>White point (RGB)</dt>
            <dd>{a.white.join(' · ')}</dd>
          </div>
          <div>
            <dt>Colours eased at limit</dt>
            <dd>{a.gamutLimitedPercent.toFixed(1)}%</dd>
          </div>
        </dl>
      </div>
      <p className="pic-note">{d.status}</p>
    </Panel>
  );
}

// ---------------------------------------------------------------------------
function AdjustPanel({ d, enabled, presetBacklight }: { d: DisplayView; enabled: boolean; presetBacklight: boolean }) {
  const p = d.params;
  const set = (partial: Partial<PictureParams>) => v.setParams(d.key, partial);
  const off = !enabled;
  return (
    <Panel eyebrow="Light & tone" title="Picture">
      {d.backlight >= 0 ? (
        <Slider
          label={d.internal ? 'Backlight (hardware)' : 'Monitor brightness (hardware)'}
          value={d.backlight}
          min={0}
          max={100}
          step={1}
          format={(x) => `${Math.round(x)}%`}
          onChange={(x) => v.setBacklight(d.key, x)}
          hint="The panel's real light output - the same control as Windows brightness."
        />
      ) : (
        <p className="pic-note">This display's backlight can't be controlled from Windows (no DDC/CI).</p>
      )}
      {d.backlight >= 0 && (
        <ToggleRow
          title="Picture modes set the backlight"
          description={
            presetBacklight
              ? `Like a TV: VIVID +25, HDR +20, GAMING +12, WORK -5, CINEMA -8, NIGHT -30 from your level (${d.baseBacklight}%${d.backlightOffset ? `, now ${d.backlightOffset > 0 ? '+' : ''}${d.backlightOffset}` : ''}).`
              : 'Off: picture modes leave the brightness exactly where you set it.'
          }
          checked={presetBacklight}
          onChange={(x) => v.setSettings({ presetBacklight: x })}
          disabled={off}
        />
      )}
      <Slider label="Brightness" value={p.brightness} min={-50} max={50} step={1} bipolar reset={0} format={signed} onChange={(x) => set({ brightness: x })} disabled={off}
        hint="Mid-tone lightness. Black and white stay where they are." />
      <Slider label="Contrast" value={p.contrast} min={-50} max={50} step={1} bipolar reset={0} format={signed} onChange={(x) => set({ contrast: x })} disabled={off}
        hint="S-curve around mid-grey; shadow and highlight detail is protected." />
      <Slider label="Black level" value={p.blackLevel} min={-20} max={20} step={1} bipolar reset={0} format={signed} onChange={(x) => set({ blackLevel: x })} disabled={off}
        hint="+ reveals near-black detail (black stays black); - deepens shadows without merging steps." />
      <Slider label="Gamma" value={p.gamma} min={1.8} max={2.6} step={0.05} reset={2.2} format={(x) => x.toFixed(2)} onChange={(x) => set({ gamma: x })} disabled={off}
        hint="Target display gamma. 2.2 = unchanged, higher = deeper mid-tones (dark rooms), lower = brighter." />
      <div className="field">
        <div className="field__label">Sharpness {d.hasSharpness ? '(hardware filter)' : ''}</div>
        <Segmented<number>
          label="Sharpness"
          value={p.sharpness}
          onChange={(x) => set({ sharpness: x })}
          disabled={off || !d.hasSharpness}
          options={[0, 1, 2, 3, 4, 5].map((n) => ({ value: n, label: n === 0 ? 'Off' : String(n) }))}
        />
        {!d.hasSharpness && <div className="field__note">Sharpness needs the Intel display pipe; not available on this display.</div>}
      </div>
    </Panel>
  );
}

function ColourPanel({ d, enabled }: { d: DisplayView; enabled: boolean }) {
  const p = d.params;
  const set = (partial: Partial<PictureParams>) => v.setParams(d.key, partial);
  const off = !enabled;
  const tempPreset = TEMPS.find((t) => Math.abs(t.value - p.temperatureK) < 1)?.value ?? -1;
  const gainSet = (i: number, x: number) => set({ gain: p.gain.map((g, k) => (k === i ? x : g)) as [number, number, number] });
  const offSet = (i: number, x: number) => set({ offset: p.offset.map((g, k) => (k === i ? x : g)) as [number, number, number] });
  const rgb = ['Red', 'Green', 'Blue'];
  return (
    <Panel eyebrow="Colour" title="Colour">
      <Slider label="Saturation" value={p.saturation} min={-50} max={50} step={1} bipolar reset={0} format={(x) => `${signed(x)}%`} onChange={(x) => set({ saturation: x })} disabled={off} />
      <Slider label="Vibrance" value={p.vibrance} min={0} max={100} step={1} reset={0} format={(x) => `${Math.round(x)}`} onChange={(x) => set({ vibrance: x })} disabled={off}
        hint="Boosts muted colours more than saturated ones; skin tones are held back." />
      <div className="field">
        <div className="field__label">Colour temperature</div>
        <Segmented<number> label="Colour temperature" value={tempPreset} onChange={(x) => set({ temperatureK: x })} disabled={off}
          options={TEMPS.map((t) => ({ value: t.value, label: t.label, hint: t.hint }))} />
      </div>
      <Slider label="Fine temperature" value={p.temperatureK} min={3500} max={10000} step={100} reset={6500} format={(x) => `${Math.round(x)} K`} onChange={(x) => set({ temperatureK: x })} disabled={off} />
      <Slider label="Blue light reduction" value={p.blueLight} min={0} max={100} step={1} reset={0} format={(x) => `${Math.round(x)}%`} onChange={(x) => set({ blueLight: x })} disabled={off} />
      <ToggleRow title="Protect skin tones" description="Keeps faces natural when saturation or vibrance is raised." checked={p.skinProtect} onChange={(x) => set({ skinProtect: x })} disabled={off} />
      <details className="pic-expert">
        <summary>White balance (2-point) and RGB balance</summary>
        <div className="pic-wb">
          <div>
            <div className="field__label">Gain (highlights)</div>
            {rgb.map((c, i) => (
              <Slider key={c} label={c} value={p.gain[i]} min={-50} max={50} step={1} bipolar reset={0} format={signed} onChange={(x) => gainSet(i, x)} disabled={off} />
            ))}
          </div>
          <div>
            <div className="field__label">Offset (shadows)</div>
            {rgb.map((c, i) => (
              <Slider key={c} label={c} value={p.offset[i]} min={-20} max={20} step={1} bipolar reset={0} format={signed} onChange={(x) => offSet(i, x)} disabled={off} />
            ))}
          </div>
        </div>
      </details>
    </Panel>
  );
}

// ---------------------------------------------------------------------------
function AutoPanel({ state, busy }: { state: AppState; busy: boolean }) {
  const vs = state.vision;
  const s = vs.settings;
  const [rulePreset, setRulePreset] = useState<PicturePresetId>('cinema');
  const presetName = (id: PicturePresetId) => vs.presets.find((p) => p.id === id)?.name ?? id;
  const fg = vs.foregroundExe;
  const fgLower = fg.toLowerCase();
  const canAddFg = fg !== '' && !vs.rules.some((r) => r.exe === fgLower);

  return (
    <Panel eyebrow="Picture + sound" title="Auto picture" className="pic-wide">
      <div className="pic-auto-grid">
        <div>
          <ToggleRow title="Auto picture" description="Pick the picture from the app in front: video players and films → CINEMA, games → GAMING, editors and Office → WORK." checked={s.autoPicture} onChange={(x) => v.setSettings({ autoPicture: x })} disabled={busy} />
          <ToggleRow title="Switch the sound too" description="Games use the GAMING sound; videos the movie preset (when the sound mode is Auto)." checked={s.linkAudio} onChange={(x) => v.setSettings({ linkAudio: x })} disabled={busy} />
          <ToggleRow title="Browser videos → CINEMA" description="A browser whose tab title shows a film or video site (YouTube, Netflix, Prime Video…) counts as video." checked={s.browserVideoAsCinema} onChange={(x) => v.setSettings({ browserVideoAsCinema: x })} disabled={busy} />
          <div className="field">
            <div className="field__label">Everything else uses</div>
            <Segmented<PicturePresetId> label="Default picture" value={s.defaultPreset} onChange={(x) => v.setSettings({ defaultPreset: x })} disabled={busy}
              options={(['cinema', 'vivid', 'work', 'night'] as PicturePresetId[]).map((id) => ({ value: id, label: presetName(id) }))} />
          </div>
          <div className="field__note">Only the app's name and window title are read - the screen is never captured.</div>
        </div>
        <div>
          <div className="field__label">App rules (always win)</div>
          {vs.rules.length === 0 ? (
            <div className="list-empty">No app rules yet.</div>
          ) : (
            <ul className="pic-rules">
              {vs.rules.map((r) => (
                <li key={r.exe}>
                  <span className="pic-rules__exe">{r.exe}</span>
                  <span className="pic-rules__preset">{presetName(r.preset)}</span>
                  <Button variant="quiet" onClick={() => v.removeRule(r.exe)} disabled={busy}>
                    Remove
                  </Button>
                </li>
              ))}
            </ul>
          )}
          <div className="pic-rule-add">
            <span className="pic-note">
              App in front before CINEMAX: <strong>{fg || '—'}</strong>
            </span>
            <select className="pic-select" value={rulePreset} onChange={(e) => setRulePreset(e.target.value as PicturePresetId)} aria-label="Preset for this app">
              {PRESET_ORDER.filter((id) => id !== 'custom').map((id) => (
                <option key={id} value={id}>
                  {presetName(id)}
                </option>
              ))}
            </select>
            <Button variant="ghost" onClick={() => v.addRule(fg, rulePreset)} disabled={busy || !canAddFg}>
              Always use for this app
            </Button>
          </div>
        </div>
      </div>
    </Panel>
  );
}

// ---------------------------------------------------------------------------
function DisplayPanel({ d, nightLight, busy }: { d: DisplayView; nightLight: boolean; busy: boolean }) {
  return (
    <Panel eyebrow="Display" title="HDR & hardware" className="pic-wide pic-hw">
      <dl className="format-grid">
        <div>
          <dt>Connection</dt>
          <dd>{d.internal ? 'Built-in panel' : 'External monitor'}</dd>
        </div>
        <div>
          <dt>Graphics</dt>
          <dd>{d.adapter || '—'}</dd>
        </div>
        <div>
          <dt>Processing</dt>
          <dd>
            {d.backend === 'intel'
              ? `Display pipe: ${d.hasLut ? `${d.lutSize}³ colour table` : 'no colour table'}${d.hasSharpness ? ' + sharpness' : ''}`
              : d.backend === 'gamma'
                ? 'Windows gamma ramp (fallback)'
                : 'None'}
          </dd>
        </div>
        <div>
          <dt>Peak / black</dt>
          <dd>
            {d.maxNits > 0 ? `${d.maxNits} nits / ${d.minNits} nits` : '—'}
          </dd>
        </div>
        <div>
          <dt>Wide colour (ACM)</dt>
          <dd>{d.wideColorSupported ? (d.wideColorEnabled ? 'On' : 'Supported, off') : 'Not supported'}</dd>
        </div>
        <div>
          <dt>Monitor controls (DDC/CI)</dt>
          <dd>{d.internal ? 'Built-in panel: backlight only' : d.ddcci ? 'Available' : 'Not answering'}</dd>
        </div>
      </dl>

      <div className="safety__row">
        <div>
          <div className="row__title">Windows HDR</div>
          <div className="row__desc">
            {d.hdrSupported
              ? d.hdrEnabled
                ? 'HDR is on. Windows colour-manages HDR content; CINEMAX keeps only sharpness active on this display.'
                : 'This display supports HDR. CINEMAX never turns it on by itself.'
              : 'This display does not support HDR, so CINEMAX will not try to enable it. HDR EXPERIENCE uses an HDR-style tone curve instead.'}
          </div>
        </div>
        {d.hdrSupported && (
          <ConfirmButton variant="ghost" confirmLabel={d.hdrEnabled ? 'Turn HDR off' : 'Turn HDR on'} onConfirm={() => v.setHdr(d.key, !d.hdrEnabled)} disabled={busy}>
            {d.hdrEnabled ? 'Turn HDR off' : 'Turn HDR on'}
          </ConfirmButton>
        )}
      </div>
      <div className="safety__row">
        <div>
          <div className="row__title">Windows Night Light {nightLight ? '(on)' : '(off)'}</div>
          <div className="row__desc">
            {nightLight
              ? 'Night Light is on and works in a different hardware stage - its warmth adds to CINEMAX blue-light reduction.'
              : 'Works alongside CINEMAX. If you use it, lower CINEMAX blue-light reduction to avoid double warming.'}
          </div>
        </div>
        <Button variant="ghost" onClick={() => v.openDisplaySettings(true)}>
          Night Light settings
        </Button>
      </div>
      {d.limitedByWindows && <p className="pic-note pic-note--warn">Windows limits how strong gamma-ramp changes can be - the effect was softened to fit.</p>}
    </Panel>
  );
}
