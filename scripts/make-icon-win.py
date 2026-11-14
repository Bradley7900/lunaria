#!/usr/bin/env python3
"""Draw the Lunaria Windows icon (a glowing crescent moon on a night sky) as .ico.

usage: make-icon-win.py OUT.ico [PREVIEW.png]

No imaging library, like make-icon.py: every pixel is evaluated from distance
functions with 4x4 supersampling, each size is rendered on its own (so small
sizes stay sharp instead of being scaled down), and the .ico holds PNG
entries, which Windows reads at every size since Vista.  The .rc next to it
names the icon GLFW_ICON, which GLFW also uses as every window's icon.
"""
import math
import struct
import sys
import zlib

SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)

SKY_TOP = (16, 14, 46)
SKY_BOTTOM = (46, 30, 104)
GLOW = (120, 132, 255)
MOON_LIT = (255, 241, 205)
MOON_EDGE = (255, 205, 120)
CRATER = (230, 200, 150)
STAR = (255, 255, 255)

# Crescent: the lit disk minus a shadow disk (x, y, r) in 0..1 units.
MOON = (0.47, 0.53, 0.30)
BITE = (0.60, 0.41, 0.255)
CRATERS = [(0.33, 0.62, 0.045), (0.40, 0.74, 0.03), (0.29, 0.47, 0.026)]
# (x, y, size): four-point sparkles in the dark part of the sky.
STARS = [(0.76, 0.24, 0.075), (0.82, 0.56, 0.045), (0.60, 0.17, 0.035), (0.24, 0.22, 0.03)]


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def over(dst, src, alpha):
    return mix(dst, src, alpha)


def rounded_square(x, y, radius):
    """Signed distance to a 0..1 square with rounded corners."""
    qx = abs(x - 0.5) - (0.5 - radius)
    qy = abs(y - 0.5) - (0.5 - radius)
    out = math.hypot(max(qx, 0.0), max(qy, 0.0))
    return out + min(max(qx, qy), 0.0) - radius


def circle(x, y, c):
    return math.hypot(x - c[0], y - c[1]) - c[2]


def sparkle(x, y, s):
    """Coverage of a four-point star: thin diamond arms plus a soft core."""
    dx, dy = abs(x - s[0]) / s[2], abs(y - s[1]) / s[2]
    arms = max(0.0, 1.0 - (dx * 7.0 + dy)) + max(0.0, 1.0 - (dy * 7.0 + dx))
    core = max(0.0, 1.0 - math.hypot(dx, dy) * 3.0)
    return min(1.0, arms + core)


def shade(x, y, size):
    """Colour and alpha of one sample point."""
    edge = rounded_square(x, y, 0.22)
    if edge > 0:
        return (0, 0, 0), 0.0
    col = mix(SKY_TOP, SKY_BOTTOM, y)
    lit = circle(x, y, MOON)
    bite = circle(x, y, BITE)
    crescent = max(lit, -bite)
    # Halo around the crescent itself; a faint, soft-edged earthshine fills
    # the bite so the dark side reads as the rest of the moon.
    col = over(col, GLOW, math.exp(-max(crescent, 0.0) * 8.0) * 0.5)
    col = over(col, GLOW, min(1.0, max(0.0, -lit / 0.06)) * 0.10)
    if size >= 32:
        for s in STARS:
            if s[2] * size >= 1.2:
                col = over(col, STAR, sparkle(x, y, s) * 0.95)
    if crescent < 0:
        depth = min(1.0, -crescent / 0.09)
        body = mix(MOON_EDGE, MOON_LIT, depth)
        if size >= 48:
            for c in CRATERS:
                d = circle(x, y, c)
                if d < 0:
                    body = mix(body, CRATER, min(1.0, -d / 0.012) * 0.55)
        col = body
    return col, 1.0


def render(size):
    n = 4
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            r = g = b = a = 0.0
            for sy in range(n):
                for sx in range(n):
                    x = (px + (sx + 0.5) / n) / size
                    y = (py + (sy + 0.5) / n) / size
                    c, al = shade(x, y, size)
                    r += c[0] * al; g += c[1] * al; b += c[2] * al; a += al
            if a:
                row += bytes((round(r / a), round(g / a), round(b / a), round(255 * a / (n * n))))
            else:
                row += b"\0\0\0\0"
        rows.append(bytes(row))
    return rows


def png(size, rows):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    raw = b"".join(b"\0" + r for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    images = [(s, png(s, render(s))) for s in SIZES]
    head = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b"", b""
    for s, data in images:
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        blobs += data
    with open(sys.argv[1], "wb") as f:
        f.write(head + entries + blobs)
    if len(sys.argv) > 2:
        with open(sys.argv[2], "wb") as f:
            f.write(images[-1][1])


if __name__ == "__main__":
    main()
