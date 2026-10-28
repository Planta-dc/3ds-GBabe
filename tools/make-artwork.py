"""GBabe build artwork generator.

    python3 tools/make-artwork.py

Writes, from code (no external tools, no binary blobs in git):

    icon.png                48x48 RGBA, SMDH icon used by the 3DSX and the CIA
    resources/banner.png    256x128 RGBA, CIA banner (bannertool makebanner)
    resources/banner.wav    short silent clip, required by makebanner

Both are plain 8-bit RGBA PNGs written by hand, so the repository needs no
image tooling to reproduce them.
"""
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------
# 5x7 bitmap font, enough for the strings used below
# ---------------------------------------------------------------------------
FONT = {
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "B": ("11110", "10001", "10001", "11110", "10001", "10001", "11110"),
    "C": ("01111", "10000", "10000", "10000", "10000", "10000", "01111"),
    "D": ("11110", "10001", "10001", "10001", "10001", "10001", "11110"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "G": ("01111", "10000", "10000", "10111", "10001", "10001", "01111"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "M": ("10001", "11011", "10101", "10101", "10001", "10001", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "Y": ("10001", "10001", "01010", "00100", "00100", "00100", "00100"),
    "3": ("11110", "00001", "00001", "01110", "00001", "00001", "11110"),
    "&": ("01100", "10010", "10100", "01000", "10101", "10010", "01101"),
    "/": ("00001", "00010", "00010", "00100", "01000", "01000", "10000"),
    " ": ("00000", "00000", "00000", "00000", "00000", "00000", "00000"),
}

# ---------------------------------------------------------------------------
# palette (matches source/palette.c "GB Green" and the icon)
# ---------------------------------------------------------------------------
BODY = (43, 43, 43, 255)
BODY_HI = (62, 62, 62, 255)
BEZEL = (22, 22, 22, 255)
SCREEN = (155, 188, 15, 255)
DARK = (15, 56, 15, 255)
BTN = (203, 80, 140, 255)
BTN2 = (150, 50, 100, 255)
TEXT = (235, 235, 235, 255)
ACCENT = (155, 188, 15, 255)


class Canvas:
    def __init__(self, w, h, bg=(0, 0, 0, 0)):
        self.w = w
        self.h = h
        self.px = [[bg for _ in range(w)] for _ in range(h)]

    def put(self, x, y, c):
        x, y = int(x), int(y)
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = c

    def rect(self, x0, y0, x1, y1, c):
        for y in range(int(y0), int(y1)):
            for x in range(int(x0), int(x1)):
                self.put(x, y, c)

    def rrect(self, x0, y0, x1, y1, r, c):
        for y in range(int(y0), int(y1)):
            for x in range(int(x0), int(x1)):
                dx = min(x - x0, x1 - 1 - x)
                dy = min(y - y0, y1 - 1 - y)
                if dx < r and dy < r:
                    cx, cy = r - dx, r - dy
                    if cx * cx + cy * cy > r * r + 2:
                        continue
                self.put(x, y, c)

    def disc(self, cx, cy, r, c):
        for y in range(int(cy - r), int(cy + r) + 1):
            for x in range(int(cx - r), int(cx + r) + 1):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    self.put(x, y, c)

    def text(self, x, y, s, scale, c):
        for ch in s:
            glyph = FONT.get(ch.upper())
            if glyph is None:
                x += 6 * scale
                continue
            for row, bits in enumerate(glyph):
                for col, bit in enumerate(bits):
                    if bit == "1":
                        self.rect(x + col * scale, y + row * scale,
                                  x + (col + 1) * scale, y + (row + 1) * scale, c)
            x += 6 * scale

    def text_width(self, s, scale):
        return len(s) * 6 * scale - scale

    def png(self, path):
        raw = b"".join(b"\x00" + b"".join(bytes(self.px[y][x]) for x in range(self.w))
                       for y in range(self.h))

        def chunk(tag, data):
            return (struct.pack(">I", len(data)) + tag + data
                    + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

        png = (b"\x89PNG\r\n\x1a\n"
               + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 6, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress(raw, 9))
               + chunk(b"IEND", b""))
        with open(path, "wb") as f:
            f.write(png)
        print("%-28s %3dx%-3d %5d bytes" % (os.path.relpath(path, ROOT), self.w, self.h, len(png)))


def draw_gameboy(c, x0, y0, s):
    """A Game Boy at (x0, y0), s pixels per unit; the unit grid is 44x44."""
    c.rrect(x0, y0, x0 + 44 * s, y0 + 44 * s, 6 * s, BODY)

    # screen bezel + screen
    c.rect(x0 + 7 * s, y0 + 5 * s, x0 + 37 * s, y0 + 23 * s, BEZEL)
    c.rect(x0 + 9 * s, y0 + 7 * s, x0 + 35 * s, y0 + 21 * s, SCREEN)

    # mascot: two eyes and a smile
    c.rect(x0 + 15 * s, y0 + 11 * s, x0 + 17 * s, y0 + 13 * s, DARK)
    c.rect(x0 + 27 * s, y0 + 11 * s, x0 + 29 * s, y0 + 13 * s, DARK)
    for i in range(5):
        c.rect(x0 + (18 + i) * s, y0 + (17 + (i in (0, 4))) * s,
               x0 + (19 + i) * s, y0 + (18 + (i in (0, 4))) * s, DARK)

    # d-pad
    c.rect(x0 + 7 * s, y0 + 29 * s, x0 + 15 * s, y0 + 32 * s, BEZEL)
    c.rect(x0 + 10 * s, y0 + 26 * s, x0 + 13 * s, y0 + 35 * s, BEZEL)

    # A / B
    c.disc(x0 + 30 * s, y0 + 34 * s, 3 * s, BTN)
    c.disc(x0 + 36 * s, y0 + 30 * s, 3 * s, BTN2)


def make_icon():
    c = Canvas(48, 48)
    draw_gameboy(c, 2, 2, 1)
    c.png(os.path.join(ROOT, "icon.png"))


def make_banner():
    c = Canvas(256, 128)
    # vertical gradient background
    for y in range(128):
        t = y / 127.0
        c.rect(0, y, 256, y + 1,
               (int(18 + 12 * (1 - t)), int(18 + 12 * (1 - t)), int(22 + 14 * (1 - t)), 255))

    draw_gameboy(c, 16, 19, 2)

    x = 124
    c.text(x, 30, "GBABE", 4, TEXT)
    c.rect(x, 30 + 7 * 4 + 6, 256 - 16, 30 + 7 * 4 + 9, ACCENT)
    c.text(x, 76, "GB & GBC", 2, ACCENT)
    c.text(x, 96, "GAME BOY", 1, (150, 150, 150, 255))
    c.text(x, 108, "FOR 3DS", 1, (150, 150, 150, 255))

    c.png(os.path.join(ROOT, "resources", "banner.png"))


def make_banner_audio():
    """bannertool makebanner wants an audio file next to the image; a little
    silence is all this banner needs."""
    rate = 22050
    frames = rate // 2  # half a second
    data = b"\x00\x00" * frames
    hdr = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
           + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
           + b"data" + struct.pack("<I", len(data)))
    path = os.path.join(ROOT, "resources", "banner.wav")
    with open(path, "wb") as f:
        f.write(hdr + data)
    print("%-28s %s %5d bytes" % (os.path.relpath(path, ROOT), "22050Hz mono", len(hdr + data)))


if __name__ == "__main__":
    os.makedirs(os.path.join(ROOT, "resources"), exist_ok=True)
    make_icon()
    make_banner()
    make_banner_audio()
