/** CINEMAX mark: an open aperture "C" with a projector point of light. */
export function LogoMark({ size = 30 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 32 32" aria-hidden="true" className="logo-mark">
      <defs>
        <radialGradient id="cmx-light" cx="0.5" cy="0.5" r="0.5">
          <stop offset="0" stopColor="#ffb07a" />
          <stop offset="0.55" stopColor="var(--accent)" />
          <stop offset="1" stopColor="var(--accent)" stopOpacity="0" />
        </radialGradient>
      </defs>
      <path d="M25.19 8.29A12 12 0 1 0 25.19 23.71" fill="none" stroke="#f5f5f6" strokeWidth="2.4" strokeLinecap="round" />
      <path d="M21.36 11.5A7 7 0 1 0 21.36 20.5" fill="none" stroke="#f5f5f6" strokeOpacity="0.45" strokeWidth="1.6" strokeLinecap="round" />
      <circle cx="26.2" cy="16" r="4.2" fill="url(#cmx-light)" opacity="0.55" />
      <circle cx="26.2" cy="16" r="2.1" fill="var(--accent)" />
    </svg>
  );
}

export function Wordmark() {
  return (
    <span className="wordmark" aria-label="CINEMAX">
      CINEMAX
    </span>
  );
}
