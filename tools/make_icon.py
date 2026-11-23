"""Renders the CINEMAX app icon (app/resources/cinemax_icon.png) with no dependencies.

Design: charcoal rounded square, a bold white "C" ring opening to the right, and a
single orange dot in the opening (sound leaving the frame). Anti-aliased with
signed-distance coverage. Usage:  python tools/make_icon.py
"""
import math
import os
import struct
import zlib

SIZE = 512
OUT = os.path.join(os.path.dirname(__file__), "..", "app", "resources", "cinemax_icon.png")


def clamp(x, lo=0.0, hi=1.0):
    return lo if x < lo else hi if x > hi else x


def sd_round_box(px, py, cx, cy, hw, hh, r):
    qx = abs(px - cx) - (hw - r)
    qy = abs(py - cy) - (hh - r)
    ox, oy = max(qx, 0.0), max(qy, 0.0)
    return math.hypot(ox, oy) + min(max(qx, qy), 0.0) - r


def sd_arc(px, py, cx, cy, radius, thick, gap_half):
    """Ring with a gap centred on angle 0 (pointing right), round caps."""
    dx, dy = px - cx, py - cy
    ang = abs(math.atan2(dy, dx))
    if ang >= gap_half:
        return abs(math.hypot(dx, dy) - radius) - thick
    # nearest cap centre
    capx = cx + radius * math.cos(gap_half)
    capy = cy + radius * math.sin(gap_half) * (1 if dy >= 0 else -1)
    return math.hypot(px - capx, py - capy) - thick


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def render():
    pixels = bytearray()
    c = SIZE / 2
    top, bottom = (24, 24, 27), (8, 8, 9)
    white, orange = (242, 242, 240), (255, 106, 26)

    for y in range(SIZE):
        pixels.append(0)  # PNG filter: none
        for x in range(SIZE):
            px, py = x + 0.5, y + 0.5

            bg_cov = clamp(0.5 - sd_round_box(px, py, c, c, 232, 232, 108))
            col = mix(top, bottom, py / SIZE)

            # subtle top highlight rim
            rim = clamp(0.5 - abs(sd_round_box(px, py, c, c, 232, 232, 108) + 1.5)) * (1.0 - py / SIZE) * 0.35
            col = mix(col, (90, 90, 96), rim)

            ring_cov = clamp(0.5 - sd_arc(px, py, c, c, 128, 30, math.radians(40)))
            col = mix(col, white, ring_cov)

            dot_cov = clamp(0.5 - (math.hypot(px - (c + 128), py - c) - 24))
            col = mix(col, orange, dot_cov)

            a = bg_cov
            pixels += bytes((int(col[0] + 0.5), int(col[1] + 0.5), int(col[2] + 0.5), int(a * 255 + 0.5)))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(pixels), 9))
    png += chunk(b"IEND", b"")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(png)
    print("wrote", os.path.abspath(OUT))


if __name__ == "__main__":
    render()
