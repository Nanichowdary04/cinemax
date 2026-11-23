import { useEffect, useState, useSyncExternalStore } from 'react';
import { isConnected } from './bridge';
import * as store from './store';
import type { Meters } from './types';

export const actions = {
  setEnabled: store.setEnabled,
  setBypass: store.setBypass,
  setParams: store.setParams,
  setEqBand: store.setEqBand,
  applyPreset: store.applyPreset,
  setListeningMode: store.setListeningMode,
  resetProfile: store.resetProfile,
  selectOutputDevice: store.selectOutputDevice,
  selectSourceDevice: store.selectSourceDevice,
  setSettings: store.setSettings,
  resetSettings: store.resetSettings,
  restoreDefaultRouting: store.restoreDefaultRouting,
  openWindowsSoundSettings: store.openWindowsSoundSettings,
  dismissNotice: store.dismissNotice,
  retryConnection: store.retryConnection,
  vision: store.vision,
  tv: store.tv,
};

/** Main hook: engine state, connection, notices and actions. */
export function useCinemax() {
  const snap = useSyncExternalStore(store.subscribeStore, store.getSnapshot);
  return {
    connected: isConnected,
    connection: snap.connection,
    error: snap.error,
    state: snap.state,
    notices: snap.notices,
    busy: snap.busy,
    actions,
  };
}

/**
 * Low-rate snapshot of the latest meter frame for text readouts.
 * Returns null when no recent frame exists (never invents values). Callers
 * should still check `.active` before showing signal-derived numbers.
 */
export function useMeterSnapshot(hz = 8): Meters | null {
  const [m, setM] = useState<Meters | null>(null);
  useEffect(() => {
    let lastSeq = -1;
    let wasFresh = false;
    const id = window.setInterval(() => {
      const fresh = store.metersAreFresh();
      if (store.meterStore.seq !== lastSeq || fresh !== wasFresh) {
        lastSeq = store.meterStore.seq;
        wasFresh = fresh;
        setM(fresh ? store.meterStore.latest : null);
      }
    }, 1000 / hz);
    return () => window.clearInterval(id);
  }, [hz]);
  return m;
}

/** Declares that the current view shows live visualisation. */
export function useVisualizer(active: boolean) {
  useEffect(() => {
    store.setVisualizerWanted(active);
    return () => store.setVisualizerWanted(false);
  }, [active]);
}
