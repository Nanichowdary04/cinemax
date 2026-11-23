import { useCallback, useEffect, useId, useRef, type CSSProperties, type KeyboardEvent, type ReactNode } from 'react';
import { clamp, roundTo } from '../lib/format';

// ---------------------------------------------------------------------------
//  Master engine switch (big, tactile)
export function MasterSwitch({
  on,
  pending,
  disabled,
  onChange,
}: {
  on: boolean;
  pending?: boolean;
  disabled?: boolean;
  onChange: (on: boolean) => void;
}) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={on}
      aria-label="CINEMAX engine"
      disabled={disabled}
      className={`master-switch${on ? ' is-on' : ''}${pending ? ' is-pending' : ''}`}
      onClick={() => onChange(!on)}
    >
      <span className="master-switch__text">
        <span className="master-switch__label">Engine</span>
        <span className="master-switch__state">{on ? 'ON' : 'OFF'}</span>
      </span>
      <span className="master-switch__track" aria-hidden="true">
        <span className="master-switch__knob" />
      </span>
    </button>
  );
}

// ---------------------------------------------------------------------------
//  Small toggle
export function Toggle({
  checked,
  onChange,
  label,
  disabled,
  size = 'md',
}: {
  checked: boolean;
  onChange: (v: boolean) => void;
  label: string;
  disabled?: boolean;
  size?: 'sm' | 'md';
}) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      aria-label={label}
      disabled={disabled}
      className={`toggle toggle--${size}${checked ? ' is-on' : ''}`}
      onClick={() => onChange(!checked)}
    >
      <span className="toggle__knob" />
    </button>
  );
}

/** A labelled settings row with a toggle on the right. */
export function ToggleRow({
  title,
  description,
  checked,
  onChange,
  disabled,
}: {
  title: string;
  description?: ReactNode;
  checked: boolean;
  onChange: (v: boolean) => void;
  disabled?: boolean;
}) {
  return (
    <div className={`row${disabled ? ' is-disabled' : ''}`}>
      <div className="row__text">
        <div className="row__title">{title}</div>
        {description && <div className="row__desc">{description}</div>}
      </div>
      <Toggle checked={checked} onChange={onChange} label={title} disabled={disabled} />
    </div>
  );
}

// ---------------------------------------------------------------------------
//  Slider (native range input, custom skin)
export function Slider({
  label,
  value,
  min,
  max,
  step,
  onChange,
  format,
  bipolar,
  reset,
  disabled,
  hint,
}: {
  label: string;
  value: number;
  min: number;
  max: number;
  step: number;
  onChange: (v: number) => void;
  format: (v: number) => string;
  bipolar?: boolean;
  reset?: number;
  disabled?: boolean;
  hint?: string;
}) {
  const id = useId();
  const pct = ((clamp(value, min, max) - min) / (max - min)) * 100;
  const zero = bipolar ? ((clamp(0, min, max) - min) / (max - min)) * 100 : 0;
  const a = Math.min(zero, pct);
  const b = Math.max(zero, pct);
  const style = { '--a': `${a}%`, '--b': `${b}%`, '--pct': `${pct}%` } as CSSProperties;
  return (
    <div className={`slider${disabled ? ' is-disabled' : ''}`} title={hint}>
      <div className="slider__head">
        <label htmlFor={id} className="slider__label">
          {label}
        </label>
        <output htmlFor={id} className="slider__value">
          {format(value)}
        </output>
      </div>
      <input
        id={id}
        type="range"
        min={min}
        max={max}
        step={step}
        value={value}
        disabled={disabled}
        style={style}
        onChange={(e) => onChange(roundTo(Number(e.target.value), step))}
        onDoubleClick={() => reset !== undefined && onChange(reset)}
        aria-valuetext={format(value)}
      />
    </div>
  );
}

// ---------------------------------------------------------------------------
//  Rotary knob (270° arc). Drag vertically, wheel, arrows; double-click resets.
const ARC = 135; // degrees either side of 12 o'clock

function polar(cx: number, cy: number, r: number, deg: number) {
  const rad = (deg * Math.PI) / 180;
  return { x: cx + r * Math.sin(rad), y: cy - r * Math.cos(rad) };
}

function arcPath(cx: number, cy: number, r: number, from: number, to: number) {
  if (Math.abs(to - from) < 0.01) return '';
  const a = Math.min(from, to);
  const b = Math.max(from, to);
  const s = polar(cx, cy, r, a);
  const e = polar(cx, cy, r, b);
  const large = b - a > 180 ? 1 : 0;
  return `M${s.x.toFixed(2)} ${s.y.toFixed(2)}A${r} ${r} 0 ${large} 1 ${e.x.toFixed(2)} ${e.y.toFixed(2)}`;
}

export function Knob({
  label,
  value,
  min,
  max,
  step,
  onChange,
  format,
  bipolar,
  reset,
  size = 88,
  disabled,
  accent,
  children,
}: {
  label: string;
  value: number;
  min: number;
  max: number;
  step: number;
  onChange: (v: number) => void;
  format: (v: number) => string;
  bipolar?: boolean;
  reset?: number;
  size?: number;
  disabled?: boolean;
  accent?: boolean;
  children?: ReactNode;
}) {
  const drag = useRef<{ y: number; v: number } | null>(null);
  const norm = (clamp(value, min, max) - min) / (max - min);
  const angle = -ARC + norm * 2 * ARC;
  const zeroAngle = bipolar ? -ARC + ((clamp(0, min, max) - min) / (max - min)) * 2 * ARC : -ARC;
  const c = size / 2;
  const r = size / 2 - 7;
  const tick = polar(c, c, r - 9, angle);
  const tick2 = polar(c, c, r - 17, angle);

  const commit = useCallback(
    (v: number) => {
      const nv = roundTo(clamp(v, min, max), step);
      if (nv !== value) onChange(nv);
    },
    [min, max, step, value, onChange],
  );

  // Non-passive wheel listener so turning the knob doesn't scroll the page.
  const dialRef = useRef<HTMLDivElement>(null);
  const wheelRef = useRef<(e: WheelEvent) => void>(() => {});
  wheelRef.current = (e: WheelEvent) => {
    if (disabled) return;
    e.preventDefault();
    commit(value + (e.deltaY < 0 ? step : -step) * (e.shiftKey ? 1 : 2));
  };
  useEffect(() => {
    const el = dialRef.current;
    if (!el) return;
    const h = (e: WheelEvent) => wheelRef.current(e);
    el.addEventListener('wheel', h, { passive: false });
    return () => el.removeEventListener('wheel', h);
  }, []);

  const onKey = (e: KeyboardEvent) => {
    const big = (max - min) / 10;
    let v: number | null = null;
    if (e.key === 'ArrowUp' || e.key === 'ArrowRight') v = value + step;
    else if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') v = value - step;
    else if (e.key === 'PageUp') v = value + big;
    else if (e.key === 'PageDown') v = value - big;
    else if (e.key === 'Home') v = min;
    else if (e.key === 'End') v = max;
    if (v !== null) {
      e.preventDefault();
      commit(v);
    }
  };

  return (
    <div className={`knob${disabled ? ' is-disabled' : ''}${accent ? ' knob--accent' : ''}`} style={{ width: size }}>
      <div
        ref={dialRef}
        className="knob__dial"
        role="slider"
        tabIndex={disabled ? -1 : 0}
        aria-label={label}
        aria-valuemin={min}
        aria-valuemax={max}
        aria-valuenow={value}
        aria-valuetext={format(value)}
        aria-disabled={disabled}
        style={{ width: size, height: size }}
        onKeyDown={onKey}
        onDoubleClick={() => !disabled && reset !== undefined && commit(reset)}
        onPointerDown={(e) => {
          if (disabled) return;
          (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
          drag.current = { y: e.clientY, v: value };
        }}
        onPointerMove={(e) => {
          if (!drag.current) return;
          const range = max - min;
          const sens = e.shiftKey ? 600 : 180;
          commit(drag.current.v + ((drag.current.y - e.clientY) / sens) * range);
        }}
        onPointerUp={() => (drag.current = null)}
        onPointerCancel={() => (drag.current = null)}
      >
        <svg width={size} height={size} aria-hidden="true">
          <path d={arcPath(c, c, r, -ARC, ARC)} className="knob__track" />
          <path d={arcPath(c, c, r, zeroAngle, angle)} className="knob__fill" />
          <circle cx={c} cy={c} r={r - 12} className="knob__cap" />
          <line x1={tick2.x} y1={tick2.y} x2={tick.x} y2={tick.y} className="knob__tick" />
        </svg>
        {children && <div className="knob__inner">{children}</div>}
      </div>
      <div className="knob__value">{format(value)}</div>
      <div className="knob__label">{label}</div>
    </div>
  );
}

// ---------------------------------------------------------------------------
//  Segmented control
export function Segmented<T extends string | number>({
  options,
  value,
  onChange,
  label,
  disabled,
}: {
  options: { value: T; label: string; hint?: string }[];
  value: T;
  onChange: (v: T) => void;
  label: string;
  disabled?: boolean;
}) {
  const idx = Math.max(0, options.findIndex((o) => o.value === value));
  return (
    <div
      className={`segmented${disabled ? ' is-disabled' : ''}`}
      role="radiogroup"
      aria-label={label}
      style={{ '--n': options.length, '--i': idx } as CSSProperties}
    >
      <span className="segmented__thumb" aria-hidden="true" />
      {options.map((o) => (
        <button
          key={String(o.value)}
          type="button"
          role="radio"
          aria-checked={o.value === value}
          title={o.hint}
          disabled={disabled}
          className={o.value === value ? 'is-active' : ''}
          onClick={() => onChange(o.value)}
        >
          {o.label}
        </button>
      ))}
    </div>
  );
}

// ---------------------------------------------------------------------------
//  Glass panel
export function Panel({
  title,
  eyebrow,
  icon,
  actions,
  children,
  className = '',
  style,
}: {
  title?: ReactNode;
  eyebrow?: ReactNode;
  icon?: ReactNode;
  actions?: ReactNode;
  children?: ReactNode;
  className?: string;
  style?: CSSProperties;
}) {
  return (
    <section className={`panel ${className}`} style={style}>
      {(title || eyebrow || actions) && (
        <header className="panel__head">
          <div className="panel__titles">
            {eyebrow && <div className="eyebrow">{eyebrow}</div>}
            {title && (
              <h2 className="panel__title">
                {icon && <span className="panel__icon">{icon}</span>}
                {title}
              </h2>
            )}
          </div>
          {actions && <div className="panel__actions">{actions}</div>}
        </header>
      )}
      {children}
    </section>
  );
}

export function Badge({ children, tone = 'neutral', title }: { children: ReactNode; tone?: 'neutral' | 'accent' | 'warn' | 'muted' | 'solid'; title?: string }) {
  return (
    <span className={`badge badge--${tone}`} title={title}>
      {children}
    </span>
  );
}

export function Button({
  children,
  onClick,
  variant = 'ghost',
  disabled,
  icon,
  title,
  type = 'button',
}: {
  children?: ReactNode;
  onClick?: () => void;
  variant?: 'ghost' | 'primary' | 'danger' | 'quiet';
  disabled?: boolean;
  icon?: ReactNode;
  title?: string;
  type?: 'button' | 'submit';
}) {
  return (
    <button type={type} className={`btn btn--${variant}`} onClick={onClick} disabled={disabled} title={title}>
      {icon && <span className="btn__icon">{icon}</span>}
      {children && <span>{children}</span>}
    </button>
  );
}
