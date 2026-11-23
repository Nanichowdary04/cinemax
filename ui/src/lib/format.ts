const MINUS = '−';

function signed(v: number, digits: number): string {
  const r = Number(v.toFixed(digits));
  if (r === 0) return (0).toFixed(digits);
  return (r > 0 ? '+' : MINUS) + Math.abs(r).toFixed(digits);
}

export function fmtDb(v: number, digits = 1, unit = 'dB'): string {
  if (!Number.isFinite(v)) return '—';
  return `${signed(v, digits)} ${unit}`;
}

/** dBFS-style readout without forced "+" (levels are almost always negative). */
export function fmtLevel(v: number, floor = -90): string {
  if (!Number.isFinite(v) || v <= floor) return '−∞';
  return (v < 0 ? MINUS : '') + Math.abs(v).toFixed(1);
}

export function fmtHz(f: number): string {
  if (!Number.isFinite(f)) return '—';
  if (f >= 1000) {
    const k = Math.round(f / 100) / 10;
    return `${k.toFixed(Number.isInteger(k) ? 0 : 1)} kHz`;
  }
  return `${Math.round(f)} Hz`;
}

export function fmtHzShort(f: number): string {
  if (f >= 1000) {
    const k = f / 1000;
    return `${Number.isInteger(k) ? k : k.toFixed(1)}k`;
  }
  return `${Math.round(f)}`;
}

export function fmtPct(v: number): string {
  return `${Math.round(v * 100)}%`;
}

export function fmtRate(sr: number): string {
  if (!sr) return '—';
  return `${(sr / 1000).toFixed(sr % 1000 === 0 ? 0 : 1)} kHz`;
}

export function fmtMs(ms: number, digits = 0): string {
  if (!Number.isFinite(ms) || ms <= 0) return '—';
  return `${ms.toFixed(digits)} ms`;
}

export function fmtChannels(n: number): string {
  if (!n) return '—';
  if (n === 1) return 'Mono';
  if (n === 2) return 'Stereo';
  if (n === 6) return '5.1';
  if (n === 8) return '7.1';
  return `${n} ch`;
}

export function clamp(v: number, lo: number, hi: number): number {
  return Math.min(hi, Math.max(lo, v));
}

export function roundTo(v: number, step: number): number {
  const r = Math.round(v / step) * step;
  // Avoid 0.30000000000000004 style noise.
  const digits = Math.max(0, Math.ceil(-Math.log10(step)) + 1);
  return Number(r.toFixed(digits));
}
