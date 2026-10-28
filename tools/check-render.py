#!/usr/bin/env python3
"""Compare a rendered frame against a reference image, by shade index.

    python3 tools/check-render.py --render build/acid2.ppm \
        --reference test/dmg-acid2-reference.ppm --window 120,48,160,144

Both files are binary PPM (P6). The window picks the Game Boy picture out of
the 400x240 top screen.

Colours are compared by *rank*, not by value: the distinct colours of each
image are sorted by luminance and replaced by their index, so the check works
whatever DMG palette the emulator was set to, and a palette change alone can
never fail it. A mismatch means a pixel has the wrong layer or the wrong
shade - a rendering bug - not a different green.

Exit status: 0 when nothing differs, 1 otherwise, 2 on bad input.
"""
import sys

MAX_SHADES = 4  # a DMG frame has four


def die(msg):
    sys.stderr.write("check-render: %s\n" % msg)
    sys.exit(2)


def read_ppm(path):
    try:
        data = open(path, 'rb').read()
    except OSError as e:
        die("cannot read %s: %s" % (path, e))
    if not data.startswith(b'P6'):
        die("%s is not a binary PPM" % path)
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if pos < len(data) and data[pos:pos + 1] == b'#':
            while pos < len(data) and data[pos:pos + 1] not in (b'\n', b'\r'):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1  # single whitespace after maxval
    w, h, _ = fields
    pixels = data[pos:]
    if len(pixels) < w * h * 3:
        die("%s is truncated (%d bytes for %dx%d)" % (path, len(pixels), w, h))
    return w, h, pixels


def luma(rgb):
    r, g, b = rgb
    return 299 * r + 587 * g + 114 * b


def shades(w, h, pixels, window):
    x0, y0, ww, wh = window
    if x0 + ww > w or y0 + wh > h:
        die("window %s does not fit in a %dx%d image" % (window, w, h))
    seen = set()
    for y in range(y0, y0 + wh):
        row = y * w
        for x in range(x0, x0 + ww):
            o = (row + x) * 3
            seen.add(pixels[o:o + 3])
    ordered = sorted(seen, key=luma)
    if len(ordered) > MAX_SHADES:
        sys.stderr.write(
            "check-render: warning: %d distinct colours, a DMG frame has at "
            "most %d\n" % (len(ordered), MAX_SHADES))
    rank = {c: i for i, c in enumerate(ordered)}
    out = bytearray(ww * wh)
    for y in range(wh):
        row = (y0 + y) * w
        for x in range(ww):
            o = (row + x0 + x) * 3
            out[y * ww + x] = rank[pixels[o:o + 3]]
    return out, [c.hex() for c in ordered]


def parse_window(text):
    try:
        parts = [int(v) for v in text.split(',')]
    except ValueError:
        die("--window wants x,y,w,h")
    if len(parts) != 4:
        die("--window wants x,y,w,h")
    return tuple(parts)


def main(argv):
    args = {"--render": None, "--reference": None, "--window": None}
    i = 1
    while i < len(argv):
        key = argv[i]
        if key not in args or i + 1 >= len(argv):
            die("usage: check-render.py --render a.ppm --reference b.ppm "
                "--window x,y,w,h")
        args[key] = argv[i + 1]
        i += 2
    if not all(args.values()):
        die("usage: check-render.py --render a.ppm --reference b.ppm "
            "--window x,y,w,h")

    window = parse_window(args["--window"])
    rw, rh, rp = read_ppm(args["--render"])
    fw, fh, fp = read_ppm(args["--reference"])

    ref_w, ref_h = window[2], window[3]
    if (fw, fh) != (ref_w, ref_h):
        die("reference is %dx%d, window is %dx%d" % (fw, fh, ref_w, ref_h))

    got, got_colours = shades(rw, rh, rp, window)
    want, want_colours = shades(fw, fh, fp, (0, 0, fw, fh))

    if len(got_colours) != len(want_colours):
        sys.stderr.write(
            "check-render: note: %d shades in the render, %d in the reference\n"
            % (len(got_colours), len(want_colours)))
        sys.stderr.write("              render:    %s\n" % ", ".join(got_colours))
        sys.stderr.write("              reference: %s\n" % ", ".join(want_colours))

    bad = [i for i in range(len(got)) if got[i] != want[i]]
    total = len(got)
    if not bad:
        print("%s: %d of %d pixels differ (%d shades, palette-independent)"
              % (args["--render"], 0, total, len(got_colours)))
        return 0

    # Where the differences are, so a failure is actionable in a CI log.
    xs = [i % ref_w for i in bad]
    ys = [i // ref_w for i in bad]
    print("%s: %d of %d pixels differ - first at (%d, %d), "
          "bounding box x=%d..%d y=%d..%d"
          % (args["--render"], len(bad), total, xs[0], ys[0],
             min(xs), max(xs), min(ys), max(ys)))
    for i in bad[:10]:
        x, y = i % ref_w, i // ref_w
        print("  (%3d,%3d) shade %d, expected %d" % (x, y, got[i], want[i]))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
