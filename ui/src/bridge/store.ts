// ============================================================================
//  App-wide store for engine state, notices and meters.
//
//  * AppState lives in a tiny external store consumed via useSyncExternalStore.
//  * Meters (~30 Hz) are kept OUT of React state: canvases read meterStore in
//    their own requestAnimationFrame loop; text readouts poll at a low rate.
//  * Slider edits are applied optimistically and sent with a ~30 ms trailing
//    throttle. Responses / state events never overwrite params that the user
//    is still dragging.
// ============================================================================

import { api, isConnected, subscribe as subscribeEvent } from './bridge';
import {
  EVENT_METERS,
  EVENT_NOTICE,
  EVENT_STATE,
  type AppState,
  type DspParams,
  type EqBand,
  type ListeningMode,
  type Meters,
  type Notice,
  type PictureParams,
  type PicturePresetId,
  type PresetId,
  type Settings,
  type VisionSettings,
  type DisplayView,
  type TvSetup,
  type TvPicture,
} from './types';

export type ConnectionStatus = 'disconnected' | 'connecting' | 'ready' | 'failed';

export interface ToastNotice extends Notice {
  id: number;
}

export interface Snapshot {
  connection: ConnectionStatus;
  error: string | null;
  state: AppState | null;
  notices: ToastNotice[];
  busy: Partial<Record<BusyKey, boolean>>;
}

export type BusyKey = 'enabled' | 'bypass' | 'preset' | 'device' | 'source' | 'routing' | 'settings' | 'reset' | 'vision' | 'tv';

// ---------------------------------------------------------------------------
let snapshot: Snapshot = {
  connection: isConnected ? 'connecting' : 'disconnected',
  error: null,
  state: null,
  notices: [],
  busy: {},
};

const listeners = new Set<() => void>();

function emit(next: Partial<Snapshot>) {
  snapshot = { ...snapshot, ...next };
  listeners.forEach((l) => l());
}

export function subscribeStore(fn: () => void) {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

export function getSnapshot(): Snapshot {
  return snapshot;
}

// ---------------------------------------------------------------------------
//  Notices
let noticeId = 1;

export function pushNotice(level: Notice['level'], message: string) {
  const n: ToastNotice = { id: noticeId++, level, message };
  // Collapse identical consecutive notices.
  const last = snapshot.notices[snapshot.notices.length - 1];
  if (last && last.message === message && last.level === level) return;
  emit({ notices: [...snapshot.notices.slice(-4), n] });
  window.setTimeout(() => dismissNotice(n.id), level === 'error' ? 10000 : level === 'warning' ? 8000 : 5000);
}

export function dismissNotice(id: number) {
  if (!snapshot.notices.some((n) => n.id === id)) return;
  emit({ notices: snapshot.notices.filter((n) => n.id !== id) });
}

function reportError(context: string, e: unknown) {
  const msg = e instanceof Error ? e.message : String(e);
  pushNotice('error', `${context}: ${msg}`);
}

function setBusy(key: BusyKey, on: boolean) {
  emit({ busy: { ...snapshot.busy, [key]: on } });
}

// ---------------------------------------------------------------------------
//  Meters (not React state)
export const meterStore = {
  latest: null as Meters | null,
  receivedAt: 0, // performance.now() of the latest frame
  seq: 0,
  lastLimitingAt: -1e9, // latched flags so short events stay visible
  lastClipAt: -1e9,
};

/** Meters older than this are treated as "no signal" by visual components. */
export const METER_STALE_MS = 400;

/** A frame arrived recently (engine is streaming meters). */
export function metersAreFresh(now = performance.now()): boolean {
  return !!meterStore.latest && now - meterStore.receivedAt < METER_STALE_MS;
}

/** A fresh frame exists AND audio is actually flowing through the engine. */
export function metersAreLive(now = performance.now()): boolean {
  return metersAreFresh(now) && !!meterStore.latest?.active;
}

// ---------------------------------------------------------------------------
//  Param editing with optimistic updates + trailing throttle
const THROTTLE_MS = 30;
const PROTECT_MS = 400;

let pendingParams: Partial<DspParams> | null = null;
let pendingBands = new Map<number, Partial<EqBand>>();
let flushTimer: number | null = null;
let inflight = 0;
let sendGeneration = 0;
let lastLocalEdit = 0;

function editsInProgress() {
  return (
    pendingParams !== null ||
    pendingBands.size > 0 ||
    flushTimer !== null ||
    inflight > 0 ||
    performance.now() - lastLocalEdit < PROTECT_MS
  );
}

function applyState(incoming: AppState) {
  const current = snapshot.state;
  let next = incoming;
  if (current && editsInProgress()) next = { ...next, params: current.params };
  // Picture sliders being dragged: keep the local values until the edit settles.
  if (current?.vision && next.vision && visionEditing()) {
    next = {
      ...next,
      vision: {
        ...next.vision,
        displays: next.vision.displays.map((d) => {
          const c = current.vision.displays.find((x) => x.key === d.key);
          return c ? { ...d, params: c.params, activePreset: c.activePreset, mode: c.mode, backlight: c.backlight } : d;
        }),
      },
    };
  }
  emit({ state: next });
}

function applyParams(params: DspParams) {
  if (!snapshot.state) return;
  emit({ state: { ...snapshot.state, params } });
}

function patchLocalParams(patch: Partial<DspParams>) {
  const s = snapshot.state;
  if (!s) return;
  emit({ state: { ...s, params: { ...s.params, ...patch } } });
}

function scheduleFlush() {
  if (flushTimer !== null) return;
  flushTimer = window.setTimeout(flush, THROTTLE_MS);
}

function track(p: Promise<DspParams>) {
  inflight++;
  const gen = ++sendGeneration;
  p.then((result) => {
    inflight--;
    if (gen === sendGeneration && !editsInProgressIgnoringTime() && result) applyParams(result);
  }).catch((e) => {
    inflight--;
    reportError('Could not apply setting', e);
    void resync();
  });
}

function editsInProgressIgnoringTime() {
  return pendingParams !== null || pendingBands.size > 0 || flushTimer !== null || inflight > 0;
}

function flush() {
  flushTimer = null;
  const p = pendingParams;
  const bands = pendingBands;
  pendingParams = null;
  pendingBands = new Map();
  if (p) track(api.setParams(p));
  bands.forEach((b, i) => track(api.setEqBand(i, b)));
}

/** Change one or more DSP params (optimistic, throttled). Marks preset as custom. */
export function setParams(partial: Partial<DspParams>) {
  if (!snapshot.state || !isConnected) return;
  lastLocalEdit = performance.now();
  const withPreset: Partial<DspParams> = partial.preset ? partial : { ...partial, preset: 'custom' };
  patchLocalParams(withPreset);
  // Only forward the keys the user actually changed (backend sets 'custom' itself).
  pendingParams = { ...(pendingParams ?? {}), ...partial };
  scheduleFlush();
}

/** Change one EQ band (optimistic, throttled). */
export function setEqBand(index: number, band: Partial<EqBand>) {
  const s = snapshot.state;
  if (!s || !isConnected) return;
  lastLocalEdit = performance.now();
  const eqBands = s.params.eqBands.map((b, i) => (i === index ? { ...b, ...band } : b));
  patchLocalParams({ eqBands, preset: 'custom' });
  pendingBands.set(index, { ...(pendingBands.get(index) ?? {}), ...band });
  scheduleFlush();
}

function dropPendingEdits() {
  if (flushTimer !== null) window.clearTimeout(flushTimer);
  flushTimer = null;
  pendingParams = null;
  pendingBands = new Map();
}

// ---------------------------------------------------------------------------
//  Actions
async function runAction<T>(key: BusyKey, context: string, fn: () => Promise<T>, onResult: (r: T) => void) {
  if (!isConnected) return;
  setBusy(key, true);
  try {
    const r = await fn();
    if (r !== undefined && r !== null) onResult(r);
  } catch (e) {
    reportError(context, e);
    void resync();
  } finally {
    setBusy(key, false);
  }
}

export function setEnabled(on: boolean) {
  const s = snapshot.state;
  if (s) {
    emit({
      state: {
        ...s,
        engine: { ...s.engine, enabled: on, status: on ? 'starting' : s.engine.status, message: on ? 'Starting audio engine…' : s.engine.message },
      },
    });
  }
  return runAction('enabled', on ? 'Could not start engine' : 'Could not stop engine', () => api.setEnabled(on), applyState);
}

export function setBypass(on: boolean) {
  const s = snapshot.state;
  if (s) emit({ state: { ...s, engine: { ...s.engine, bypass: on } } });
  return runAction('bypass', 'Could not change bypass', () => api.setBypass(on), applyState);
}

export function applyPreset(id: PresetId) {
  dropPendingEdits();
  patchLocalParams({ preset: id });
  return runAction('preset', 'Could not apply preset', () => api.applyPreset(id), applyParams);
}

export function setListeningMode(mode: ListeningMode) {
  dropPendingEdits();
  const s = snapshot.state;
  if (s) emit({ state: { ...s, settings: { ...s.settings, listeningMode: mode } } });
  return runAction('preset', 'Could not change listening mode', () => api.setListeningMode(mode), applyState);
}

export function resetProfile() {
  dropPendingEdits();
  return runAction('reset', 'Could not reset profile', () => api.resetProfile(), applyParams);
}

export function selectOutputDevice(id: string) {
  return runAction('device', 'Could not switch output device', () => api.selectOutputDevice(id), applyState);
}

export function selectSourceDevice(id: string) {
  return runAction('source', 'Could not change source device', () => api.selectSourceDevice(id), applyState);
}

export function setSettings(partial: Partial<Settings>) {
  const s = snapshot.state;
  if (s) emit({ state: { ...s, settings: { ...s.settings, ...partial } } });
  return runAction('settings', 'Could not save settings', () => api.setSettings(partial), (settings: Settings) => {
    const cur = snapshot.state;
    if (cur) emit({ state: { ...cur, settings } });
  });
}

export function resetSettings() {
  dropPendingEdits();
  return runAction('settings', 'Could not reset settings', () => api.resetSettings(), applyState);
}

export function restoreDefaultRouting() {
  return runAction('routing', 'Could not restore routing', () => api.restoreDefaultRouting(), (s: AppState) => {
    applyState(s);
    pushNotice('info', 'Windows audio routing restored. CINEMAX processing is off.');
  });
}

export function openWindowsSoundSettings() {
  return runAction('routing', 'Could not open Windows sound settings', () => api.openWindowsSoundSettings(), () => {});
}

// ---------------------------------------------------------------------------
//  CINEMAX Vision (picture). Slider edits are optimistic and throttled (~60 ms):
//  every send rebuilds the hardware colour table, which takes a few milliseconds.
const VISION_THROTTLE_MS = 60;
let visionPending: { key: string; partial: Partial<PictureParams> } | null = null;
let visionBacklight: { key: string; percent: number } | null = null;
let visionTimer: number | null = null;
let visionInflight = 0;
let visionLastEdit = 0;

function visionEditing() {
  return visionPending !== null || visionBacklight !== null || visionTimer !== null || visionInflight > 0 || performance.now() - visionLastEdit < PROTECT_MS;
}

function patchDisplay(key: string, patch: (d: DisplayView) => DisplayView) {
  const s = snapshot.state;
  if (!s?.vision) return;
  emit({ state: { ...s, vision: { ...s.vision, displays: s.vision.displays.map((d) => (d.key === key ? patch(d) : d)) } } });
}

function scheduleVisionFlush() {
  if (visionTimer === null) visionTimer = window.setTimeout(flushVision, VISION_THROTTLE_MS);
}

function flushVision() {
  visionTimer = null;
  const p = visionPending;
  const b = visionBacklight;
  visionPending = null;
  visionBacklight = null;
  if (p) {
    visionInflight++;
    api
      .visionSetParams(p.key, p.partial)
      .then((s) => {
        visionInflight--;
        if (visionPending === null && visionTimer === null && visionInflight === 0) applyState(s);
      })
      .catch((e) => {
        visionInflight--;
        reportError('Could not apply picture setting', e);
        void resync();
      });
  }
  if (b) {
    visionInflight++;
    api
      .visionSetBacklight(b.key, b.percent)
      .then((ok) => {
        visionInflight--;
        if (!ok) pushNotice('warning', 'Windows did not accept the backlight change.');
      })
      .catch((e) => {
        visionInflight--;
        reportError('Could not change backlight', e);
      });
  }
}

function patchVisionSettings(partial: Partial<VisionSettings>) {
  const s = snapshot.state;
  if (s?.vision) emit({ state: { ...s, vision: { ...s.vision, settings: { ...s.vision.settings, ...partial } } } });
}

// ---- TV: sliders send at most every 150 ms; the screen updates at once ---------------
const tvTimers = new Map<string, number>();
const tvPending = new Map<string, Partial<TvSetup>>();

export const tv = {
  setSetup(deviceId: string, partial: Partial<TvSetup>) {
    const s = snapshot.state;
    if (s?.tv) {
      emit({
        state: {
          ...s,
          tv: { ...s.tv, devices: s.tv.devices.map((d) => (d.id === deviceId ? { ...d, setup: { ...d.setup, ...partial } } : d)) },
        },
      });
    }
    tvPending.set(deviceId, { ...(tvPending.get(deviceId) ?? {}), ...partial });
    if (tvTimers.has(deviceId)) return;
    tvTimers.set(
      deviceId,
      window.setTimeout(() => {
        tvTimers.delete(deviceId);
        const send = tvPending.get(deviceId) ?? {};
        tvPending.delete(deviceId);
        void runAction('tv', 'Could not save the TV setup', () => api.setTvSetup(deviceId, send), applyState);
      }, 150),
    );
  },
  setPicture(displayKey: string, partial: Partial<Pick<TvPicture, 'range' | 'contentAuto' | 'gameMode'>>) {
    const s = snapshot.state;
    if (s?.vision) {
      emit({
        state: {
          ...s,
          vision: { ...s.vision, displays: s.vision.displays.map((d) => (d.key === displayKey ? { ...d, tv: { ...d.tv, ...partial } } : d)) },
        },
      });
    }
    return runAction('tv', 'Could not change the TV picture', () => api.visionSetTv(displayKey, partial), applyState);
  },
};

export const vision = {
  /** Re-read displays (backlight, HDR, Night Light). Silent: used on a timer while visible. */
  refresh() {
    if (!isConnected || visionEditing()) return;
    api.visionRefresh().then(applyState).catch(() => {});
  },
  setEnabled(on: boolean) {
    patchVisionSettings({ enabled: on });
    return runAction('vision', 'Could not change picture processing', () => api.visionSetEnabled(on), applyState);
  },
  setSettings(partial: Partial<VisionSettings>) {
    patchVisionSettings(partial);
    return runAction('vision', 'Could not save picture settings', () => api.visionSetSettings(partial), applyState);
  },
  selectPreset(key: string, id: PicturePresetId) {
    patchDisplay(key, (d) => ({ ...d, mode: 'manual', activePreset: id }));
    return runAction('vision', 'Could not apply picture preset', () => api.visionSelectPreset(key, id), applyState);
  },
  setAuto(key: string) {
    patchDisplay(key, (d) => ({ ...d, mode: 'auto' }));
    return runAction('vision', 'Could not switch to Auto picture', () => api.visionSetAuto(key), applyState);
  },
  setParams(key: string, partial: Partial<PictureParams>) {
    if (!isConnected) return;
    visionLastEdit = performance.now();
    patchDisplay(key, (d) => ({ ...d, mode: 'manual', activePreset: 'custom', params: { ...d.params, ...partial, preset: 'custom' } }));
    visionPending = { key, partial: { ...(visionPending && visionPending.key === key ? visionPending.partial : {}), ...partial } };
    scheduleVisionFlush();
  },
  setBacklight(key: string, percent: number) {
    if (!isConnected) return;
    visionLastEdit = performance.now();
    patchDisplay(key, (d) => ({ ...d, backlight: percent }));
    visionBacklight = { key, percent };
    scheduleVisionFlush();
  },
  resetDisplay(key: string) {
    return runAction('vision', 'Could not reset the picture', () => api.visionResetDisplay(key), applyState);
  },
  saveProfiles() {
    return runAction('vision', 'Could not save picture profiles', () => api.visionSaveProfiles(), () => {});
  },
  setCompare(on: boolean) {
    const s = snapshot.state;
    if (s?.vision) emit({ state: { ...s, vision: { ...s.vision, comparing: on } } });
    if (isConnected) api.visionSetCompare(on).catch(() => {});
  },
  restoreOriginal() {
    return runAction('vision', 'Could not restore the display', () => api.visionRestoreOriginal(), applyState);
  },
  setHdr(key: string, on: boolean) {
    return runAction('vision', 'Could not change HDR', () => api.visionSetHdr(key, on), applyState);
  },
  addRule(exe: string, id: PicturePresetId) {
    return runAction('vision', 'Could not add the app rule', () => api.visionAddRule(exe, id), applyState);
  },
  removeRule(exe: string) {
    return runAction('vision', 'Could not remove the app rule', () => api.visionRemoveRule(exe), applyState);
  },
  openDisplaySettings(nightLight: boolean) {
    if (isConnected) api.openWindowsDisplaySettings(nightLight).catch(() => {});
  },
};

// ---------------------------------------------------------------------------
//  Visualiser activity (only stream meters when someone is looking)
let visualizerWanted = false;
let visualizerSent: boolean | null = null;

function syncVisualizer() {
  if (!isConnected || snapshot.connection !== 'ready') return;
  const active = visualizerWanted && document.visibilityState === 'visible';
  if (active === visualizerSent) return;
  visualizerSent = active;
  api.setVisualizerActive(active).catch(() => {
    visualizerSent = null;
  });
}

export function setVisualizerWanted(on: boolean) {
  visualizerWanted = on;
  syncVisualizer();
}

// ---------------------------------------------------------------------------
//  Lifecycle
let started = false;

function markReady() {
  if (snapshot.connection === 'ready') return;
  emit({ connection: 'ready', error: null });
  visualizerSent = null;
  syncVisualizer();
}

export async function resync() {
  if (!isConnected) return;
  try {
    const s = await api.getState();
    applyState(s);
    markReady();
  } catch (e) {
    if (!snapshot.state) emit({ connection: 'failed', error: e instanceof Error ? e.message : String(e) });
  }
}

export function startStore() {
  if (started || !isConnected) return;
  started = true;

  subscribeEvent<AppState>(EVENT_STATE, (s) => {
    if (!s || typeof s !== 'object' || !s.engine) return;
    applyState(s);
    markReady();
  });

  subscribeEvent<Meters>(EVENT_METERS, (m) => {
    if (!m || !Array.isArray(m.spectrum)) return;
    const now = performance.now();
    meterStore.latest = m;
    meterStore.receivedAt = now;
    meterStore.seq++;
    if (m.active && m.limiting) meterStore.lastLimitingAt = now;
    if (m.active && m.clip) meterStore.lastClipAt = now;
  });

  subscribeEvent<Notice>(EVENT_NOTICE, (n) => {
    if (n && typeof n.message === 'string') pushNotice(n.level ?? 'info', n.message);
  });

  document.addEventListener('visibilitychange', syncVisualizer);

  void (async () => {
    for (let attempt = 0; attempt < 4; attempt++) {
      try {
        const s = await api.getState();
        applyState(s);
        markReady();
        return;
      } catch (e) {
        if (attempt === 3) emit({ connection: 'failed', error: e instanceof Error ? e.message : String(e) });
        await new Promise((r) => setTimeout(r, 500 * (attempt + 1)));
      }
    }
  })();
}

export function retryConnection() {
  if (!isConnected) return;
  emit({ connection: 'connecting', error: null });
  void resync().then(() => {
    visualizerSent = null;
    syncVisualizer();
  });
}
