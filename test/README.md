# Test data

`dmg-acid2.gb` is Matt Currie's DMG rendering test ROM, taken unmodified from
https://github.com/mattcurrie/dmg-acid2 (MIT, Copyright (c) 2020 Matt Currie).

`dmg-acid2-reference.ppm` is the reference image from the same repository
(`img/reference-dmg.png`, MIT), converted from its 2-bit greyscale PNG to a
plain PPM so the check that uses it needs no image library: 160x144, four
levels (0, 85, 170, 255), one per DMG shade.

`tools/check-render.py` compares the emulator's output against it. The
comparison is done on *shade indices*, not colours, so it does not care which
DMG palette is selected: the four colours found in the render are sorted by
luminance and matched against the four levels of the reference. What it checks
is that the right pixels are the right shade of the right layer - which is what
`dmg-acid2` exists to test.

## Running it

From the repository root:

    make -C tools/host acid2

`acid2` renders 300 frames at `SCALE_ORIG`, which is the only scale that puts
the frame 1:1 on the 400x240 top screen (x 120..279, y 48..191), so the render
can be compared with the 160x144 reference pixel for pixel; then it runs the
comparison. The harness also fails the run if any frame started drawing into
the buffer the panel was still showing - that is a torn frame, which a single
PPM cannot show.

CI runs it on every push.

