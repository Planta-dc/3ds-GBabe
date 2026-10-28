# Test ROM

`dmg-acid2.gb` is Matt Currie's *dmg-acid2* PPU test ROM (MIT), the same binary
that Walnut-CGB ships in its own test suite. It draws a face; if sprites,
priority, window, palettes or object sizes are wrong, the face is visibly wrong.

```sh
make -C tools/host
tools/host/gbabe-host test/dmg-acid2.gb 300 /tmp/acid2.ppm 0
```

`docs/screenshot-acid2.png` was produced this way.
