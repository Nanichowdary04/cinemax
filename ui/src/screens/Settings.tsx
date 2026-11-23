import type { AppState, PresetId } from '../bridge/types';
import type { BusyKey } from '../bridge/store';
import { actions } from '../bridge/useCinemax';
import { ConfirmButton, ScreenHeader } from '../components/Chrome';
import { Button, Panel, Segmented, ToggleRow } from '../components/Controls';
import { IconExternal, IconRefresh, IconShield } from '../components/Icons';
import { fmtChannels, fmtMs, fmtRate } from '../lib/format';

const BUFFERS = [10, 20, 40, 80];

const NEW_DEVICE_PRESETS: { value: PresetId; label: string }[] = [
  { value: 'cinema', label: 'Cinema' },
  { value: 'imax', label: 'IMAX-style' },
  { value: 'theatre', label: 'Theatre' },
  { value: 'music', label: 'Music' },
  { value: 'concert-hall', label: 'Concert hall' },
  { value: 'neutral', label: 'Flat' },
];

const BUFFER_NOTES: Record<number, string> = {
  10: 'Lowest delay. Best for games and lip-sync; may crackle on busy or older PCs.',
  20: 'Balanced. Recommended for most PCs.',
  40: 'Extra safety margin against dropouts, still fine for video.',
  80: 'Maximum stability for heavily loaded systems. Noticeable delay in games.',
};

export function SettingsScreen({ state, busy }: { state: AppState; busy: Partial<Record<BusyKey, boolean>> }) {
  const s = state.settings;
  const e = state.engine;
  const running = e.status === 'running';
  const set = actions.setSettings;
  const bufferValue = BUFFERS.includes(s.bufferMs) ? s.bufferMs : 20;

  return (
    <div className="settings">
      <ScreenHeader eyebrow="Preferences" title="Settings" />

      <div className="settings-grid">
        <Panel eyebrow="Startup & window" title="General">
          <ToggleRow title="Launch on startup" description="Start CINEMAX in the system tray when you sign in to Windows." checked={s.launchOnStartup} onChange={(v) => set({ launchOnStartup: v })} />
          <ToggleRow title="Minimize to system tray" description="Closing the window keeps the engine running in the tray." checked={s.minimizeToTray} onChange={(v) => set({ minimizeToTray: v })} />
          <ToggleRow title="Start engine on launch" description="Turn processing on automatically when CINEMAX starts." checked={s.startEngineOnLaunch} onChange={(v) => set({ startEngineOnLaunch: v })} />
        </Panel>

        <Panel eyebrow="Devices" title="Switching">
          <ToggleRow
            title="Automatic device switching"
            description="Follow newly connected headphones or earbuds and fall back safely when a Bluetooth device disconnects."
            checked={s.autoSwitchDevices}
            onChange={(v) => set({ autoSwitchDevices: v })}
          />
          <ToggleRow
            title="Follow Windows volume"
            description="Compensate the virtual device's volume so the Windows volume keys behave normally while processing."
            checked={s.compensateWindowsVolume}
            onChange={(v) => set({ compensateWindowsVolume: v })}
          />
        </Panel>

        <Panel eyebrow="Music · Movies · Auto" title="Listening modes" className="settings-wide">
          <div className="field">
            <div className="field__label">Music mode uses</div>
            <Segmented<PresetId>
              label="Preset for Music mode"
              value={s.musicPreset === 'music' ? 'music' : 'concert-hall'}
              onChange={(v) => set({ musicPreset: v })}
              options={[
                { value: 'concert-hall', label: 'Concert hall' },
                { value: 'music', label: 'Music (studio)' },
              ]}
            />
          </div>
          <div className="field">
            <div className="field__label">Movies mode uses</div>
            <Segmented<PresetId>
              label="Preset for Movies mode"
              value={['cinema', 'imax', 'theatre'].includes(s.moviePreset) ? s.moviePreset : 'cinema'}
              onChange={(v) => set({ moviePreset: v })}
              options={[
                { value: 'cinema', label: 'Cinema' },
                { value: 'imax', label: 'IMAX-style' },
                { value: 'theatre', label: 'Theatre' },
              ]}
            />
            <div className="field__note">
              Auto first looks at which app or site is playing (e.g. VLC, Netflix, Prime Video, Hotstar → Movies; Spotify, YouTube Music,
              JioSaavn → Music) and switches within about 2 seconds. Websites are recognised from the browser window's title, so keep the
              playing tab in front. For anything else (e.g. plain YouTube) it analyses the sound — speech rhythm, pauses, loudness swings,
              surround channels — which takes 10–15 seconds and can misjudge music videos or podcasts. Pick Music or Movies to fix the choice.
              Nothing about what you play is stored or sent anywhere.
            </div>
          </div>
          <div className="field">
            <div className="field__label">Games use (Auto, with Picture → Switch the sound too)</div>
            <Segmented<PresetId>
              label="Preset while a game is in front"
              value={['gaming', 'imax', 'cinema'].includes(s.gamePreset) ? s.gamePreset : 'gaming'}
              onChange={(v) => set({ gamePreset: v })}
              options={[
                { value: 'gaming', label: 'Gaming' },
                { value: 'imax', label: 'IMAX-style' },
                { value: 'cinema', label: 'Cinema' },
              ]}
            />
          </div>
          <ToggleRow
            title="Smart tone (automatic bass & treble)"
            description="Compares each song or film with the balance of well-mastered recordings and gently corrects bass and treble (at most +5 / −4 dB, changing no faster than 1 dB per second). Shown live on the Dashboard."
            checked={s.smartTone}
            onChange={(v) => set({ smartTone: v })}
          />
        </Panel>

        <Panel eyebrow="First sound" title="When CINEMAX turns on" className="settings-wide">
          <div className="field">
            <div className="field__label">Sound for devices without saved settings</div>
            <Segmented<PresetId>
              label="Sound for devices without saved settings"
              value={NEW_DEVICE_PRESETS.some((o) => o.value === s.newDevicePreset) ? s.newDevicePreset : 'cinema'}
              onChange={(v) => set({ newDevicePreset: v })}
              options={NEW_DEVICE_PRESETS}
            />
            <div className="field__note">
              Used the first time each speaker or headset plays through CINEMAX. After you pick a preset or adjust anything, that device keeps
              its own settings.
            </div>
          </div>
        </Panel>

        <Panel eyebrow="Audio quality" title="Engine" className="settings-wide">
          <div className="field">
            <div className="field__label">Buffer size</div>
            <Segmented<number>
              label="Buffer size"
              value={bufferValue}
              onChange={(v) => set({ bufferMs: v })}
              options={BUFFERS.map((b) => ({ value: b, label: `${b} ms` }))}
            />
            <div className="field__note">
              {BUFFER_NOTES[bufferValue]} Changes apply by restarting the audio stream for a moment.
              {running && e.bufferMs > 0 && <> Windows granted {fmtMs(e.bufferMs, 1)}.</>}
            </div>
          </div>
          <ToggleRow
            title="High-quality resampling"
            description="Higher-order resampler when the virtual device and your output run at different sample rates. Slightly more CPU."
            checked={s.highQualityResampling}
            onChange={(v) => set({ highQualityResampling: v })}
          />
        </Panel>

        <Panel eyebrow="Read-only" title="Audio format" className="settings-wide">
          {running ? (
            <dl className="format-grid">
              <div>
                <dt>Source</dt>
                <dd>{e.source?.name ?? '—'}</dd>
              </div>
              <div>
                <dt>Input</dt>
                <dd>
                  {fmtRate(e.inputSampleRate)} · {fmtChannels(e.inputChannels)}
                </dd>
              </div>
              <div>
                <dt>Output device</dt>
                <dd>{e.output?.name ?? '—'}</dd>
              </div>
              <div>
                <dt>Output</dt>
                <dd>
                  {fmtRate(e.outputSampleRate)} · {fmtChannels(e.outputChannels)}
                </dd>
              </div>
              <div>
                <dt>Render buffer</dt>
                <dd>{fmtMs(e.bufferMs, 1)}</dd>
              </div>
              <div>
                <dt>Added latency</dt>
                <dd>{fmtMs(e.latencyMs, 0)}</dd>
              </div>
            </dl>
          ) : (
            <div className="list-empty">Sample rate, channels and latency are shown while the engine is running.</div>
          )}
        </Panel>

        <Panel eyebrow="Recovery" title="Safety" className="settings-wide safety">
          <div className="safety__row">
            <div>
              <div className="row__title">Restore default audio routing</div>
              <div className="row__desc">
                Turns processing off and sets your original speakers or headphones back as the Windows default. Use this if sound ever goes
                missing.
              </div>
            </div>
            <Button variant="primary" icon={<IconShield size={16} />} onClick={actions.restoreDefaultRouting} disabled={!!busy.routing}>
              Restore routing
            </Button>
          </div>
          <div className="safety__row">
            <div>
              <div className="row__title">Windows sound settings</div>
              <div className="row__desc">Open the system Sound page to check default devices and volumes.</div>
            </div>
            <Button variant="ghost" icon={<IconExternal size={16} />} onClick={actions.openWindowsSoundSettings}>
              Open
            </Button>
          </div>
          <div className="safety__row">
            <div>
              <div className="row__title">Reset settings</div>
              <div className="row__desc">Restore every setting on this page to its default. Device sound profiles are kept.</div>
            </div>
            <ConfirmButton variant="ghost" icon={<IconRefresh size={16} />} confirmLabel="Reset all settings" onConfirm={actions.resetSettings} disabled={!!busy.settings}>
              Reset settings
            </ConfirmButton>
          </div>
        </Panel>
      </div>
      <p className="credit">Developed by Nani Chowdary</p>
    </div>
  );
}
