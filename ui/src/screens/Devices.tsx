import type { ReactNode } from 'react';
import type { AppState, DeviceInfo } from '../bridge/types';
import { VB_CABLE_URL } from '../bridge/types';
import { api } from '../bridge/bridge';
import type { BusyKey } from '../bridge/store';
import { actions } from '../bridge/useCinemax';
import { ScreenHeader } from '../components/Chrome';
import { Badge, Button, Panel } from '../components/Controls';
import {
  DeviceIcon,
  IconApps,
  IconBluetooth,
  IconCheck,
  IconDevices,
  IconExternal,
  IconVirtual,
  IconWarning,
  isBluetooth,
} from '../components/Icons';
import { fmtChannels, fmtRate } from '../lib/format';

export function Devices({ state, busy }: { state: AppState; busy: Partial<Record<BusyKey, boolean>> }) {
  const e = state.engine;
  const outputs = state.devices.filter((d) => !d.isVirtual);
  const virtuals = state.devices.filter((d) => d.isVirtual);
  const preferred = state.settings.preferredVirtualDeviceId;
  const running = e.status === 'running';

  return (
    <div className="devices">
      <ScreenHeader eyebrow="Routing" title="Audio Devices">
        <Button variant="ghost" icon={<IconExternal size={16} />} onClick={actions.openWindowsSoundSettings}>
          Windows sound settings
        </Button>
      </ScreenHeader>

      {(e.status === 'no-virtual-device' || virtuals.length === 0) && <VirtualSetup />}

      <Panel className="route-panel" eyebrow="How CINEMAX hears your PC" title="Signal routing">
        <div className="route">
          <RouteNode icon={<IconApps size={20} />} title="Your apps" sub="Browsers, players, games" active={running} />
          <RouteLink active={running} />
          <RouteNode icon={<IconVirtual size={20} />} title={e.source?.name ?? 'Virtual device'} sub="Windows default output" active={running} />
          <RouteLink active={running} />
          <RouteNode icon={<span className="route__cmx">C</span>} title="CINEMAX engine" sub={running ? (e.bypass ? 'Bypassed' : 'Processing') : 'Off'} active={running} accent />
          <RouteLink active={running} />
          <RouteNode
            icon={e.output ? <DeviceIcon kind={e.output.kind} size={20} /> : <IconDevices size={20} />}
            title={e.output?.name ?? 'No output device'}
            sub={e.output?.kindLabel ?? 'Connect speakers or headphones'}
            active={running}
          />
        </div>
        <p className="route__text">
          While the engine is on, Windows plays into a virtual device. CINEMAX captures that audio, processes it and sends it to your real
          speakers or headphones. Turning the engine off — or closing CINEMAX — puts your original default device back.
        </p>
      </Panel>

      <Panel
        eyebrow={`${outputs.length} connected`}
        title="Output devices"
        actions={<span className="panel__note">Each device keeps its own sound profile</span>}
      >
        {outputs.length === 0 ? (
          <div className="list-empty">No playback devices are active. Connect speakers or headphones.</div>
        ) : (
          <ul className="device-list">
            {outputs.map((d) => (
              <DeviceRow key={d.id} d={d} busy={!!busy.device} />
            ))}
          </ul>
        )}
      </Panel>

      <Panel eyebrow="Capture" title="Virtual source device">
        <p className="panel__lead">The virtual device Windows plays into while CINEMAX is on. Automatic picks the first supported one found.</p>
        <div className="source-list" role="radiogroup" aria-label="Virtual source device">
          <SourceOption
            checked={preferred === ''}
            title="Automatic"
            sub={e.source && preferred === '' ? `Using ${e.source.name}` : 'Detect a virtual cable automatically'}
            onSelect={() => actions.selectSourceDevice('')}
            disabled={!!busy.source}
          />
          {virtuals.map((d) => (
            <SourceOption
              key={d.id}
              checked={preferred === d.id}
              title={d.name}
              sub={`${fmtRate(d.sampleRate)} · ${fmtChannels(d.channels)}${d.isCinemaxSource ? ' · in use' : ''}`}
              onSelect={() => actions.selectSourceDevice(d.id)}
              disabled={!!busy.source}
            />
          ))}
        </div>
      </Panel>
    </div>
  );
}

function DeviceRow({ d, busy }: { d: DeviceInfo; busy: boolean }) {
  const handsFree = d.kind === 'bluetooth-handsfree';
  return (
    <li className={`device${d.isCinemaxOutput ? ' is-active' : ''}`}>
      <span className="device__icon">
        <DeviceIcon kind={d.kind} size={22} />
        {isBluetooth(d.kind) && (
          <span className="device__bt" title="Bluetooth">
            <IconBluetooth size={11} strokeWidth={2} />
          </span>
        )}
      </span>
      <div className="device__text">
        <div className="device__name" title={d.name}>
          {d.name}
        </div>
        <div className="device__meta">
          <span>{d.kindLabel}</span>
          {d.sampleRate > 0 && (
            <span>
              {fmtRate(d.sampleRate)} · {fmtChannels(d.channels)}
            </span>
          )}
          <span>{d.headphoneLike ? 'Binaural spatial' : 'Speaker spatial'}</span>
        </div>
        <div className="device__badges">
          {d.isCinemaxOutput && <Badge tone="accent">CINEMAX output</Badge>}
          {d.isWindowsDefault && <Badge tone="neutral">Windows default</Badge>}
          {d.hasProfile && <Badge tone="muted">Saved profile</Badge>}
          {handsFree && (
            <Badge tone="warn" title="Hands-free (HFP) is a narrow-band call profile. CINEMAX never selects it automatically.">
              Hands-free · low quality
            </Badge>
          )}
        </div>
      </div>
      <div className="device__action">
        {d.isCinemaxOutput ? (
          <span className="device__inuse">
            <IconCheck size={16} /> In use
          </span>
        ) : (
          <Button
            variant={handsFree ? 'quiet' : 'ghost'}
            disabled={busy}
            onClick={() => actions.selectOutputDevice(d.id)}
            title={handsFree ? 'Narrow-band call audio — music and films will sound poor' : 'Send CINEMAX output to this device'}
          >
            Use for output
          </Button>
        )}
      </div>
    </li>
  );
}

function SourceOption({ checked, title, sub, onSelect, disabled }: { checked: boolean; title: string; sub: string; onSelect: () => void; disabled?: boolean }) {
  return (
    <button type="button" role="radio" aria-checked={checked} className={`source${checked ? ' is-checked' : ''}`} onClick={onSelect} disabled={disabled}>
      <span className="source__radio" />
      <span className="source__text">
        <span className="source__title">{title}</span>
        <span className="source__sub">{sub}</span>
      </span>
    </button>
  );
}

function RouteNode({ icon, title, sub, active, accent }: { icon: ReactNode; title: string; sub: string; active: boolean; accent?: boolean }) {
  return (
    <div className={`route__node${active ? ' is-active' : ''}${accent ? ' is-accent' : ''}`}>
      <span className="route__icon">{icon}</span>
      <span className="route__title" title={title}>
        {title}
      </span>
      <span className="route__sub">{sub}</span>
    </div>
  );
}

function RouteLink({ active }: { active: boolean }) {
  return <span className={`route__link${active ? ' is-active' : ''}`} aria-hidden="true" />;
}

function VirtualSetup() {
  return (
    <Panel className="setup" icon={<IconWarning size={18} />} title="Set up a virtual audio device">
      <p className="panel__lead">
        Windows has no built-in way to let one app process all system sound. CINEMAX uses a free virtual audio cable as the hand-off point. No
        virtual device was found on this PC.
      </p>
      <ol className="steps">
        <li>
          <span className="steps__n">1</span>
          <span>
            Download <strong>VB-CABLE Virtual Audio Device</strong> from <span className="mono">vb-audio.com/Cable</span> (free, donationware).{' '}
            <Button variant="ghost" icon={<IconExternal size={14} />} onClick={() => { api.openExternalUrl(VB_CABLE_URL).catch(() => {}); }}>
              Open download page
            </Button>
          </span>
        </li>
        <li>
          <span className="steps__n">2</span>
          <span>
            Extract the zip, right-click <span className="mono">VBCABLE_Setup_x64.exe</span> → <em>Run as administrator</em> → <em>Install Driver</em>.
          </span>
        </li>
        <li>
          <span className="steps__n">3</span>
          <span>Restart CINEMAX (reboot if the installer asks). The cable is detected automatically and set up when you turn the engine on.</span>
        </li>
      </ol>
      <p className="field__note">Your current default speakers or headphones are remembered and restored whenever CINEMAX is turned off or closed.</p>
    </Panel>
  );
}
