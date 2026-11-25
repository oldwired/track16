# Track16

An interactive tracker-module player for the Pioneer DJ TORAIZ SP-16. It plays
MOD, S3M, XM, IT and other module formats from a USB stick and turns them into
something to perform with: pad modes for muting, looping, playing the module's
instruments and sending channels through the SP-16's analog filter, MIDI clock
and notes on the MIDI out, and the pattern data as "digital rain" on the
display.

Status: early development. The app is started from the community
[boot menu](https://github.com/freesp16/bootmenu); nothing is installed on the
unit and nothing is written to its internal memory.

## Based on juce-app-template

Track16 starts from freesp16's
[juce-app-template](https://github.com/freesp16/juce-app-template), which
provides the SP-16 hardware layer (`hw/`), the cross toolchain and the build.
The template's own documentation is in
[docs/template-README.md](docs/template-README.md).

The template is kept as the git remote `template`. To merge its later fixes:

```bash
git pull template main
```

Keep changes to `hw/` small so these merges stay easy.

## Build

```bash
git submodule update --init --depth 1   # JUCE 9.0.6
scripts/build-unit.sh                   # → out/track16 for the USB stick
```

Copy `out/track16` next to the boot menu's `autoexec.bin` on a FAT32 stick.
See the template documentation for settings and the stick layout.

## Licence

Track16 is GPL-3.0 (see `LICENSE`). JUCE is used under its free licence,
AGPLv3. libopenmpt (to be added) is BSD-3-Clause. Distributed binaries come
with their complete source.
