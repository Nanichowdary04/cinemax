// ============================================================================
//  Low-level bridge to the C++ host (JUCE WebBrowserComponent native
//  integration). Own minimal implementation of JUCE's frontend protocol:
//
//    send event     window.__JUCE__.backend.emitEvent(id, payload)
//    receive event  window.__JUCE__.backend.addEventListener(id, fn) -> handle
//    native call    emit "__juce__invoke" {name, params, resultId}
//                   <- event "__juce__complete" {promiseId, result}
//
//  When the page is not hosted by CINEMAX (window.__JUCE__ missing) nothing is
//  faked: isConnected is false and every call rejects.
// ============================================================================

import type { NativeApi } from './types';

type ListenerHandle = unknown;

interface JuceBackend {
  emitEvent(eventId: string, payload: unknown): void;
  addEventListener(eventId: string, fn: (payload: any) => void): ListenerHandle;
  removeEventListener(handle: ListenerHandle): void;
}

interface JuceGlobal {
  backend: JuceBackend;
  initialisationData?: Record<string, unknown[]>;
}

declare global {
  interface Window {
    __JUCE__?: JuceGlobal;
  }
}

const juce: JuceGlobal | undefined =
  typeof window !== 'undefined' && window.__JUCE__ && window.__JUCE__.backend ? window.__JUCE__ : undefined;

/** True only when running inside the CINEMAX host application. */
export const isConnected: boolean = juce !== undefined;

const CALL_TIMEOUT_MS = 8000;

interface Pending {
  resolve: (v: unknown) => void;
  reject: (e: Error) => void;
  timer: number;
  name: string;
}

const pending = new Map<number, Pending>();
let nextResultId = 1;

if (juce) {
  juce.backend.addEventListener('__juce__complete', (payload: { promiseId: number; result: unknown }) => {
    if (!payload || typeof payload.promiseId !== 'number') return;
    const entry = pending.get(payload.promiseId);
    if (!entry) return;
    pending.delete(payload.promiseId);
    window.clearTimeout(entry.timer);
    entry.resolve(payload.result);
  });
}

export class BridgeError extends Error {}

/** Invoke a native function registered with WebBrowserComponent::Options::withNativeFunction. */
export function invoke<T>(name: string, ...args: unknown[]): Promise<T> {
  if (!juce) return Promise.reject(new BridgeError('CINEMAX engine not connected'));
  const resultId = nextResultId++;
  return new Promise<T>((resolve, reject) => {
    const timer = window.setTimeout(() => {
      pending.delete(resultId);
      reject(new BridgeError(`${name}() timed out after ${CALL_TIMEOUT_MS / 1000}s`));
    }, CALL_TIMEOUT_MS);
    pending.set(resultId, { resolve: resolve as (v: unknown) => void, reject, timer, name });
    try {
      juce.backend.emitEvent('__juce__invoke', { name, params: args, resultId });
    } catch (e) {
      pending.delete(resultId);
      window.clearTimeout(timer);
      reject(new BridgeError(`${name}() could not be sent: ${String(e)}`));
    }
  });
}

/** Subscribe to an event emitted by the backend. Returns an unsubscribe function. */
export function subscribe<T>(eventId: string, fn: (payload: T) => void): () => void {
  if (!juce) return () => {};
  const handle = juce.backend.addEventListener(eventId, fn);
  return () => {
    try {
      juce.backend.removeEventListener(handle);
    } catch {
      /* host already gone */
    }
  };
}

/** Typed wrapper around every native function in the contract. */
export const api: NativeApi = {
  getState: () => invoke('getState'),
  setEnabled: (on) => invoke('setEnabled', on),
  setBypass: (on) => invoke('setBypass', on),
  setParams: (partial) => invoke('setParams', partial),
  setEqBand: (index, band) => invoke('setEqBand', index, band),
  applyPreset: (id) => invoke('applyPreset', id),
  resetProfile: () => invoke('resetProfile'),
  visionRefresh: () => invoke('visionRefresh'),
  visionSetEnabled: (on) => invoke('visionSetEnabled', on),
  visionSetSettings: (partial) => invoke('visionSetSettings', partial),
  visionSelectPreset: (key, id) => invoke('visionSelectPreset', key, id),
  visionSetAuto: (key) => invoke('visionSetAuto', key),
  visionSetParams: (key, partial) => invoke('visionSetParams', key, partial),
  visionSetBacklight: (key, percent) => invoke('visionSetBacklight', key, percent),
  visionResetDisplay: (key) => invoke('visionResetDisplay', key),
  visionSaveProfiles: () => invoke('visionSaveProfiles'),
  visionSetCompare: (on) => invoke('visionSetCompare', on),
  visionRestoreOriginal: () => invoke('visionRestoreOriginal'),
  visionSetHdr: (key, on) => invoke('visionSetHdr', key, on),
  visionAddRule: (exe, id) => invoke('visionAddRule', exe, id),
  visionRemoveRule: (exe) => invoke('visionRemoveRule', exe),
  openWindowsDisplaySettings: (nightLight) => invoke('openWindowsDisplaySettings', nightLight),
  selectOutputDevice: (id) => invoke('selectOutputDevice', id),
  selectSourceDevice: (id) => invoke('selectSourceDevice', id),
  setSettings: (partial) => invoke('setSettings', partial),
  resetSettings: () => invoke('resetSettings'),
  restoreDefaultRouting: () => invoke('restoreDefaultRouting'),
  openWindowsSoundSettings: () => invoke('openWindowsSoundSettings'),
  openExternalUrl: (url) => invoke('openExternalUrl', url),
  setListeningMode: (mode) => invoke('setListeningMode', mode),
  setVisualizerActive: (on) => invoke('setVisualizerActive', on),
  startEarTest: (key) => invoke('startEarTest', key),
  stopEarTest: () => invoke('stopEarTest'),
  getHeadTracking: () => invoke('getHeadTracking'),
  setTvSetup: (id, partial) => invoke('setTvSetup', id, partial),
  visionSetTv: (key, partial) => invoke('visionSetTv', key, partial),
};
