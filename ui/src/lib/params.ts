import type { DspParams, EqBand } from '../bridge/types';
import { fmtDb, fmtHz } from './format';

type NumKey = { [K in keyof DspParams]: DspParams[K] extends number ? K : never }[keyof DspParams];

export interface NumSpec {
  label: string;
  min: number;
  max: number;
  step: number;
  /** value restored on double-click */
  reset: number;
  bipolar?: boolean;
  format: (v: number) => string;
}

const pct = (v: number) => `${Math.round(v * 100)}%`;

// Ranges mirror the contract in bridge/types.ts. The engine clamps as well.
export const SPECS: Record<NumKey, NumSpec> = {
  preampDb: { label: 'Preamp', min: -12, max: 6, step: 0.5, reset: 0, bipolar: true, format: (v) => fmtDb(v) },
  bassDb: { label: 'Bass', min: -12, max: 12, step: 0.5, reset: 0, bipolar: true, format: (v) => fmtDb(v) },
  midDb: { label: 'Mid', min: -12, max: 12, step: 0.5, reset: 0, bipolar: true, format: (v) => fmtDb(v) },
  trebleDb: { label: 'Treble', min: -12, max: 12, step: 0.5, reset: 0, bipolar: true, format: (v) => fmtDb(v) },
  bassBoostDb: { label: 'Bass boost', min: 0, max: 12, step: 0.5, reset: 0, format: (v) => fmtDb(v) },
  bassFreq: { label: 'Boost frequency', min: 40, max: 160, step: 1, reset: 90, format: (v) => fmtHz(v) },
  subBassDb: { label: 'Sub-bass', min: -6, max: 9, step: 0.5, reset: 0, bipolar: true, format: (v) => fmtDb(v) },
  bassHarmonics: { label: 'Harmonic bass', min: 0, max: 1, step: 0.01, reset: 0, format: pct },
  dialogueAmount: { label: 'Voice clarity', min: 0, max: 1, step: 0.01, reset: 0, format: pct },
  backgroundDb: { label: 'Background level', min: -12, max: 0, step: 0.5, reset: 0, format: (v) => fmtDb(v) },
  ieqAmount: { label: 'Strength', min: 0, max: 1, step: 0.01, reset: 0.6, format: pct },
  surroundIntensity: { label: 'Surround intensity', min: 0, max: 1, step: 0.01, reset: 0, format: pct },
  stereoWidth: {
    label: 'Stereo width',
    min: 0,
    max: 1,
    step: 0.01,
    reset: 0.5,
    format: (v) => (Math.abs(v - 0.5) < 0.005 ? 'Original' : `${Math.round(v * 200)}%`),
  },
  roomAmount: { label: 'Room ambience', min: 0, max: 1, step: 0.01, reset: 0, format: pct },
  heightAmount: { label: '3D height', min: 0, max: 1, step: 0.01, reset: 0, format: pct },
  roomSize: {
    label: 'Venue',
    min: 0,
    max: 1,
    step: 0.01,
    reset: 0.25,
    // Mirrors RoomAmbience::rt60ForSize in the engine.
    format: (v) => `${v < 0.34 ? 'Theatre' : v < 0.67 ? 'Opera house' : 'Concert hall'} · ${(0.45 + 1.65 * v).toFixed(1)} s`,
  },
  compressorAmount: { label: 'Compression', min: 0, max: 1, step: 0.01, reset: 0, format: pct },
  loudnessTargetLufs: { label: 'Target loudness', min: -24, max: -10, step: 0.5, reset: -18, format: (v) => `${v.toFixed(1).replace('-', '−')} LUFS` },
  limiterCeilingDb: { label: 'Limiter ceiling', min: -6, max: -0.1, step: 0.1, reset: -1, format: (v) => fmtDb(v, 1, 'dBFS') },
};

export const EQ_LIMITS = {
  freq: { min: 20, max: 20000 },
  gain: { min: -12, max: 12, step: 0.1 },
  q: { min: 0.3, max: 8, step: 0.01 },
};

/** ISO octave centres used by "Reset bands". */
export const STANDARD_BANDS: EqBand[] = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000].map((freq) => ({
  freq,
  gainDb: 0,
  q: 1.41,
}));
