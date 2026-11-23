import { useEffect, useMemo, useRef } from 'react';
import type { EqBand } from '../bridge/types';
import { getSnapshot, meterStore, metersAreLive } from '../bridge/store';
import { bandResponse, eqResponse, logFreqs, type EqDisplayParams } from '../lib/biquad';
import { approach, useAnimatedCanvas } from '../lib/canvas';
import { EQ_LIMITS } from '../lib/params';
import { bandToNorm } from './Visuals';
import { clamp, fmtDb, fmtHz } from '../lib/format';

const RANGE = 15; // ±dB shown
const PAD = { l: 40, r: 14, t: 16, b: 26 };
const ACCENT = '#ff6a1a';
const N_POINTS = 260;

function roundFreq(f: number) {
  if (f < 100) return Math.round(f);
  if (f < 1000) return Math.round(f / 5) * 5;
  if (f < 10000) return Math.round(f / 50) * 50;
  return Math.round(f / 100) * 100;
}

interface Props {
  params: EqDisplayParams;
  selected: number;
  onSelect: (i: number) => void;
  onBandChange: (i: number, b: Partial<EqBand>) => void;
}

export function EqCurve({ params, selected, onSelect, onBandChange }: Props) {
  const freqs = useMemo(() => logFreqs(N_POINTS), []);
  const resp = useMemo(() => eqResponse(params, freqs), [params, freqs]);
  const sel = params.eqBands[selected];
  const selResp = useMemo(
    () => (sel && params.eqEnabled ? bandResponse(sel.freq, sel.gainDb, sel.q, freqs) : null),
    [sel, params.eqEnabled, freqs],
  );
  const spec = useRef(new Float32Array(64));
  const geom = useRef({ w: 0, h: 0 });
  const dragging = useRef<number | null>(null);

  const ref = useAnimatedCanvas(
    (ctx, w, h, dt, now) => {
      geom.current = { w, h };
      const pw = w - PAD.l - PAD.r;
      const ph = h - PAD.t - PAD.b;
      const fx = (f: number) => PAD.l + (Math.log(f / 20) / Math.log(1000)) * pw;
      const dy = (db: number) => PAD.t + (1 - (clamp(db, -RANGE, RANGE) + RANGE) / (2 * RANGE)) * ph;
      const enabled = params.eqEnabled;

      ctx.clearRect(0, 0, w, h);

      // Grid
      ctx.font = '500 10px "Segoe UI Variable Text", "Segoe UI", system-ui';
      ctx.lineWidth = 1;
      for (const f of [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000]) {
        const x = Math.round(fx(f)) + 0.5;
        ctx.strokeStyle = 'rgba(255,255,255,0.05)';
        ctx.beginPath();
        ctx.moveTo(x, PAD.t);
        ctx.lineTo(x, PAD.t + ph);
        ctx.stroke();
        ctx.fillStyle = 'rgba(255,255,255,0.32)';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'top';
        ctx.fillText(f >= 1000 ? `${f / 1000}k` : String(f), x, PAD.t + ph + 8);
      }
      for (const db of [12, 6, 0, -6, -12]) {
        const y = Math.round(dy(db)) + 0.5;
        ctx.strokeStyle = db === 0 ? 'rgba(255,255,255,0.14)' : 'rgba(255,255,255,0.05)';
        ctx.beginPath();
        ctx.moveTo(PAD.l, y);
        ctx.lineTo(PAD.l + pw, y);
        ctx.stroke();
        ctx.fillStyle = 'rgba(255,255,255,0.32)';
        ctx.textAlign = 'right';
        ctx.textBaseline = 'middle';
        ctx.fillText(db > 0 ? `+${db}` : db === 0 ? '0' : `−${-db}`, PAD.l - 10, y);
      }

      // Live output spectrum (background reference only)
      const live = metersAreLive(now);
      const m = meterStore.latest;
      const s = spec.current;
      let animating = false;
      for (let i = 0; i < 64; i++) {
        const t = live && m && m.spectrum.length >= 64 ? bandToNorm(m.spectrum[i]) : 0;
        s[i] = approach(s[i], t, dt, 0.04, 0.25);
        if (s[i] > 0.002) animating = true;
      }
      if (animating) {
        ctx.beginPath();
        ctx.moveTo(PAD.l, PAD.t + ph);
        for (let i = 0; i < 64; i++) {
          const x = PAD.l + ((i + 0.5) / 64) * pw;
          ctx.lineTo(x, PAD.t + ph - s[i] * ph * 0.9);
        }
        ctx.lineTo(PAD.l + pw, PAD.t + ph);
        ctx.closePath();
        const g = ctx.createLinearGradient(0, PAD.t, 0, PAD.t + ph);
        g.addColorStop(0, 'rgba(255,255,255,0.10)');
        g.addColorStop(1, 'rgba(255,255,255,0.01)');
        ctx.fillStyle = g;
        ctx.fill();
      }

      // Selected band contribution
      if (selResp && enabled) {
        ctx.beginPath();
        for (let i = 0; i < freqs.length; i++) {
          const x = fx(freqs[i]);
          const y = dy(selResp[i]);
          if (i === 0) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
        }
        ctx.strokeStyle = 'rgba(255,106,26,0.55)';
        ctx.lineWidth = 1.2;
        ctx.stroke();
        ctx.lineTo(PAD.l + pw, dy(0));
        ctx.lineTo(PAD.l, dy(0));
        ctx.closePath();
        ctx.fillStyle = 'rgba(255,106,26,0.07)';
        ctx.fill();
      }

      // Combined curve
      ctx.beginPath();
      for (let i = 0; i < freqs.length; i++) {
        const x = fx(freqs[i]);
        const y = dy(resp[i]);
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.strokeStyle = enabled ? 'rgba(255,255,255,0.95)' : 'rgba(255,255,255,0.3)';
      ctx.lineWidth = 2;
      ctx.shadowColor = enabled ? 'rgba(255,255,255,0.35)' : 'transparent';
      ctx.shadowBlur = 10;
      ctx.stroke();
      ctx.shadowBlur = 0;
      ctx.lineTo(PAD.l + pw, dy(0));
      ctx.lineTo(PAD.l, dy(0));
      ctx.closePath();
      ctx.fillStyle = enabled ? 'rgba(255,255,255,0.05)' : 'rgba(255,255,255,0.02)';
      ctx.fill();

      // Handles
      params.eqBands.forEach((b, i) => {
        const x = fx(b.freq);
        const y = dy(b.gainDb);
        const isSel = i === selected;
        ctx.beginPath();
        ctx.arc(x, y, isSel ? 9 : 7, 0, Math.PI * 2);
        ctx.fillStyle = isSel ? '#1a1a1d' : '#121214';
        ctx.fill();
        ctx.lineWidth = isSel ? 2 : 1.4;
        ctx.strokeStyle = !enabled ? 'rgba(255,255,255,0.25)' : isSel ? ACCENT : 'rgba(255,255,255,0.85)';
        ctx.stroke();
        ctx.fillStyle = !enabled ? 'rgba(255,255,255,0.35)' : isSel ? '#fff' : 'rgba(255,255,255,0.8)';
        ctx.font = '600 9px "Segoe UI Variable Text", "Segoe UI", system-ui';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText(String(i + 1), x, y + 0.5);
      });

      // Selected readout
      if (sel) {
        const label = `${fmtHz(sel.freq)}   ${fmtDb(sel.gainDb)}   Q ${sel.q.toFixed(2)}`;
        ctx.font = '500 11px "Segoe UI Variable Text", "Segoe UI", system-ui';
        ctx.textAlign = 'right';
        ctx.textBaseline = 'top';
        ctx.fillStyle = 'rgba(255,255,255,0.55)';
        ctx.fillText(label, PAD.l + pw, 2);
      }
      return animating;
    },
    [params, selected, resp, selResp],
  );

  // Pointer interaction
  const toBand = (clientX: number, clientY: number, canvas: HTMLCanvasElement) => {
    const r = canvas.getBoundingClientRect();
    const { w, h } = geom.current;
    const pw = w - PAD.l - PAD.r;
    const ph = h - PAD.t - PAD.b;
    const x = clientX - r.left;
    const y = clientY - r.top;
    const f = 20 * Math.pow(1000, clamp((x - PAD.l) / pw, 0, 1));
    const db = RANGE - (clamp((y - PAD.t) / ph, 0, 1) * 2 * RANGE);
    return {
      x,
      y,
      freq: clamp(roundFreq(f), EQ_LIMITS.freq.min, EQ_LIMITS.freq.max),
      gainDb: clamp(Math.round(db * 10) / 10, EQ_LIMITS.gain.min, EQ_LIMITS.gain.max),
    };
  };

  const hit = (x: number, y: number) => {
    const { w, h } = geom.current;
    const pw = w - PAD.l - PAD.r;
    const ph = h - PAD.t - PAD.b;
    let best = -1;
    let bestD = 16;
    params.eqBands.forEach((b, i) => {
      const bx = PAD.l + (Math.log(b.freq / 20) / Math.log(1000)) * pw;
      const by = PAD.t + (1 - (clamp(b.gainDb, -RANGE, RANGE) + RANGE) / (2 * RANGE)) * ph;
      const d = Math.hypot(bx - x, by - y);
      if (d < bestD) {
        bestD = d;
        best = i;
      }
    });
    return best;
  };

  // Wheel over the curve changes Q of the selected band (non-passive).
  const latest = useRef({ params, selected, onBandChange });
  latest.current = { params, selected, onBandChange };
  useEffect(() => {
    const c = ref.current;
    if (!c) return;
    const onWheel = (e: WheelEvent) => {
      const { params: p, selected: s, onBandChange: change } = latest.current;
      const b = p.eqBands[s];
      if (!b || !p.eqEnabled) return;
      e.preventDefault();
      const factor = e.deltaY < 0 ? 1.08 : 1 / 1.08;
      change(s, { q: clamp(Math.round(b.q * factor * 100) / 100, EQ_LIMITS.q.min, EQ_LIMITS.q.max) });
    };
    c.addEventListener('wheel', onWheel, { passive: false });
    return () => c.removeEventListener('wheel', onWheel);
  }, [ref]);

  return (
    <canvas
      ref={ref}
      className={`eq-canvas${params.eqEnabled ? '' : ' is-disabled'}`}
      tabIndex={0}
      role="application"
      aria-label="Equalizer response. Drag numbered handles to set frequency and gain, scroll to change Q, arrow keys adjust the selected band."
      onPointerDown={(e) => {
        if (!params.eqEnabled) return;
        const c = e.currentTarget;
        const { x, y } = toBand(e.clientX, e.clientY, c);
        const i = hit(x, y);
        if (i >= 0) {
          onSelect(i);
          dragging.current = i;
          c.setPointerCapture(e.pointerId);
        }
      }}
      onPointerMove={(e) => {
        const i = dragging.current;
        if (i === null) return;
        const { freq, gainDb } = toBand(e.clientX, e.clientY, e.currentTarget);
        const b = params.eqBands[i];
        if (b && (b.freq !== freq || b.gainDb !== gainDb)) onBandChange(i, { freq, gainDb });
      }}
      onPointerUp={() => (dragging.current = null)}
      onPointerCancel={() => (dragging.current = null)}
      onDoubleClick={(e) => {
        if (!params.eqEnabled) return;
        const { x, y } = toBand(e.clientX, e.clientY, e.currentTarget);
        const i = hit(x, y);
        if (i >= 0) onBandChange(i, { gainDb: 0 });
      }}
      onKeyDown={(e) => {
        // Read the store (not props) so rapid key repeats accumulate correctly.
        const cur = getSnapshot().state?.params ?? params;
        const b = cur.eqBands[selected];
        if (!b || !cur.eqEnabled) return;
        let patch: Partial<EqBand> | null = null;
        const fine = e.shiftKey;
        if (e.key === 'ArrowUp') patch = { gainDb: clamp(Math.round((b.gainDb + (fine ? 0.1 : 0.5)) * 10) / 10, -12, 12) };
        else if (e.key === 'ArrowDown') patch = { gainDb: clamp(Math.round((b.gainDb - (fine ? 0.1 : 0.5)) * 10) / 10, -12, 12) };
        else if (e.key === 'ArrowRight') patch = { freq: clamp(roundFreq(b.freq * Math.pow(2, fine ? 1 / 24 : 1 / 6)), 20, 20000) };
        else if (e.key === 'ArrowLeft') patch = { freq: clamp(roundFreq(b.freq / Math.pow(2, fine ? 1 / 24 : 1 / 6)), 20, 20000) };
        else if (e.key === 'Tab' && !e.altKey) return;
        else if (/^[0-9]$/.test(e.key)) {
          const n = e.key === '0' ? 9 : Number(e.key) - 1;
          if (n < params.eqBands.length) onSelect(n);
          e.preventDefault();
          return;
        }
        if (patch) {
          e.preventDefault();
          onBandChange(selected, patch);
        }
      }}
    />
  );
}
