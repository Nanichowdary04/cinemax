import { useEffect, useRef, type DependencyList } from 'react';
import { meterStore } from '../bridge/store';

/**
 * Draw callback. Return true while the drawing is still animating (e.g. bars
 * decaying); when it returns false the loop idles until new meter data, a
 * resize, or a dependency change marks it dirty again.
 */
export type DrawFn = (ctx: CanvasRenderingContext2D, w: number, h: number, dt: number, now: number) => boolean;

export function useAnimatedCanvas(draw: DrawFn, deps: DependencyList = []) {
  const ref = useRef<HTMLCanvasElement>(null);
  const drawRef = useRef(draw);
  drawRef.current = draw;
  const dirty = useRef(true);

  useEffect(() => {
    dirty.current = true;
  }, deps);

  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    let w = 0;
    let h = 0;
    let dpr = 1;
    let raf = 0;
    let last = performance.now();
    let lastSeq = -1;
    let animating = true;

    const resize = () => {
      const r = canvas.getBoundingClientRect();
      dpr = window.devicePixelRatio || 1;
      w = r.width;
      h = r.height;
      const pw = Math.max(1, Math.round(w * dpr));
      const ph = Math.max(1, Math.round(h * dpr));
      if (canvas.width !== pw || canvas.height !== ph) {
        canvas.width = pw;
        canvas.height = ph;
      }
      dirty.current = true;
    };
    const ro = new ResizeObserver(resize);
    ro.observe(canvas);
    resize();

    const frame = (now: number) => {
      raf = requestAnimationFrame(frame);
      const dt = Math.min(0.1, Math.max(0, (now - last) / 1000));
      last = now;
      const seqChanged = meterStore.seq !== lastSeq;
      if (!(animating || seqChanged || dirty.current)) return;
      if (w < 2 || h < 2) return;
      lastSeq = meterStore.seq;
      dirty.current = false;
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      animating = drawRef.current(ctx, w, h, dt, now) === true;
    };
    raf = requestAnimationFrame(frame);
    return () => {
      cancelAnimationFrame(raf);
      ro.disconnect();
    };
  }, []);

  return ref;
}

/** Exponential approach with separate rise/fall time constants (seconds). */
export function approach(cur: number, target: number, dt: number, tauUp: number, tauDown: number): number {
  const tau = target > cur ? tauUp : tauDown;
  if (tau <= 0) return target;
  const k = 1 - Math.exp(-dt / tau);
  return cur + (target - cur) * k;
}

export function prefersReducedMotion(): boolean {
  return typeof window !== 'undefined' && window.matchMedia?.('(prefers-reduced-motion: reduce)').matches === true;
}

export function cssVar(name: string, fallback: string): string {
  const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  return v || fallback;
}
