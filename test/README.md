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

    make -C tools/host acid2                 # Gambatte
    make -C tools/host acid2 CORE=walnut     # Walnut-CGB

`acid2` renders 300 frames at `SCALE_ORIG`, which is the only scale that puts
the frame 1:1 on the 400x240 top screen (x 120..279, y 48..191), so the render
can be compared with the 160x144 reference pixel for pixel; then it runs the
comparison. It relinks `tools/host/gbabe-host` when the binary was built for
the other core, so the two invocations can be made in either order.

CI runs both.

`make -C tools/host cross` goes one step further: it renders `dmg-acid2` with
both cores at all three scales and compares the two PPMs byte for byte, so the
cores have to agree with each other as well as with the reference.
