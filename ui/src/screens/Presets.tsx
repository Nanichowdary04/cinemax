import type { AppState, PresetId } from '../bridge/types';
import type { BusyKey } from '../bridge/store';
import { actions } from '../bridge/useCinemax';
import { ConfirmButton, ScreenHeader } from '../components/Chrome';
import { DeviceIcon, IconRefresh, PresetGlyph } from '../components/Icons';

const ORDER: PresetId[] = ['cinema', 'imax', 'dolby-style', 'theatre', 'concert-hall', 'music', 'deep-bass', 'gaming', 'dialogue', 'night', 'custom'];

export function Presets({ state, busy }: { state: AppState; busy: Partial<Record<BusyKey, boolean>> }) {
  const active = state.params.preset;
  const byId = new Map(state.presets.map((p) => [p.id, p]));
  const tiles = ORDER.map((id) => byId.get(id)).filter((p): p is NonNullable<typeof p> => !!p);
  // Any preset the engine offers that isn't in our ordering (except neutral) still gets a tile.
  for (const p of state.presets) if (!ORDER.includes(p.id) && p.id !== 'neutral') tiles.push(p);
  const neutral = byId.get('neutral');
  const out = state.engine.output;

  return (
    <div className="presets">
      <ScreenHeader eyebrow="Sound profiles" title="Presets">
        {out && (
          <span className="preset-device">
            <DeviceIcon kind={out.kind} size={16} />
            Profile for <strong>{out.name}</strong>
          </span>
        )}
      </ScreenHeader>

      <div className="preset-grid" role="radiogroup" aria-label="Presets">
        {tiles.map((p) => {
          const isActive = p.id === active;
          return (
            <button
              key={p.id}
              type="button"
              role="radio"
              aria-checked={isActive}
              className={`preset${isActive ? ' is-active' : ''}`}
              disabled={!!busy.preset}
              onClick={() => actions.applyPreset(p.id)}
            >
              <span className="preset__glyph">
                <PresetGlyph id={p.id} />
              </span>
              <span className="preset__name">{p.name}</span>
              <span className="preset__tagline">{p.tagline}</span>
              <span className="preset__state">{isActive ? 'Active' : 'Select'}</span>
            </button>
          );
        })}
      </div>

      <div className="preset-foot">
        {neutral && (
          <button
            type="button"
            className={`preset-flat${active === 'neutral' ? ' is-active' : ''}`}
            onClick={() => actions.applyPreset('neutral')}
            disabled={!!busy.preset}
            aria-pressed={active === 'neutral'}
          >
            <PresetGlyph id="neutral" size={28} />
            <span>
              <span className="preset-flat__name">Flat / Neutral</span>
              <span className="preset-flat__tag">{neutral.tagline}</span>
            </span>
          </button>
        )}
        <div className="preset-foot__note">
          <p>
            Presets are starting points. Any change you make afterwards is kept as <strong>CUSTOM</strong> for this device. Labels describe a
            listening style — they are not Dolby or IMAX certifications.
          </p>
          <ConfirmButton icon={<IconRefresh size={16} />} confirmLabel="Reset profile" onConfirm={actions.resetProfile} disabled={!!busy.reset}>
            Reset this device's profile
          </ConfirmButton>
        </div>
      </div>
    </div>
  );
}
