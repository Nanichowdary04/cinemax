import type { ReactNode, SVGProps } from 'react';
import type { DeviceKind, PresetId } from '../bridge/types';

type IconProps = { size?: number; className?: string; strokeWidth?: number } & Omit<SVGProps<SVGSVGElement>, 'children'>;

function Svg({ size = 20, className, strokeWidth = 1.6, children, ...rest }: IconProps & { children: ReactNode }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth={strokeWidth}
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
      focusable="false"
      className={className}
      {...rest}
    >
      {children}
    </svg>
  );
}

// ---- Navigation -----------------------------------------------------------
/** Picture (CINEMAX Vision): a screen with a sun-like brightness mark. */
export const IconTv = (p: IconProps) => (
  <Svg {...p}>
    <rect x="2.5" y="5" width="19" height="12" rx="1.6" />
    <path d="M8 20h8M9.5 2.8 12 5l2.5-2.2" />
  </Svg>
);

export const IconPicture = (p: IconProps) => (
  <Svg {...p}>
    <rect x="3" y="4.5" width="18" height="12" rx="2" />
    <path d="M8.5 20h7M12 16.5V20" />
    <circle cx="12" cy="10.5" r="2.2" />
    <path d="M12 6.6v.9M12 13.5v.9M8.1 10.5h.9M15 10.5h.9" />
  </Svg>
);

export const IconDashboard = (p: IconProps) => (
  <Svg {...p}>
    <path d="M4.5 16.5a8 8 0 1 1 15 0" />
    <path d="M12 12.5l3.6-3.6" />
    <circle cx="12" cy="13" r="1.3" fill="currentColor" stroke="none" />
    <path d="M4 19.5h16" />
  </Svg>
);

export const IconEqualizer = (p: IconProps) => (
  <Svg {...p}>
    <path d="M6 4v16M12 4v16M18 4v16" />
    <rect x="4" y="13" width="4" height="2.6" rx="1.2" fill="currentColor" stroke="none" />
    <rect x="10" y="7" width="4" height="2.6" rx="1.2" fill="currentColor" stroke="none" />
    <rect x="16" y="11" width="4" height="2.6" rx="1.2" fill="currentColor" stroke="none" />
  </Svg>
);

export const IconEffects = (p: IconProps) => (
  <Svg {...p}>
    <path d="M3 13c1.6 0 1.6-5 3.2-5s1.6 8 3.2 8 1.6-11 3.2-11 1.6 14 3.2 14 1.6-6 3.2-6" />
    <path d="M19.5 3.5v3M18 5h3" />
  </Svg>
);

export const IconDevices = (p: IconProps) => (
  <Svg {...p}>
    <path d="M4 15v-2.5a8 8 0 0 1 16 0V15" />
    <rect x="3.5" y="14" width="4" height="6" rx="1.6" />
    <rect x="16.5" y="14" width="4" height="6" rx="1.6" />
  </Svg>
);

export const IconPresets = (p: IconProps) => (
  <Svg {...p}>
    <rect x="3.5" y="6" width="17" height="12.5" rx="2.2" />
    <path d="M3.5 9.5h17M7 6v3.5M11 6v3.5M15 6v3.5" />
    <path d="M10.5 12.3v4l3.3-2z" fill="currentColor" stroke="none" />
  </Svg>
);

export function IconSettings(p: IconProps) {
  const teeth = Array.from({ length: 8 }, (_, i) => (i * Math.PI) / 4);
  return (
    <Svg {...p}>
      <circle cx="12" cy="12" r="5.6" />
      <circle cx="12" cy="12" r="2.2" />
      {teeth.map((a, i) => (
        <path
          key={i}
          d={`M${(12 + Math.cos(a) * 6.6).toFixed(2)} ${(12 + Math.sin(a) * 6.6).toFixed(2)}L${(12 + Math.cos(a) * 8.8).toFixed(2)} ${(12 + Math.sin(a) * 8.8).toFixed(2)}`}
          strokeWidth={2.4}
        />
      ))}
    </Svg>
  );
}

// ---- Devices ------------------------------------------------------------
export const IconSpeaker = (p: IconProps) => (
  <Svg {...p}>
    <rect x="5.5" y="3" width="13" height="18" rx="2.5" />
    <circle cx="12" cy="14.2" r="3.6" />
    <circle cx="12" cy="14.2" r="1" fill="currentColor" stroke="none" />
    <circle cx="12" cy="6.8" r="1.2" />
  </Svg>
);

export const IconLaptop = (p: IconProps) => (
  <Svg {...p}>
    <rect x="4.5" y="5" width="15" height="10.5" rx="1.6" />
    <path d="M2.5 18.5h19" />
    <path d="M8 10.2h.01M10.5 10.2h.01M13.5 10.2h.01M16 10.2h.01" strokeWidth={2.2} />
  </Svg>
);

export const IconHeadphones = (p: IconProps) => (
  <Svg {...p}>
    <path d="M4 16v-3.5a8 8 0 0 1 16 0V16" />
    <path d="M4 14.5h2.5a1.5 1.5 0 0 1 1.5 1.5v3a1.5 1.5 0 0 1-1.5 1.5H5.5A1.5 1.5 0 0 1 4 19v-4.5z" />
    <path d="M20 14.5h-2.5a1.5 1.5 0 0 0-1.5 1.5v3a1.5 1.5 0 0 0 1.5 1.5h1A1.5 1.5 0 0 0 20 19v-4.5z" />
  </Svg>
);

export const IconHeadset = (p: IconProps) => (
  <Svg {...p}>
    <path d="M4 15v-2.5a8 8 0 0 1 16 0V15" />
    <rect x="3.5" y="13.5" width="4" height="6" rx="1.6" />
    <rect x="16.5" y="13.5" width="4" height="6" rx="1.6" />
    <path d="M18.5 19.5c0 1.5-2 2-5 2" />
  </Svg>
);

export const IconEarbuds = (p: IconProps) => (
  <Svg {...p}>
    <path d="M8 4.5a3.5 3.5 0 0 0-3.5 3.5c0 2 1.3 3.2 3 3.4V19a1.5 1.5 0 0 0 3 0V8A3.5 3.5 0 0 0 8 4.5z" />
    <path d="M16 4.5A3.5 3.5 0 0 1 19.5 8c0 2-1.3 3.2-3 3.4V19a1.5 1.5 0 0 1-3 0V8A3.5 3.5 0 0 1 16 4.5z" />
  </Svg>
);

export const IconBtSpeaker = (p: IconProps) => (
  <Svg {...p}>
    <rect x="6" y="3" width="12" height="18" rx="6" />
    <circle cx="12" cy="13" r="3.2" />
    <path d="M10.5 6.5h3" />
  </Svg>
);

export const IconBluetooth = (p: IconProps) => (
  <Svg {...p}>
    <path d="M7 7.5l10 9-5 4.5v-18l5 4.5-10 9" />
  </Svg>
);

export const IconUsb = (p: IconProps) => (
  <Svg {...p}>
    <path d="M12 3v15" />
    <path d="M10 5l2-2.5L14 5" />
    <path d="M12 14l-4-2.5V9" />
    <path d="M12 12l4-2.5V7.5" />
    <rect x="6.8" y="7.6" width="2.4" height="2" rx=".4" />
    <circle cx="16" cy="6.6" r="1.1" />
    <circle cx="12" cy="19.5" r="1.8" />
  </Svg>
);

export const IconMonitor = (p: IconProps) => (
  <Svg {...p}>
    <rect x="3" y="4" width="18" height="12.5" rx="1.8" />
    <path d="M9 20.5h6M12 16.5v4" />
  </Svg>
);

export const IconDigital = (p: IconProps) => (
  <Svg {...p}>
    <rect x="4" y="4" width="16" height="16" rx="3" />
    <circle cx="12" cy="12" r="3.5" />
    <circle cx="12" cy="12" r=".9" fill="currentColor" stroke="none" />
  </Svg>
);

export const IconLineOut = (p: IconProps) => (
  <Svg {...p}>
    <path d="M12 2.5v5" />
    <rect x="9" y="7.5" width="6" height="4" rx="1" />
    <path d="M10.5 11.5v5.5a1.5 1.5 0 0 0 3 0v-5.5" />
    <path d="M12 18.5v3" />
  </Svg>
);

export const IconVirtual = (p: IconProps) => (
  <Svg {...p}>
    <path d="M7 8.5A4.5 4.5 0 1 0 7 17.5h3" />
    <path d="M17 15.5a4.5 4.5 0 1 0 0-9h-3" />
    <path d="M8.5 12h7" strokeDasharray="1.6 2" />
  </Svg>
);

export const IconUnknown = (p: IconProps) => (
  <Svg {...p}>
    <circle cx="12" cy="12" r="8.5" />
    <path d="M9.6 9.6a2.5 2.5 0 0 1 4.8.9c0 1.7-2.4 2-2.4 3.5" />
    <path d="M12 16.8h.01" strokeWidth={2.4} />
  </Svg>
);

// ---- UI glyphs ----------------------------------------------------------
export const IconPower = (p: IconProps) => (
  <Svg {...p}>
    <path d="M12 3.5v7" />
    <path d="M7 6.6a7.5 7.5 0 1 0 10 0" />
  </Svg>
);

export const IconWarning = (p: IconProps) => (
  <Svg {...p}>
    <path d="M12 4l9 15.5H3z" />
    <path d="M12 10v4.2M12 17h.01" />
  </Svg>
);

export const IconInfo = (p: IconProps) => (
  <Svg {...p}>
    <circle cx="12" cy="12" r="8.5" />
    <path d="M12 11v5.2M12 7.8h.01" />
  </Svg>
);

export const IconCheck = (p: IconProps) => (
  <Svg {...p}>
    <path d="M5 12.5l4.2 4.2L19 7" />
  </Svg>
);

export const IconClose = (p: IconProps) => (
  <Svg {...p}>
    <path d="M6 6l12 12M18 6L6 18" />
  </Svg>
);

export const IconArrowRight = (p: IconProps) => (
  <Svg {...p}>
    <path d="M4.5 12h15M14 6.5l5.5 5.5-5.5 5.5" />
  </Svg>
);

export const IconChevronRight = (p: IconProps) => (
  <Svg {...p}>
    <path d="M9.5 6l6 6-6 6" />
  </Svg>
);

export const IconShield = (p: IconProps) => (
  <Svg {...p}>
    <path d="M12 3l7.5 3v5.5c0 4.6-3.2 8-7.5 9.5-4.3-1.5-7.5-4.9-7.5-9.5V6z" />
    <path d="M8.8 12.2l2.2 2.2 4.3-4.4" />
  </Svg>
);

export const IconRefresh = (p: IconProps) => (
  <Svg {...p}>
    <path d="M19.5 12a7.5 7.5 0 1 1-2.2-5.3" />
    <path d="M19.5 4v4h-4" />
  </Svg>
);

export const IconExternal = (p: IconProps) => (
  <Svg {...p}>
    <path d="M13.5 4.5h6v6M19.5 4.5l-8 8" />
    <path d="M17.5 13.5v5a1.5 1.5 0 0 1-1.5 1.5H6A1.5 1.5 0 0 1 4.5 18.5V8A1.5 1.5 0 0 1 6 6.5h5" />
  </Svg>
);

export const IconApps = (p: IconProps) => (
  <Svg {...p}>
    <rect x="4" y="4" width="6.5" height="6.5" rx="1.6" />
    <rect x="13.5" y="4" width="6.5" height="6.5" rx="1.6" />
    <rect x="4" y="13.5" width="6.5" height="6.5" rx="1.6" />
    <rect x="13.5" y="13.5" width="6.5" height="6.5" rx="1.6" />
  </Svg>
);

export const IconWave = (p: IconProps) => (
  <Svg {...p}>
    <path d="M4 10v4M8 7v10M12 4v16M16 8v8M20 10.5v3" />
  </Svg>
);

export const IconCompare = (p: IconProps) => (
  <Svg {...p}>
    <path d="M12 3.5v17" />
    <rect x="3.5" y="6" width="17" height="12" rx="2" />
    <path d="M6.5 14.5l2-5 2 5M7.2 13h2.6" />
    <path d="M14 9.5h2.1a1.3 1.3 0 0 1 0 2.6H14zM14 12.1h2.4a1.2 1.2 0 0 1 0 2.4H14z" />
  </Svg>
);

export const IconBass = (p: IconProps) => (
  <Svg {...p}>
    <circle cx="12" cy="12" r="8.5" />
    <circle cx="12" cy="12" r="4.5" />
    <circle cx="12" cy="12" r="1.2" fill="currentColor" stroke="none" />
  </Svg>
);

export const IconVoice = (p: IconProps) => (
  <Svg {...p}>
    <rect x="9" y="3" width="6" height="11" rx="3" />
    <path d="M5.5 11a6.5 6.5 0 0 0 13 0M12 17.5V21" />
  </Svg>
);

export const IconSurround = (p: IconProps) => (
  <Svg {...p}>
    <circle cx="12" cy="12" r="2.2" />
    <path d="M7.8 7.8a6 6 0 0 0 0 8.4M16.2 7.8a6 6 0 0 1 0 8.4" />
    <path d="M5 5a10 10 0 0 0 0 14M19 5a10 10 0 0 1 0 14" />
  </Svg>
);

export const IconDynamics = (p: IconProps) => (
  <Svg {...p}>
    <path d="M3 18.5h18" />
    <path d="M3 18.5L12 9.5h9" />
    <path d="M12 9.5V5.5" strokeDasharray="1.5 2" />
  </Svg>
);

export const IconSliders = IconEqualizer;

// ---- Mappings -------------------------------------------------------------
export function DeviceIcon({ kind, ...p }: IconProps & { kind: DeviceKind }) {
  switch (kind) {
    case 'speakers':
      return <IconLaptop {...p} />;
    case 'headphones':
      return <IconHeadphones {...p} />;
    case 'headset':
      return <IconHeadset {...p} />;
    case 'bluetooth-headphones':
      return <IconHeadphones {...p} />;
    case 'bluetooth-earbuds':
      return <IconEarbuds {...p} />;
    case 'bluetooth-speaker':
      return <IconBtSpeaker {...p} />;
    case 'bluetooth-handsfree':
      return <IconHeadset {...p} />;
    case 'usb':
      return <IconUsb {...p} />;
    case 'hdmi':
      return <IconMonitor {...p} />;
    case 'digital':
      return <IconDigital {...p} />;
    case 'line-out':
      return <IconLineOut {...p} />;
    case 'virtual':
      return <IconVirtual {...p} />;
    default:
      return <IconUnknown {...p} />;
  }
}

export function isBluetooth(kind: DeviceKind) {
  return kind.startsWith('bluetooth');
}

/** Abstract glyphs for preset tiles (stroke drawings on a 48 grid). */
export function PresetGlyph({ id, size = 44 }: { id: PresetId; size?: number }) {
  const common = {
    width: size,
    height: size,
    viewBox: '0 0 48 48',
    fill: 'none',
    stroke: 'currentColor',
    strokeWidth: 1.4,
    strokeLinecap: 'round' as const,
    strokeLinejoin: 'round' as const,
    'aria-hidden': true,
  };
  switch (id) {
    case 'cinema':
      return (
        <svg {...common}>
          <path d="M6 14c12-4 24-4 36 0v20c-12-4-24-4-36 0z" />
          <path d="M20 19.5v9l8-4.5z" fill="currentColor" stroke="none" />
        </svg>
      );
    case 'imax':
      return (
        <svg {...common}>
          <path d="M4 12c13 6 27 6 40 0M4 36c13-6 27-6 40 0" />
          <path d="M4 12v24M44 12v24" />
          <path d="M14 24h20" strokeDasharray="2 3" />
        </svg>
      );
    case 'theatre':
      return (
        <svg {...common}>
          <path d="M6 8h36" />
          <path d="M8 8c0 12 3 22 8 30M40 8c0 12-3 22-8 30" />
          <path d="M16 38h16" />
          <path d="M24 8v6" />
        </svg>
      );
    case 'deep-bass':
      return (
        <svg {...common}>
          <circle cx="24" cy="24" r="17" />
          <circle cx="24" cy="24" r="11" />
          <circle cx="24" cy="24" r="4.5" fill="currentColor" stroke="none" />
        </svg>
      );
    case 'gaming':
      return (
        <svg {...common}>
          <circle cx="24" cy="24" r="16" />
          <circle cx="24" cy="24" r="3" fill="currentColor" stroke="none" />
          <path d="M24 4v8M24 36v8M4 24h8M36 24h8" />
        </svg>
      );
    case 'music':
      return (
        <svg {...common}>
          <path d="M6 28c4 0 4-10 8-10s4 16 8 16 4-22 8-22 4 14 8 14 4-6 4-6" />
        </svg>
      );
    case 'concert-hall':
      return (
        <svg {...common}>
          <path d="M6 40V20c0-8 8-14 18-14s18 6 18 14v20" />
          <path d="M12 40V24M18 40V22M24 40V21M30 40V22M36 40V24" />
          <path d="M4 40h40" />
        </svg>
      );
    case 'dialogue':
      return (
        <svg {...common}>
          <path d="M8 12h22a4 4 0 0 1 4 4v8a4 4 0 0 1-4 4H18l-6 5v-5H8a4 4 0 0 1-4-4v-8a4 4 0 0 1 4-4z" />
          <path d="M38 20h2a4 4 0 0 1 4 4v7a4 4 0 0 1-4 4h-2v4l-5-4h-5" />
          <path d="M11 20h12" />
        </svg>
      );
    case 'night':
      return (
        <svg {...common}>
          <path d="M31 8a15 15 0 1 0 9 22A13 13 0 0 1 31 8z" />
          <path d="M14 40h20" strokeDasharray="2 3" />
        </svg>
      );
    case 'dolby-style':
      return (
        <svg {...common}>
          <path d="M8 12h10a12 12 0 0 1 0 24H8z" />
          <path d="M40 12H30a12 12 0 0 0 0 24h10z" />
        </svg>
      );
    case 'custom':
      return (
        <svg {...common}>
          <path d="M12 8v32M24 8v32M36 8v32" />
          <rect x="8.5" y="25" width="7" height="4.5" rx="2" fill="currentColor" stroke="none" />
          <rect x="20.5" y="14" width="7" height="4.5" rx="2" fill="currentColor" stroke="none" />
          <rect x="32.5" y="30" width="7" height="4.5" rx="2" fill="currentColor" stroke="none" />
        </svg>
      );
    default:
      return (
        <svg {...common}>
          <path d="M6 24h36" />
          <path d="M6 18v12M42 18v12" />
        </svg>
      );
  }
}
