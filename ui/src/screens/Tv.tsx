import type { AppState, DisplayView, TvAudioDevice, TvRange, TvSpeakers } from '../bridge/types';
import { actions } from '../bridge/useCinemax';
import type { ScreenId } from '../App';
import { ScreenHeader } from '../components/Chrome';
import { Badge, Button, Panel, Segmented, Slider, ToggleRow } from '../components/Controls';
import { IconPicture, IconTv } from '../components/Icons';

const yes = (ok: boolean) => (ok ? '✓' : '✗');

function TvSound({ d, state }: { d: TvAudioDevice; state: AppState }) {
  const s = d.setup;
  const set = (partial: Partial<typeof s>) => actions.tv.setSetup(d.id, partial);
  const anyDolby = d.dolby.dolbyDigital || d.dolby.dolbyDigitalPlus || d.dolby.atmos;
  const bluetooth = d.kind.startsWith('bluetooth');
  const passthrough = d.isOutput && state.engine.status === 'passthrough';

  return (
    <Panel
      className="tv-card"
      icon={<IconTv size={18} />}
      title="TV sound"
      eyebrow={`${d.name}${bluetooth ? ' · Bluetooth' : ''}`}
      actions={
        d.isOutput ? (
          <Badge tone="accent">{passthrough ? 'TV is decoding' : 'Playing on this TV'}</Badge>
        ) : (
          <Button variant="primary" onClick={() => void actions.selectOutputDevice(d.id)}>
            Play sound on this TV
          </Button>
        )
      }
    >
      <ToggleRow
        title="This output is a TV"
        description={
          bluetooth
            ? 'Turns on the TV tuning below and makes CINEMAX treat this Bluetooth device as room speakers (some TVs call themselves "headphones" over Bluetooth).'
            : "Turns on the TV tuning below. Switch it off for an HDMI monitor or an adapter that isn't a TV."
        }
        checked={s.isTv}
        onChange={(v) => set({ isTv: v })}
      />

      <div className={s.isTv ? '' : 'is-dimmed'}>
        <div className="field">
          <div className="field__label">What plays the sound</div>
          <Segmented<TvSpeakers>
            label="TV speakers"
            value={s.speakers}
            onChange={(v) => set({ speakers: v })}
            options={[
              { value: 'tv', label: 'TV speakers', hint: 'The speakers built into the TV' },
              { value: 'soundbar', label: 'Soundbar', hint: 'A soundbar connected to the TV' },
              { value: 'receiver', label: 'AV receiver', hint: 'Home theatre / AV receiver with real speakers' },
            ]}
          />
          <div className="field__note">
            {s.speakers === 'tv'
              ? 'TV speakers are small and often face down: CINEMAX protects them from deep bass, puts the body back around 160 Hz and moves voices forward so they stay clear across the room.'
              : s.speakers === 'soundbar'
                ? 'Soundbars are tuned already: CINEMAX only adds a touch of body and presence. Turn off the soundbar’s own surround / virtual modes so they don’t stack.'
                : 'Real speakers: no tuning, and the virtual surround is set for speakers about 60° apart.'}
          </div>
        </div>

        {s.speakers !== 'receiver' && (
          <Slider
            label="Screen size"
            value={s.sizeInch}
            min={24}
            max={100}
            step={1}
            reset={55}
            format={(v) => `${Math.round(v)}″`}
            onChange={(v) => set({ sizeInch: v })}
          />
        )}
        <Slider
          label="Where you sit"
          value={s.distanceM}
          min={1}
          max={6}
          step={0.1}
          reset={2.5}
          format={(v) => `${v.toFixed(1)} m from the TV`}
          onChange={(v) => set({ distanceM: v })}
        />
        <div className="field__note">
          From your seat the speakers are about <b>{d.speakerAngle}°</b> apart. CINEMAX sizes its virtual surround to
          that, so the sound reaches past the edges of the TV.
        </div>

        {bluetooth && (
          <div className="tv-bt">
            <b>Bluetooth to a TV</b>
            <ul>
              <li>
                Bluetooth sends plain stereo (SBC / AAC), so <b>Dolby can't travel over it</b> - the DOLBY-STYLE preset below is
                how you get the Dolby-like sound on the TV.
              </li>
              <li>
                Bluetooth adds about 0.2 s of delay. If you watch on the <b>laptop screen</b>, the sound arrives late and only the
                video player can fix that (VLC: press K / J to shift the sound; most players have an "audio delay" setting).
              </li>
              <li>If the picture is on the TV (cast / screen mirroring) and voices come early, use Lip-sync below.</li>
              <li>Keep the laptop within a few metres of the TV; walls and Wi-Fi traffic can make Bluetooth drop out.</li>
            </ul>
          </div>
        )}

        <Slider
          label="Lip-sync"
          value={s.lipSyncMs}
          min={0}
          max={300}
          step={5}
          reset={0}
          format={(v) => (v === 0 ? 'Off' : `+${Math.round(v)} ms`)}
          onChange={(v) => set({ lipSyncMs: v })}
          hint="Delays the sound. Use it when the picture runs late (TV picture processing, screen mirroring) and voices come before the lips move."
        />

        {!bluetooth && (
        <div className="tv-dolby">
          <div className="field__label">Dolby this TV can decode</div>
          {d.dolby.probed ? (
            <div className="tv-dolby__row">
              <span className={d.dolby.dolbyDigital ? 'is-yes' : 'is-no'}>{yes(d.dolby.dolbyDigital)} Dolby Digital</span>
              <span className={d.dolby.dolbyDigitalPlus ? 'is-yes' : 'is-no'}>{yes(d.dolby.dolbyDigitalPlus)} Dolby Digital Plus</span>
              <span className={d.dolby.atmos ? 'is-yes' : 'is-no'}>{yes(d.dolby.atmos)} Dolby Atmos</span>
            </div>
          ) : (
            <div className="field__note">Couldn’t ask the TV (it may be asleep). Reconnect it or switch it on.</div>
          )}
          <ToggleRow
            title="Let the TV decode Dolby"
            description={
              anyDolby
                ? 'For films with a real Dolby track: CINEMAX steps aside while this TV is the output, so the movie app can send the original Dolby sound and the TV decodes it. CINEMAX’s own effects are off meanwhile; it comes back as soon as you switch to another output.'
                : 'This TV didn’t report any Dolby support, so the films would play plain stereo. Keep CINEMAX processing on for the best sound.'
            }
            checked={s.letTvDecode}
            onChange={(v) => set({ letTvDecode: v })}
          />
        </div>
        )}

        {(bluetooth || !s.letTvDecode) && (
          <div className="tv-tip">
            <span>For the Dolby-like sound through CINEMAX, use the DOLBY-STYLE preset on this TV and set the TV’s own sound mode to Standard (its virtual surround off).</span>
            <Button
              variant="ghost"
              disabled={!d.isOutput || state.params.preset === 'dolby-style'}
              onClick={() => void actions.applyPreset('dolby-style')}
            >
              {state.params.preset === 'dolby-style' && d.isOutput ? 'DOLBY-STYLE is on' : 'Use DOLBY-STYLE'}
            </Button>
          </div>
        )}
      </div>
    </Panel>
  );
}

function TvPictureCard({ d, onGo }: { d: DisplayView; onGo: (s: ScreenId) => void }) {
  const t = d.tv;
  const set = (partial: Partial<Pick<typeof t, 'range' | 'contentAuto' | 'gameMode'>>) => void actions.tv.setPicture(d.key, partial);
  const sending = { default: 'nothing', off: 'nothing', desktop: 'Desktop', film: 'Film', game: 'Game' }[t.sentContent];

  return (
    <Panel
      className="tv-card"
      icon={<IconPicture size={18} />}
      title="TV picture"
      eyebrow={`${d.name} · ${d.width}×${d.height} @ ${Math.round(d.refreshHz)} Hz`}
      actions={
        <Button variant="ghost" onClick={() => onGo('picture')}>
          Picture modes
        </Button>
      }
    >
      <p className="effect__desc">
        CINEMAX Vision already processes this TV’s picture (modes, colour, sharpness) on the Picture screen. These are
        extra HDMI settings for TVs.
      </p>
      {!t.available ? (
        <div className="field__note">
          Your graphics driver or this TV doesn’t accept HDMI picture signalling, so these controls aren’t available.
          {d.backend !== 'intel' && ' (They need Intel graphics.)'}
        </div>
      ) : (
        <>
          {t.canRange && (
            <div className="field">
              <div className="field__label">Black level (HDMI range)</div>
              <Segmented<TvRange>
                label="HDMI range"
                value={t.range}
                onChange={(v) => set({ range: v })}
                options={[
                  { value: 'auto', label: 'Automatic', hint: 'Let the graphics driver decide' },
                  { value: 'full', label: 'Full 0–255', hint: 'Use when the TV is set to Full / High / Normal black level' },
                  { value: 'limited', label: 'Limited 16–235', hint: 'Use when the TV is set to Limited / Low black level' },
                ]}
              />
              <div className="field__note">
                Greyish blacks and a washed-out picture, or black shadows with no detail, mean the laptop and the TV
                disagree. Pick the one that matches the TV’s own “HDMI black level” setting (Full ↔ Full, Limited ↔ Limited).
              </div>
            </div>
          )}
          {t.canContentType && (
            <>
              <ToggleRow
                title="Tell the TV what’s on"
                description="Sends the TV whether a film, a game or the desktop is in front (from CINEMAX’s Auto scene), so a TV that supports it switches its own picture mode to match."
                checked={t.contentAuto}
                onChange={(v) => set({ contentAuto: v })}
              />
              {t.contentAuto && <div className="field__note">Now telling the TV: <b>{sending}</b></div>}
            </>
          )}
          {t.canGameMode && (
            <>
              <ToggleRow
                title="TV game mode while you play"
                description="Asks the TV for its low-latency game mode (ALLM) while a game is in front, and lets it go back afterwards."
                checked={t.gameMode}
                onChange={(v) => set({ gameMode: v })}
              />
              {t.gameModeActive && <div className="field__note"><Badge tone="accent">Game mode requested</Badge></div>}
            </>
          )}
        </>
      )}
    </Panel>
  );
}

function PickTv({ state }: { state: AppState }) {
  const listed = new Set((state.tv?.devices ?? []).map((d) => d.id));
  const others = state.devices.filter((d) => !d.isVirtual && !listed.has(d.id));
  if (others.length === 0) return null;
  return (
    <div className="tv-pick">
      <span>My TV isn’t listed:</span>
      <select
        className="pic-select"
        aria-label="Mark an output as your TV"
        value=""
        onChange={(e) => e.target.value && actions.tv.setSetup(e.target.value, { isTv: true })}
      >
        <option value="">Choose the output that is your TV…</option>
        {others.map((d) => (
          <option key={d.id} value={d.id}>
            {d.name} ({d.kindLabel})
          </option>
        ))}
      </select>
    </div>
  );
}

export function TvScreen({ state, onGo }: { state: AppState; onGo: (s: ScreenId) => void }) {
  const audio = state.tv?.devices ?? [];
  const screens = (state.vision?.displays ?? []).filter((d) => d.tv?.isTv);

  return (
    <div className="tv">
      <ScreenHeader eyebrow="Big screen" title="TV" />
      {audio.length === 0 && screens.length === 0 ? (
        <Panel icon={<IconTv size={18} />} title="Connect your TV">
          <ol className="tv-steps">
            <li>
              <b>Bluetooth:</b> on the TV, turn on receiving sound from other devices (Samsung: <i>Sound Mirroring</i>; LG:{' '}
              <i>Bluetooth sound sharing</i> or <i>Sound Share</i>; many Android / Google TVs can't receive). Then pair it in
              Windows Settings &gt; Bluetooth &amp; devices.
            </li>
            <li>
              <b>HDMI:</b> connect the cable and pick that HDMI input on the TV - you also get the TV picture settings.
            </li>
            <li>The TV appears here by itself when its name looks like a TV. If it doesn’t, choose it below.</li>
          </ol>
          <PickTv state={state} />
        </Panel>
      ) : (
        <div className="tv-grid">
          {audio.map((d) => (
            <TvSound key={d.id} d={d} state={state} />
          ))}
          {screens.map((d) => (
            <TvPictureCard key={d.key} d={d} onGo={onGo} />
          ))}
          <div className="tv-grid__wide">
            <PickTv state={state} />
          </div>
          {audio.length === 0 && (
            <Panel icon={<IconTv size={18} />} title="TV sound">
              <div className="field__note">
                The TV picture is connected but Windows doesn’t list an HDMI sound output for it. Check that the TV’s
                speakers are on, or use a soundbar / headphones.
              </div>
            </Panel>
          )}
        </div>
      )}
    </div>
  );
}
