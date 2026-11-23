"""Convert a measured HRTF (SOFA file, SimpleFreeFieldHRIR) into CINEMAX's .chrtf format.

What it does, per set:
  1. reads every measured direction (azimuth/elevation) and its left/right impulse responses
  2. resamples to 48 kHz and rotates the set so its acoustic front (equal level in both ears) is at 0
  3. measures the interaural time difference (cross-correlation below 1.5 kHz)
  4. diffuse-field equalisation: divides out the average spectrum over all directions, so the
     microphones / ear canal / measurement chain don't colour the sound and every set has the
     same overall tone and loudness (only the direction-dependent part remains)
  5. keeps the bass flat below ~150 Hz (measurement speakers can't reproduce it reliably)
  6. re-samples the directions onto CINEMAX's grid (azimuth 0..355 every 5 deg clockwise,
     elevation -30..+90 every 15 deg) by interpolating log-magnitude spectra of the nearest
     measured directions on the sphere
  7. turns each response into a short minimum-phase filter (128 taps) - the delay is kept separately

Usage:
  python convert_sofa.py IN.sofa OUT.chrtf --key ku100 --name "Dummy head (KU100)" --info "SADIE II, CC BY 4.0"
"""
from __future__ import annotations

import argparse
import struct
import sys

import h5py
import numpy as np
from scipy.signal import resample_poly

FS = 48000
TAPS = 128
NFFT = 1024
AZ_STEP = 5.0
EL_MIN, EL_MAX, EL_STEP = -30.0, 90.0, 15.0


def read_sofa(path):
    with h5py.File(path, "r") as f:
        ir = np.asarray(f["Data.IR"], dtype=np.float64)              # (M, R, N)
        fs = float(np.asarray(f["Data.SamplingRate"]).ravel()[0])
        pos = np.asarray(f["SourcePosition"], dtype=np.float64)       # (M, 3) or (1, 3)
        ptype = f["SourcePosition"].attrs.get("Type", b"spherical")
        ptype = ptype.decode() if isinstance(ptype, bytes) else str(ptype)
        delay = np.asarray(f["Data.Delay"], dtype=np.float64) if "Data.Delay" in f else None
    if ir.ndim != 3 or ir.shape[1] != 2:
        raise SystemExit(f"expected Data.IR with 2 receivers, got {ir.shape}")
    if pos.shape[0] == 1 and ir.shape[0] > 1:
        raise SystemExit("one source position for many measurements is not supported")
    if "cart" in ptype.lower():
        x, y, z = pos[:, 0], pos[:, 1], pos[:, 2]
        az = np.degrees(np.arctan2(y, x))
        el = np.degrees(np.arctan2(z, np.hypot(x, y)))
    else:
        az, el = pos[:, 0], pos[:, 1]
    if delay is not None and np.any(np.abs(delay) > 0):
        # Some sets store a per-measurement delay separately; fold it back in.
        d = np.broadcast_to(delay.reshape(-1, delay.shape[-1]), (ir.shape[0], 2)).round().astype(int)
        n = ir.shape[2] + int(d.max())
        full = np.zeros((ir.shape[0], 2, n))
        for m in range(ir.shape[0]):
            for r in range(2):
                full[m, r, d[m, r]:d[m, r] + ir.shape[2]] = ir[m, r]
        ir = full
    # SOFA azimuth is counter-clockwise (left positive); CINEMAX uses clockwise (right positive).
    az_cw = np.mod(-az, 360.0)
    return ir, fs, az_cw, el


def acoustic_front(ir, az, el, max_offset=5.0):
    """Azimuth (deg, clockwise) where a horizontal source is equally loud in both ears.
    Measurement rigs are often rotated by a degree or two; near the front the balance changes
    by ~0.5 dB per degree, so this offset would pull centred sounds to one side."""
    ring = np.abs(el) < 0.5
    a = np.mod(az[ring] + 180.0, 360.0) - 180.0
    near = np.abs(a) <= 20.0
    if near.sum() < 3:
        return 0.0
    e = (ir[ring][near] ** 2).sum(-1)
    bal = 10 * np.log10(e[:, 0] / e[:, 1])
    order = np.argsort(a[near])
    x, y = a[near][order], bal[order]
    # balance falls as the source moves right: find the zero crossing
    for i in range(len(x) - 1):
        if y[i] >= 0.0 >= y[i + 1] and y[i] != y[i + 1]:
            off = x[i] + (x[i + 1] - x[i]) * y[i] / (y[i] - y[i + 1])
            return float(np.clip(off, -max_offset, max_offset))
    return 0.0


def interaural_delays(ir, up=8, chunk=500):
    """Interaural time difference per direction from the cross-correlation of the low-passed
    (< 1.5 kHz, where ITD is the dominant cue) left and right responses, 8x upsampled.
    Returns (M, 2) delays in samples: the far ear is delayed, the near ear gets 0.
    (Threshold onsets proved unreliable: pre-ringing at the far ear.)"""
    from scipy.signal import butter, sosfilt
    sos = butter(4, 1500, fs=FS, output="sos")
    lp = sosfilt(sos, ir, axis=-1)
    M, _, N = ir.shape
    n = N * up
    nfft = 1 << int(np.ceil(np.log2(2 * n)))
    itd = np.zeros(M)
    for s0 in range(0, M, chunk):
        u = resample_poly(lp[s0:s0 + chunk], up, 1, axis=-1)          # (m, 2, n)
        L = np.fft.rfft(u[:, 0], nfft, axis=-1)
        R = np.fft.rfft(u[:, 1], nfft, axis=-1)
        xc = np.fft.irfft(L * np.conj(R), nfft, axis=-1)               # lag k: left delayed by k
        maxlag = int(0.0011 * FS * up)
        lags = np.concatenate([xc[:, :maxlag + 1], xc[:, -maxlag:]], axis=1)
        idx = np.argmax(lags, axis=1)
        k = np.where(idx <= maxlag, idx, idx - (2 * maxlag + 1))
        itd[s0:s0 + chunk] = k / up
    out = np.zeros((M, 2))
    out[:, 0] = np.maximum(itd, 0.0)
    out[:, 1] = np.maximum(-itd, 0.0)
    return out


def onsets(ir, up=8):
    """Onset (samples) of every impulse response in ir (..., N): first point reaching 15% of the
    peak, found on an 8x upsampled copy for sub-sample precision."""
    hu = np.abs(resample_poly(ir, up, 1, axis=-1))
    thr = 0.15 * hu.max(axis=-1, keepdims=True)
    return np.argmax(hu >= thr, axis=-1) / up


def smoothing_matrix(freqs, fraction):
    """Matrix S so that sqrt((mag**2) @ S.T) is a fractional-octave (power) smoothing."""
    n = len(freqs)
    S = np.zeros((n, n))
    for i, f in enumerate(freqs):
        if f <= 0:
            S[i, i] = 1.0
            continue
        lo, hi = f * 2 ** (-0.5 / fraction), f * 2 ** (0.5 / fraction)
        sel = (freqs >= lo) & (freqs <= hi)
        if not sel.any():
            sel[i] = True
        S[i, sel] = 1.0 / sel.sum()
    return S


def smooth_octave(mag, freqs, fraction=6):
    """Fractional-octave smoothing along the last axis (power average)."""
    S = smoothing_matrix(freqs, fraction)
    return np.sqrt(np.maximum((mag ** 2) @ S.T, 0.0))


def min_phase_from_mag(mag, taps):
    """Minimum-phase impulse response with the given one-sided magnitude (NFFT/2+1 bins)."""
    logmag = np.log(np.maximum(mag, 1e-6))
    full = np.concatenate([logmag, logmag[-2:0:-1]])
    cep = np.fft.ifft(full).real
    n = len(cep)
    fold = np.zeros(n)
    fold[0] = cep[0]
    fold[1:n // 2] = 2 * cep[1:n // 2]
    fold[n // 2] = cep[n // 2]
    h = np.fft.ifft(np.exp(np.fft.fft(fold))).real[:taps]
    win = np.ones(taps)
    tail = taps // 8
    win[-tail:] = 0.5 + 0.5 * np.cos(np.linspace(0, np.pi, tail))
    return h * win


def unit_vectors(az_deg, el_deg):
    az, el = np.radians(az_deg), np.radians(el_deg)
    return np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], axis=-1)


def convert(path, out, key, name, info, max_el_gap=40.0):
    ir, fs, az, el = read_sofa(path)
    if fs != FS:
        from math import gcd
        g = gcd(int(fs), FS)
        ir = resample_poly(ir, FS // g, int(fs) // g, axis=2)
    M = ir.shape[0]
    print(f"{key}: {M} directions, {ir.shape[2]} samples @ {fs:.0f} Hz, el {el.min():.0f}..{el.max():.0f}")
    front = acoustic_front(ir, az, el)
    az = np.mod(az - front, 360.0)
    print(f"  acoustic front at {front:+.1f} deg -> rotated to 0")

    # 1) onsets -> ITD (relative to the earlier ear)
    rel = interaural_delays(ir)                                        # (M, 2) far ear delayed

    # 2) magnitude spectra
    spec = np.abs(np.fft.rfft(ir, n=NFFT, axis=2))                    # (M, 2, NFFT/2+1)
    freqs = np.fft.rfftfreq(NFFT, 1.0 / FS)

    # 3) diffuse-field average (weight by cos(elevation) ~ solid angle of a regular grid)
    #    Per ear: this also removes any sensitivity mismatch between the two measurement mics,
    #    which otherwise pulls a centred sound to one side.
    w = np.maximum(np.cos(np.radians(el)), 0.05)
    df = np.sqrt(np.sum(w[:, None, None] * spec ** 2, axis=0) / w.sum())   # (2, bins)
    df = smooth_octave(df, freqs, 3)
    eq = 1.0 / np.maximum(df, 1e-4)
    spec_eq = spec * eq[None, :, :]
    # limit how far equalisation may lift any single direction's spectrum
    spec_eq = np.minimum(spec_eq, 10 ** (18 / 20))

    # 4) flat, equal bass below ~150 Hz (blend up to 300 Hz)
    blend = np.clip((freqs - 150.0) / 150.0, 0.0, 1.0)
    spec_eq = spec_eq * blend[None, None, :] + (1.0 - blend)[None, None, :]
    # gentle smoothing (1/12 octave) removes measurement noise without losing pinna detail
    spec_eq = smooth_octave(spec_eq, freqs, 12)

    # 5) target grid
    az_grid = np.arange(0.0, 360.0, AZ_STEP)
    el_grid = np.arange(EL_MIN, EL_MAX + 0.1, EL_STEP)
    src_v = unit_vectors(az, el)
    logspec = np.log(np.maximum(spec_eq, 1e-6))
    out_ir = np.zeros((len(el_grid), len(az_grid), 2, TAPS))
    out_delay = np.zeros((len(el_grid), len(az_grid), 2))
    lowest = el.min()
    for ei, e in enumerate(el_grid):
        for ai, a in enumerate(az_grid):
            e_eff = max(e, lowest)                           # below the measured range: reuse the lowest ring
            v = unit_vectors(np.array([a]), np.array([e_eff]))[0]
            ang = np.degrees(np.arccos(np.clip(src_v @ v, -1, 1)))
            nn = np.argsort(ang)[:3]
            d = np.maximum(ang[nn], 1e-3)
            wt = 1.0 / d ** 2
            wt /= wt.sum()
            ls = np.tensordot(wt, logspec[nn], axes=1)        # (2, bins)
            out_delay[ei, ai] = wt @ rel[nn]
            for r in range(2):
                out_ir[ei, ai, r] = min_phase_from_mag(np.exp(ls[r]), TAPS)

    # 6) loudness: frontal source at 1 kHz ~ 0 dB per ear
    k = int(round(1000 / (FS / NFFT)))
    front = np.abs(np.fft.rfft(out_ir[int(round((0 - EL_MIN) / EL_STEP)), 0], n=NFFT, axis=-1))[:, k].mean()
    out_ir /= max(front, 1e-6)

    scale = float(np.abs(out_ir).max()) * 1.0001
    q = np.round(out_ir / scale * 32767).astype("<i2")
    with open(out, "wb") as fh:
        fh.write(b"CHRTF01\0")
        fh.write(struct.pack("<IIII", FS, TAPS, len(az_grid), len(el_grid)))
        fh.write(struct.pack("<ffff", AZ_STEP, EL_MIN, EL_STEP, scale))
        for s in (key, name, info):
            b = s.encode("utf-8")
            fh.write(struct.pack("<I", len(b)))
            fh.write(b)
        fh.write(out_delay.astype("<f4").tobytes())
        fh.write(q.tobytes())
    itd = out_delay[int(round((0 - EL_MIN) / EL_STEP)), int(90 / AZ_STEP)]
    print(f"  -> {out}: ITD at 90 deg right = {(itd[0] - itd[1]) / FS * 1e6:.0f} us (left-ear delay), "
          f"peak {scale:.2f}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("sofa")
    ap.add_argument("out")
    ap.add_argument("--key", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--info", default="")
    a = ap.parse_args()
    convert(a.sofa, a.out, a.key, a.name, a.info)
