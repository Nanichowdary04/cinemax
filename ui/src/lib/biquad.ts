// RBJ "Audio EQ Cookbook" biquads — used ONLY to draw the EQ response curve.
// The actual filtering happens in the C++ engine.

import type { DspParams } from '../bridge/types';

const FS = 48000;

interface Coeffs {
  b0: number;
  b1: number;
  b2: number;
  a0: number;
  a1: number;
  a2: number;
}

function peaking(f0: number, gainDb: number, q: number): Coeffs {
  const A = Math.pow(10, gainDb / 40);
  const w0 = (2 * Math.PI * Math.min(f0, FS * 0.49)) / FS;
  const alpha = Math.sin(w0) / (2 * Math.max(q, 0.05));
  const c = Math.cos(w0);
  return { b0: 1 + alpha * A, b1: -2 * c, b2: 1 - alpha * A, a0: 1 + alpha / A, a1: -2 * c, a2: 1 - alpha / A };
}

function shelf(f0: number, gainDb: number, high: boolean): Coeffs {
  const A = Math.pow(10, gainDb / 40);
  const w0 = (2 * Math.PI * Math.min(f0, FS * 0.49)) / FS;
  const c = Math.cos(w0);
  const alpha = (Math.sin(w0) / 2) * Math.SQRT2; // shelf slope S = 1
  const k = 2 * Math.sqrt(A) * alpha;
  if (!high) {
    return {
      b0: A * (A + 1 - (A - 1) * c + k),
      b1: 2 * A * (A - 1 - (A + 1) * c),
      b2: A * (A + 1 - (A - 1) * c - k),
      a0: A + 1 + (A - 1) * c + k,
      a1: -2 * (A - 1 + (A + 1) * c),
      a2: A + 1 + (A - 1) * c - k,
    };
  }
  return {
    b0: A * (A + 1 + (A - 1) * c + k),
    b1: -2 * A * (A - 1 + (A + 1) * c),
    b2: A * (A + 1 + (A - 1) * c - k),
    a0: A + 1 - (A - 1) * c + k,
    a1: 2 * (A - 1 - (A + 1) * c),
    a2: A + 1 - (A - 1) * c - k,
  };
}

function magDb(co: Coeffs, f: number): number {
  const w = (2 * Math.PI * f) / FS;
  const c1 = Math.cos(w);
  const s1 = Math.sin(w);
  const c2 = Math.cos(2 * w);
  const s2 = Math.sin(2 * w);
  const nr = co.b0 + co.b1 * c1 + co.b2 * c2;
  const ni = -(co.b1 * s1 + co.b2 * s2);
  const dr = co.a0 + co.a1 * c1 + co.a2 * c2;
  const di = -(co.a1 * s1 + co.a2 * s2);
  const num = nr * nr + ni * ni;
  const den = dr * dr + di * di;
  return 10 * Math.log10(Math.max(num, 1e-20) / Math.max(den, 1e-20));
}

/** Log-spaced frequencies between lo and hi. */
export function logFreqs(n: number, lo = 20, hi = 20000): Float64Array {
  const out = new Float64Array(n);
  const r = Math.log(hi / lo);
  for (let i = 0; i < n; i++) out[i] = lo * Math.exp((r * i) / (n - 1));
  return out;
}

export type EqDisplayParams = Pick<DspParams, 'eqEnabled' | 'eqBands' | 'bassDb' | 'midDb' | 'trebleDb' | 'preampDb'>;

/** Combined magnitude response (dB) of preamp + tone controls + 10 bands. */
export function eqResponse(p: EqDisplayParams, freqs: Float64Array, out?: Float64Array): Float64Array {
  const res = out ?? new Float64Array(freqs.length);
  const filters: Coeffs[] = [];
  if (p.eqEnabled) {
    for (const b of p.eqBands) if (Math.abs(b.gainDb) > 0.001) filters.push(peaking(b.freq, b.gainDb, b.q));
    if (Math.abs(p.bassDb) > 0.001) filters.push(shelf(100, p.bassDb, false));
    if (Math.abs(p.midDb) > 0.001) filters.push(peaking(1000, p.midDb, 0.9)); // matches engine Equalizer.cpp
    if (Math.abs(p.trebleDb) > 0.001) filters.push(shelf(8000, p.trebleDb, true));
  }
  for (let i = 0; i < freqs.length; i++) {
    let db = p.preampDb;
    for (const f of filters) db += magDb(f, freqs[i]);
    res[i] = db;
  }
  return res;
}

/** Response of a single band (for highlighting the selected band). */
export function bandResponse(freq: number, gainDb: number, q: number, freqs: Float64Array, out?: Float64Array) {
  const res = out ?? new Float64Array(freqs.length);
  const co = peaking(freq, gainDb, q);
  for (let i = 0; i < freqs.length; i++) res[i] = magDb(co, freqs[i]);
  return res;
}
