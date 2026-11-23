import type { ReactNode } from 'react';
import type { AppState, DspParams, IeqProfile, SurroundMode } from '../bridge/types';
import { actions } from '../bridge/useCinemax';
import { ScreenHeader } from '../components/Chrome';
import { Panel, Segmented, Slider, Toggle, ToggleRow } from '../components/Controls';
import { IconBass, IconDynamics, IconSurround, IconVoice } from '../components/Icons';
import { SPECS } from '../lib/params';
import { RealEars } from './RealEars';

type NumKey = keyof typeof SPECS;

function P({ k, p, disabled, hint }: { k: NumKey; p: DspParams; disabled?: boolean; hint?: string }) {
  const s = SPECS[k];
  return (
    <Slider
      label={s.label}
      value={p[k]}
      min={s.min}
      max={s.max}
      step={s.step}
      bipolar={s.bipolar}
      reset={s.reset}
      format={s.format}
      disabled={disabled}
      hint={hint}
      onChange={(v) => actions.setParams({ [k]: v } as Partial<DspParams>)}
    />
  );
}

function EffectCard({
  icon,
  title,
  description,
  enabled,
  onToggle,
  children,
}: {
  icon: ReactNode;
  title: string;
  description: string;
  enabled: boolean;
  onToggle: (v: boolean) => void;
  children: ReactNode;
}) {
  return (
    <Panel
      className={`effect${enabled ? ' is-on' : ''}`}
      icon={icon}
      title={title}
      actions={<Toggle checked={enabled} onChange={onToggle} label={`${title} enabled`} />}
    >
      <p className="effect__desc">{description}</p>
      <div className={`effect__body${enabled ? '' : ' is-dimmed'}`}>{children}</div>
    </Panel>
  );
}

export function Effects({ state }: { state: AppState }) {
  const p = state.params;
  const out = state.engine.output;
  const headphoneRender = p.surroundMode === 'headphones' || (p.surroundMode === 'auto' && !!out?.headphoneLike);
  const autoTarget = out ? (out.headphoneLike ? `binaural (HRTF) for ${out.name}` : `wide stereo for ${out.name}`) : 'chosen from the output device';

  return (
    <div className="effects">
      <ScreenHeader eyebrow="Processing chain" title="Sound Effects" />
      <div className="effects-grid">
        <EffectCard
          icon={<IconBass size={18} />}
          title="Bass"
          description="Low-frequency shelf, sub-bass focus and harmonic bass that lets small drivers suggest notes they cannot physically play."
          enabled={p.bassEnabled}
          onToggle={(v) => actions.setParams({ bassEnabled: v })}
        >
          <P k="bassBoostDb" p={p} />
          <P k="bassFreq" p={p} />
          <P k="subBassDb" p={p} />
          <P k="bassHarmonics" p={p} hint="Psychoacoustic bass: adds harmonics of low notes so they are perceived on laptop speakers and earbuds." />
          <ToggleRow
            title="Bass protection"
            description="Dynamic low-frequency limiter and device-aware high-pass to prevent distortion."
            checked={p.bassProtect}
            onChange={(v) => actions.setParams({ bassProtect: v })}
          />
        </EffectCard>

        <EffectCard
          icon={<IconVoice size={18} />}
          title="Dialogue"
          description="Lifts the centre-panned voice range and lets you pull background music and effects down behind speech."
          enabled={p.dialogueEnabled}
          onToggle={(v) => actions.setParams({ dialogueEnabled: v })}
        >
          <P k="dialogueAmount" p={p} />
          <P k="backgroundDb" p={p} />
        </EffectCard>

        <EffectCard
          icon={<IconSurround size={18} />}
          title="Surround"
          description="Virtual surround with 3D height: binaural (HRTF) rendering with overhead speakers on headphones, crosstalk-aware widening on speakers. Real 5.1 / 7.1 / 7.1.4 input is placed channel by channel."
          enabled={p.surroundEnabled}
          onToggle={(v) => actions.setParams({ surroundEnabled: v })}
        >
          <div className="field">
            <div className="field__label">Rendering mode</div>
            <Segmented<SurroundMode>
              label="Surround rendering mode"
              value={p.surroundMode}
              onChange={(v) => actions.setParams({ surroundMode: v })}
              options={[
                { value: 'auto', label: 'Auto', hint: 'Pick from the output device type' },
                { value: 'headphones', label: 'Headphones', hint: 'Binaural HRTF rendering' },
                { value: 'speakers', label: 'Speakers', hint: 'Stereo widening for speakers' },
              ]}
            />
            {p.surroundMode === 'auto' && <div className="field__note">Auto is using {autoTarget}.</div>}
          </div>
          <ToggleRow
            title="Atmos-style 7.1.4 up-mix"
            description="Splits stereo frequency by frequency into 11 virtual speakers: voices on a real centre speaker, ambience around and above you (4 ceiling speakers), film surround decoded, plus reflections from each speaker's direction on headphones. Adds about 11 ms of delay."
            checked={p.immersive}
            onChange={(v) => actions.setParams({ immersive: v })}
          />
          <P k="surroundIntensity" p={p} />
          <P k="stereoWidth" p={p} />
          <P k="roomAmount" p={p} />
          <P k="roomSize" p={p} />
          <P k="heightAmount" p={p} />
          {headphoneRender && <RealEars state={state} />}
        </EffectCard>

        <EffectCard
          icon={<IconBass size={18} />}
          title="Smart EQ"
          description="Dolby-style intelligent equalizer: listens to what is playing and slowly steers its tonal balance to a target, so thin recordings get body and dull ones get air. Well-balanced material barely changes."
          enabled={p.ieqEnabled}
          onToggle={(v) => actions.setParams({ ieqEnabled: v })}
        >
          <div className="field">
            <div className="field__label">Target sound</div>
            <Segmented<IeqProfile>
              label="Smart EQ target"
              value={p.ieqProfile}
              onChange={(v) => actions.setParams({ ieqProfile: v })}
              options={[
                { value: 'balanced', label: 'Balanced', hint: 'The tonal balance of a typical professional mix' },
                { value: 'warm', label: 'Warm', hint: 'More body, softer top' },
                { value: 'detailed', label: 'Detailed', hint: 'More presence and air' },
              ]}
            />
          </div>
          <P k="ieqAmount" p={p} />
        </EffectCard>

        <EffectCard
          icon={<IconBass size={18} />}
          title="Volume-aware tone"
          description="Our ears lose bass (and some top) faster than mid-range as sound gets quieter, so mixes sound thin at low volume. This follows your Windows volume and adds back what a lower volume takes away: nothing near full volume, up to +9 dB of bass and +3 dB of top when it is very quiet."
          enabled={p.volumeModeler}
          onToggle={(v) => actions.setParams({ volumeModeler: v })}
        >
          <div className="field__note">
            {state.engine.output ? 'Follows the volume of ' + state.engine.output.name + '.' : 'Follows the output device volume.'}
          </div>
        </EffectCard>

        <EffectCard
          icon={<IconDynamics size={18} />}
          title="Dynamics"
          description="Compression evens out loud explosions and quiet whispers; loudness normalisation keeps different sources at a similar level."
          enabled={p.dynamicsEnabled}
          onToggle={(v) => actions.setParams({ dynamicsEnabled: v })}
        >
          <P k="compressorAmount" p={p} />
          <ToggleRow
            title="Loudness normalisation"
            description="Slowly matches program loudness to the target."
            checked={p.loudnessEnabled}
            onChange={(v) => actions.setParams({ loudnessEnabled: v })}
          />
          <P k="loudnessTargetLufs" p={p} disabled={!p.loudnessEnabled} />
          <P k="limiterCeilingDb" p={p} />
          <div className="field__note">The peak limiter is always active, whatever these switches say, so the output never clips.</div>
        </EffectCard>
      </div>
    </div>
  );
}
