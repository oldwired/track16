# Track16

An interactive tracker-module player for the Pioneer DJ TORAIZ SP-16. It plays MOD, S3M, XM, IT and the other
formats [libopenmpt](https://lib.openmpt.org) supports from a USB stick and turns them into something to perform
with: pad modes for muting, looping, playing the module's instruments and sending channels through the SP-16's
analog filter, MIDI clock and notes on the MIDI outs, and the pattern data as "digital rain" on the display.

Status: first implementation (0.1.0). The engine is tested on a build machine (`tests/`); the unit test of the whole
app is pending. The app is started from the community [boot menu](https://github.com/freesp16/bootmenu); nothing is
installed on the unit and nothing is written to its internal memory.

## The stick

```
autoexec.bin        the boot menu (its release)
track16             this app (out/track16)
track16.conf        optional settings (below)
modules/…           your modules, in any folders (all folders of the stick are searched)
```

Plug the stick into the running SP-16, choose **Track16** in the boot menu. **SHIFT + BACK** quits to the menu.
Quit before pulling the stick: the menu and the app write their logs to it.

## Screens

- **LIBRARY**: every module on the stick. Turn SELECT to choose, push SELECT to play.
- **PLAYER**: the pattern rain. The rows fall onto the playhead line, flare in their instrument's colour as they
  cross it, and fade into a trail. Column headers show each channel's level; muted channels are grey, filtered ones
  orange. The context bar shows order, row, speed, BPM, the title and the pad modes.

BACK switches between them (HOME: the library).

## Controls

An interactive map of the panel with every control: [docs/panel.html](docs/panel.html).

| Control | Action |
|---|---|
| PLAY | Play / pause (MIDI Start/Continue/Stop follow) |
| STOP | Stop and rewind. SHIFT + STOP also clears mutes, solos and the master filter |
| SELECT turn / push | Library: choose / play. Player: the instrument for the PLAY pads / back to the library |
| SCENE / PATTERN | Previous / next module. With SHIFT: the PLAY pads' octave |
| STEP 1–16 | Jump to order 1–16 of the current order page; the jump is quantized (BAR keys). SHIFT + STEP n: order page n |
| BAR 1 / 2 / 3 | Jumps at the pattern end / on the beat / on the next row |
| BAR 4 | Pattern loop on/off |
| REC | MIDI notes out on/off (module channels 1–16 → MIDI channels 1–16) |
| ANALOG FILTER ACTIVE | The whole mix through the analog filter (the board in the main output's path) |
| Knobs 1–6 | TEMPO (SHIFT: 100 %), PITCH (SHIFT: 0), STEREO separation, INTERPolation, GAIN (SHIFT: 0 dB), VIEW (rain density) |
| MAIN VOLUME | Output level |
| Touch strip | Acts by its mode while touched (below); HOLD keeps the last value after you let go |
| Strip MODE | PITCH: pitch bend ±12 semitones · REPEAT: stutter loop, 1 row at the top … 16 rows at the bottom, returns in time · USER1: tempo bend ±8 % · USER2: volume (top full, bottom silent) |
| DRIVE | 0…+12 dB into the analog filter (the stock firmware's curve); OVER lights near the send's full scale |
| SHIFT + BACK | Quit to the boot menu |

### Pad modes (the four keys below the pads)

| Key | Mode | Pad | SHIFT + pad |
|---|---|---|---|
| TRACK | **FILTER** | Hold: that channel through the analog filter | Keep it there (latch) |
| MUTE | **MUTE** | Mute on/off | Solo on/off |
| SLICE | **LOOP** | Hold: loop 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 rows (pads 1–12); on release the song continues where it would be, in time | Keep the loop running |
| SCALE | **PLAY** | Play the selected instrument chromatically (pad 1 = C-5) | Select instrument |
| SHIFT + SCALE | **KIT** | Pads 1–16 play instruments 1–16 at their standard pitch | — |

Pressing the active mode's key again shows the next 16 channels / instruments on the pads.

The analog filter's LP cutoff and resonance are analog knobs on the unit: hold a FILTER pad with one hand and turn
them with the other. HP cutoff is read by the panel and sets the board's high-pass.

The filter board has two ways in, chosen by its select line. **ANALOG FILTER ACTIVE** puts the board in the main
output's path: the whole mix goes through the filter on its way to MAIN OUT, with no latency (PHONES stays
unfiltered: it has its own DAC). **FILTER pads** use the send/return loop instead: the held channels go out on the
filter send and come back on the return, the rest of the mix stays clean. DRIVE is a digital gain (as in the stock
firmware) on what goes into the filter, the main mix with ACTIVE or the send with FILTER pads; the saturation is the
analog board driven hot. OVER lights when that signal peaks above 90 % of its DAC's full scale (with ACTIVE, the main
DAC has no headroom: at high drive the mix saturates digitally before the board).

Track16 measures the filter loop's latency at start (ANALOG FILTER ACTIVE lights up for a moment; nothing plays until
it is done) and delays the unfiltered channels by it, so both paths stay aligned. The result is shown and logged
(`FILTER LOOP latency …`); "no return" means the click did not come back (LP cutoff closed?) and a guess is used.
After the loop drops periods it is measured again as soon as no channel goes through the filter. While the return
brings no audio at all (no clock, a codec fault) the loop counts as absent and everything plays unfiltered, so a
dead loop never silences the output. The STAT lines in `bootmenu.log` show the loop's state (`loop_live`,
`loop_frames`, `loop_drops`) and its levels (`send_peak`, `return_peak`).

Loops, the pattern loop and jumps take effect at a row boundary, so the song never stutters off the grid.

## MIDI

- **Clock**: 24 pulses per beat from the module's actual row timing (`rows_per_beat`, default 4), Start / Stop /
  Continue, and Song Position Pointer after jumps and pattern-loop repeats. The stutter loop sends neither: it returns
  in time, where a follower counting the clock already is. Sent to the ports `midi_out` selects.
- **Notes** (REC, or `midi_notes=1`): the notes of module channels 1–16 on MIDI channels 1–16, velocity from the
  volume column. Muting a channel ends its MIDI note, REC off ends them all; quitting sends Stop and All Notes Off.
- **In**: notes play the selected instrument.

All MIDI out is timed against the audio output (`midi_offset_ms` adjusts it per setup).

## Settings (`track16.conf` on the stick)

```
modules=/media/usb1/sda1/modules   where to look (default: the whole stick)
autoplay=0           1: play the first module at start
audio_period=128     audio period in frames (latency); 256 if you hear dropouts
stereo=100           stereo separation, 0…200 %
interp=4             interpolation taps: 1 none, 2 linear, 4 cubic, 8 sinc
rows_per_beat=4      for the MIDI clock and beat-quantized jumps
strip_pitch=12       the PITCH strip's range in semitones each way
midi_out=all         all, din, usb or none
midi_clock=1
midi_notes=0
midi_offset_ms=0
filter_loop=1        the analog filter loop (0: off)
filter_latency=0     fixed loop latency in frames (0: measure at start)
filter_send_gain=0.5 the filter send's level (−6 dB: headroom for the drive); the return is raised by 1/gain
stat=10              seconds between STAT lines in bootmenu.log
```

## Build

```bash
git submodule update --init --depth 1   # JUCE 9.0.6
scripts/build-unit.sh                   # → out/track16 for the stick
scripts/test-host.sh ../modules         # engine tests with a folder of modules
```

`scripts/build-unit.sh` needs Docker (Colima works); the first run builds the cross toolchain (about 30 minutes).

## Code

| Path | What it is |
|---|---|
| `app/Player.*` | The engine: libopenmpt on the audio thread, pad modes, quantized jumps, the stutter loop, the filter routes, MIDI clock and notes |
| `app/Song.*` | Loading a module: the libopenmpt instances (two with the filter loop: "dry" and "wet" stems) and a snapshot of the pattern data |
| `app/Screen.*` | The UI: library, player, context bar, knob strip, the rain, the controls and the lamps |
| `app/GlDisplay.*` | OpenGL ES output: the rain's rectangles and glyphs (an atlas built from the app font) under the UI |
| `app/Midi.*` | MIDI in and the timed MIDI out |
| `app/Library.*` | The module files on the stick |
| `hw/` | The SP-16 hardware layer from juce-app-template (keep changes small) |
| `third_party/libopenmpt` | libopenmpt 0.8.10 with a small position extension (see `VENDORED.md`) |
| `tests/` | Host tests of the engine |

## Based on juce-app-template

Track16 starts from freesp16's [juce-app-template](https://github.com/freesp16/juce-app-template), kept as the git
remote `template` (`git pull template main` merges its fixes). Its documentation is in
[docs/template-README.md](docs/template-README.md).

## Licence

GPL-3.0 (see `LICENSE`). JUCE is used under its free licence, AGPLv3. libopenmpt is BSD-3-Clause. Distributed
binaries come with their complete source.
