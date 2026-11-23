import { useEffect, useRef, useState } from 'react';
import { meterStore, metersAreLive } from '../bridge/store';
import { approach, prefersReducedMotion, useAnimatedCanvas } from '../lib/canvas';

const ACCENT = '#ff6a1a';
const BANDS = 64;
const DB_FLOOR = -90;
const DB_SPAN = 84; // -90 .. -6 dBFS fills the height

function bandToNorm(db: number) {
  return Math.min(1, Math.max(0, (db - DB_FLOOR) / DB_SPAN));
}

/** Center frequency of log-spaced band i (20 Hz .. 20 kHz). */
function bandFreq(i: number) {
  return 20 * Math.pow(1000, (i + 0.5) / BANDS);
}

function freqToX(f: number, x0: number, w: number) {
  return x0 + (Math.log(f / 20) / Math.log(1000)) * w;
}

function roundedTopRect(ctx: CanvasRenderingContext2D, x: number, y: number, w: number, h: number, r: number) {
  const rr = Math.min(r, w / 2, h);
  ctx.moveTo(x, y + h);
  ctx.lineTo(x, y + rr);
  ctx.quadraticCurveTo(x, y, x + rr, y);
  ctx.lineTo(x + w - rr, y);
  ctx.quadraticCurveTo(x + w, y, x + w, y + rr);
  ctx.lineTo(x + w, y + h);
  ctx.closePath();
}

// ---------------------------------------------------------------------------
//  Spectrum: 64 log bands with smoothed rise/fall and peak-hold caps.
export function SpectrumView({ showLabels = true, className = '' }: { showLabels?: boolean; className?: string }) {
  const bars = useRef(new Float32Array(BANDS));
  const peaks = useRef(new Float32Array(BANDS));
  const hold = useRef(new Float32Array(BANDS));

  const ref = useAnimatedCanvas((ctx, w, h, dt, now) => {
    const live = metersAreLive(now);
    const m = meterStore.latest;
    const reduce = prefersReducedMotion();
    const labelH = showLabels ? 18 : 0;
    const top = 8;
    const ch = h - labelH - top;
    const x0 = 2;
    const cw = w - 4;

    ctx.clearRect(0, 0, w, h);

    // Grid
    ctx.strokeStyle = 'rgba(255,255,255,0.045)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let i = 1; i <= 3; i++) {
      const y = Math.round(top + (ch * i) / 4) + 0.5;
      ctx.moveTo(x0, y);
      ctx.lineTo(x0 + cw, y);
    }
    ctx.stroke();

    let animating = false;
    const b = bars.current;
    const p = peaks.current;
    const hd = hold.current;
    for (let i = 0; i < BANDS; i++) {
      const target = live && m && m.spectrum.length >= BANDS ? bandToNorm(m.spectrum[i]) : 0;
      b[i] = reduce ? target : approach(b[i], target, dt, 0.025, 0.2);
      if (b[i] >= p[i]) {
        p[i] = b[i];
        hd[i] = 0.7;
      } else if (hd[i] > 0) {
        hd[i] -= dt;
      } else {
        p[i] = Math.max(b[i], p[i] - dt * 0.5);
      }
      if (b[i] > 0.002 || p[i] > 0.002) animating = true;
    }

    const gap = cw / BANDS > 9 ? 3 : 2;
    const bw = cw / BANDS - gap;
    const grad = ctx.createLinearGradient(0, top, 0, top + ch);
    grad.addColorStop(0, 'rgba(255,255,255,0.95)');
    grad.addColorStop(0.55, 'rgba(255,255,255,0.42)');
    grad.addColorStop(1, 'rgba(255,255,255,0.08)');

    ctx.fillStyle = grad;
    ctx.beginPath();
    for (let i = 0; i < BANDS; i++) {
      const bh = b[i] * ch;
      if (bh < 0.5) continue;
      const x = x0 + i * (bw + gap) + gap / 2;
      roundedTopRect(ctx, x, top + ch - bh, bw, bh, 2);
    }
    ctx.fill();

    // Idle baseline dots (calm, static — not a fake signal)
    ctx.fillStyle = 'rgba(255,255,255,0.16)';
    for (let i = 0; i < BANDS; i++) {
      if (b[i] * ch > 2) continue;
      const x = x0 + i * (bw + gap) + gap / 2;
      ctx.fillRect(x, top + ch - 1.5, bw, 1.5);
    }

    // Peak caps
    ctx.fillStyle = ACCENT;
    for (let i = 0; i < BANDS; i++) {
      if (p[i] < 0.02 || p[i] - b[i] < 0.012) continue;
      const x = x0 + i * (bw + gap) + gap / 2;
      ctx.globalAlpha = 0.85;
      ctx.fillRect(x, top + ch - p[i] * ch - 2, bw, 2);
    }
    ctx.globalAlpha = 1;

    if (showLabels) {
      ctx.fillStyle = 'rgba(255,255,255,0.32)';
      ctx.font = '500 10px "Segoe UI Variable Text", "Segoe UI", system-ui';
      ctx.textAlign = 'center';
      ctx.textBaseline = 'bottom';
      for (const f of [50, 100, 200, 500, 1000, 2000, 5000, 10000]) {
        const x = freqToX(f, x0, cw);
        ctx.fillText(f >= 1000 ? `${f / 1000}k` : String(f), x, h - 2);
      }
    }
    return animating;
  }, [showLabels]);

  return <canvas ref={ref} className={`viz-canvas ${className}`} aria-hidden="true" />;
}

// Exported for EQ background drawing.
export { bandFreq, bandToNorm };

// ---------------------------------------------------------------------------
//  Waveform: the engine's latest 256-point output snapshot, cross-faded.
export function WaveformView({ className = '' }: { className?: string }) {
  const disp = useRef(new Float32Array(256));

  const ref = useAnimatedCanvas((ctx, w, h, dt, now) => {
    const live = metersAreLive(now);
    const m = meterStore.latest;
    const src = live && m ? m.waveform : null;
    const d = disp.current;
    const n = d.length;
    let animating = false;
    for (let i = 0; i < n; i++) {
      const t = src && src.length ? src[Math.floor((i * src.length) / n)] ?? 0 : 0;
      d[i] = approach(d[i], t, dt, 0.03, 0.06);
      if (Math.abs(d[i]) > 0.001) animating = true;
    }

    ctx.clearRect(0, 0, w, h);
    const mid = h / 2;
    ctx.strokeStyle = 'rgba(255,255,255,0.08)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, Math.round(mid) + 0.5);
    ctx.lineTo(w, Math.round(mid) + 0.5);
    ctx.stroke();

    const amp = h * 0.46;
    const edge = ctx.createLinearGradient(0, 0, w, 0);
    edge.addColorStop(0, 'rgba(255,255,255,0)');
    edge.addColorStop(0.12, 'rgba(255,255,255,0.85)');
    edge.addColorStop(0.88, 'rgba(255,255,255,0.85)');
    edge.addColorStop(1, 'rgba(255,255,255,0)');

    ctx.beginPath();
    for (let i = 0; i < n; i++) {
      const x = (i / (n - 1)) * w;
      const y = mid - Math.max(-1, Math.min(1, d[i])) * amp;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    }
    ctx.strokeStyle = edge;
    ctx.lineWidth = 1.25;
    ctx.stroke();

    ctx.lineTo(w, mid);
    ctx.lineTo(0, mid);
    ctx.closePath();
    ctx.fillStyle = 'rgba(255,255,255,0.05)';
    ctx.fill();
    return animating;
  });

  return <canvas ref={ref} className={`viz-canvas ${className}`} aria-hidden="true" />;
}

// ---------------------------------------------------------------------------
//  Stereo output level meter (RMS body + peak sliver + peak hold).
const LM_MIN = -60;

function dbToNorm(db: number) {
  if (!Number.isFinite(db)) return 0;
  return Math.min(1, Math.max(0, (db - LM_MIN) / -LM_MIN));
}

export function LevelMeter({ className = '' }: { className?: string }) {
  const st = useRef({ rms: [0, 0], peak: [0, 0], hold: [0, 0], holdT: [0, 0] });

  const ref = useAnimatedCanvas((ctx, w, h, dt, now) => {
    const live = metersAreLive(now);
    const m = meterStore.latest;
    const s = st.current;
    const scaleW = 26;
    const barW = Math.min(12, (w - scaleW - 8) / 2);
    const top = 14;
    const bottom = h - 4;
    const bh = bottom - top;
    let animating = false;

    for (let c = 0; c < 2; c++) {
      const tr = live && m ? dbToNorm(m.rmsDb[c]) : 0;
      const tp = live && m ? dbToNorm(m.peakDb[c]) : 0;
      s.rms[c] = approach(s.rms[c], tr, dt, 0.04, 0.3);
      s.peak[c] = approach(s.peak[c], tp, dt, 0.01, 0.25);
      if (s.peak[c] >= s.hold[c]) {
        s.hold[c] = s.peak[c];
        s.holdT[c] = 1.2;
      } else if (s.holdT[c] > 0) s.holdT[c] -= dt;
      else s.hold[c] = Math.max(s.peak[c], s.hold[c] - dt * 0.35);
      if (s.rms[c] > 0.002 || s.hold[c] > 0.002) animating = true;
    }

    ctx.clearRect(0, 0, w, h);

    // Scale
    ctx.font = '500 9.5px "Segoe UI Variable Text", "Segoe UI", system-ui';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (const db of [0, -6, -12, -24, -36, -48]) {
      const y = top + (1 - dbToNorm(db)) * bh;
      ctx.fillStyle = db === 0 ? 'rgba(255,255,255,0.55)' : 'rgba(255,255,255,0.3)';
      ctx.fillText(db === 0 ? '0' : `${db}`.replace('-', '−'), scaleW - 4, y);
      ctx.fillStyle = 'rgba(255,255,255,0.06)';
      ctx.fillRect(scaleW, Math.round(y), w - scaleW, 1);
    }

    const clipRecent = now - meterStore.lastClipAt < 1500;
    for (let c = 0; c < 2; c++) {
      const x = scaleW + 6 + c * (barW + 6);
      // channel track
      ctx.fillStyle = 'rgba(255,255,255,0.05)';
      ctx.fillRect(x, top, barW, bh);
      // peak sliver (translucent)
      const ph = s.peak[c] * bh;
      ctx.fillStyle = 'rgba(255,255,255,0.18)';
      ctx.fillRect(x, top + bh - ph, barW, ph);
      // RMS body
      const rh = s.rms[c] * bh;
      const g = ctx.createLinearGradient(0, top, 0, bottom);
      g.addColorStop(0, '#ffffff');
      g.addColorStop(0.1, 'rgba(255,255,255,0.92)');
      g.addColorStop(1, 'rgba(255,255,255,0.35)');
      ctx.fillStyle = g;
      ctx.fillRect(x, top + bh - rh, barW, rh);
      // hold marker (accent above -6 dBFS)
      if (s.hold[c] > 0.01) {
        const hy = top + (1 - s.hold[c]) * bh;
        ctx.fillStyle = s.hold[c] > dbToNorm(-6) ? ACCENT : 'rgba(255,255,255,0.8)';
        ctx.fillRect(x, Math.round(hy) - 1, barW, 2);
      }
      // clip cap
      ctx.fillStyle = clipRecent ? '#ff4d4f' : 'rgba(255,255,255,0.08)';
      ctx.fillRect(x, 2, barW, 7);
    }
    return animating || clipRecent;
  });

  return <canvas ref={ref} className={`viz-canvas ${className}`} aria-hidden="true" />;
}

// ---------------------------------------------------------------------------
//  Bass activity: sets --bass (0..1) on a decorative element via rAF.
export function BassPulse({ className = '' }: { className?: string }) {
  const el = useRef<HTMLDivElement>(null);
  useEffect(() => {
    let raf = 0;
    let v = 0;
    let last = performance.now();
    let lastWritten = -1;
    const frame = (now: number) => {
      raf = requestAnimationFrame(frame);
      const dt = Math.min(0.1, (now - last) / 1000);
      last = now;
      const live = metersAreLive(now);
      const t = live && meterStore.latest ? Math.min(1, Math.max(0, meterStore.latest.bass)) : 0;
      v = prefersReducedMotion() ? t : approach(v, t, dt, 0.03, 0.18);
      const q = Math.round(v * 200) / 200;
      if (q !== lastWritten && el.current) {
        el.current.style.setProperty('--bass', String(q));
        lastWritten = q;
      }
    };
    raf = requestAnimationFrame(frame);
    return () => cancelAnimationFrame(raf);
  }, []);
  return <div ref={el} className={`bass-pulse ${className}`} aria-hidden="true" />;
}

// ---------------------------------------------------------------------------
//  Peak protection / clip indicators (latched so brief events are visible).
export function ProtectionIndicators() {
  const [flags, setFlags] = useState({ limiting: false, clip: false });
  useEffect(() => {
    const id = window.setInterval(() => {
      const now = performance.now();
      const limiting = now - meterStore.lastLimitingAt < 350;
      const clip = now - meterStore.lastClipAt < 2000;
      setFlags((f) => (f.limiting === limiting && f.clip === clip ? f : { limiting, clip }));
    }, 66);
    return () => window.clearInterval(id);
  }, []);
  return (
    <div className="indicators">
      <span className={`indicator${flags.limiting ? ' is-lit' : ''}`} title="The output limiter is catching peaks so nothing distorts">
        <span className="indicator__led" />
        Peak protection
      </span>
      <span className={`indicator indicator--clip${flags.clip ? ' is-lit' : ''}`} title="Lights if any output sample reached full scale">
        <span className="indicator__led" />
        Clip
      </span>
    </div>
  );
}
