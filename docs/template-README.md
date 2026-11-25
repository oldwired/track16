# JUCE app template for the SP-16

A small, complete starting point for your own software on the Pioneer DJ TORAIZ SP-16: a **JUCE 9** app that the
community [boot menu](https://github.com/freesp16/bootmenu) starts from a USB stick. It drives all of the unit's
hardware:

- the 16 pads (velocity and pressure), every key, the encoders, the knobs and the touch strip
- the LCD touch screen and every LED
- all the audio outputs, DIN and USB MIDI
- the GPU (OpenGL ES 2)

It comes with a demo in [freesp16](https://github.com/freesp16)'s design that you can replace.

![The demo screen](docs/screenshot.png)

The demo:

- **Screen:** an ordinary `juce::Component` tree in freesp16's look. JUCE paints it, and the GPU puts it on the
  screen with a live oscilloscope drawn in OpenGL ES.
- **Pads:** a 16-voice synth (pressure gives vibrato, the strip bends the pitch). Each hit flashes its tile and LED and
  is sent as a MIDI note.
- **Audio test:** a sine tone you can send to any jack (MAIN, OUT 3-4, 5-6, 7-8, PHONES), left, right or both, with a
  meter for every output.
- **MIDI:** notes and pitch bend from DIN and USB play the synth. The status row shows each message that arrives.
- **Sequencer:** 16 steps on the step keys, PLAY to start it.

The same code runs in a window on a Linux desktop, with the keyboard and mouse standing in for the panel, so you can
work on your app without the unit.

## Run it on the SP-16

You need a FAT32 USB stick.

1. Download `autoexec.bin` from the [boot menu's latest release](https://github.com/freesp16/bootmenu/releases/latest)
   and copy it to the root of the stick.
2. Download `app-template` from this project's [latest release](../../releases/latest) (or build it, below) and copy it
   to the root of the stick, next to `autoexec.bin`.
3. Plug the stick into the SP-16 while it is on. The boot menu appears and lists **App Template** with its icon. Turn
   SELECT to choose it and push SELECT to start it.

**SHIFT + BACK**, or ↩ at the top left of the screen, quits back to the menu. Pulling the stick brings the official
software back. Nothing is installed on the unit and nothing is written to its internal memory.

What the boot menu does with the app:

| | |
|---|---|
| `<stick>/app-template` | the program. The menu reads its name, version and icon from the file itself. You can also put it in `<stick>/apps/` |
| `<stick>/app-template.conf` | optional settings, one `key=value` per line (below) |
| `<stick>/app-template-data/` | the app's data folder (`/root/data` while it runs) |
| `<stick>/bootmenu.log` | the menu's log, which includes everything the app prints (start-up, audio, inputs, `STAT` lines, crash reports) |

Settings (all optional):

| Key | |
|---|---|
| `tone=1`, `tone_port=out34` | start with the test tone on that jack (`main`, `out34`, `out56`, `out78`, `phones`): a jack check without touching anything |
| `seq=1` | start with the sequencer running |
| `audio_period=128` | the audio period in frames (default 256 at 44.1 kHz) |
| `midi=0`, `audio=0` | no MIDI ports / no audio |
| `stat=10` | seconds between the `STAT` log lines (frame rate, DSP load, xruns); 0 = none |

## Controls

| On the unit | In the demo | On a desktop |
|---|---|---|
| Pads 1–16 | play (pad 1 bottom left) | `z x c v` / `a s d f` / `q w e r` / `1 2 3 4` |
| Knobs 1–6 | volume, waveform, attack, release, cutoff, tone pitch | ← → pick a knob, ↑ ↓ turn it |
| SELECT turn / push | test-tone jack / tone on-off | `,` `.` / Return |
| SHIFT + SELECT | tone on left + right, left only, right only | Shift + `,` `.` |
| PLAY / STOP | sequencer / everything off | Space / `b` |
| Step keys | the sequencer's 16 steps | `5 … =`, `t … ]` |
| BAR 1–4 | tempo 90, 120, 140, 174 | F1–F4 |
| Pad-mode keys | pad scale: pentatonic, major, minor, chromatic | F5–F8 |
| SCENE / PATTERN | octave down / up | F9 / F10 |
| Touch strip | pitch bend | Ctrl + mouse wheel |
| Touch screen | the tiles play; tap the top bar's items | the mouse |
| SHIFT + BACK | quit | Shift + Esc, or close the window |

## Build

You need CMake ≥ 3.25, Ninja, Python 3 and a C++20 compiler. Building for the unit also needs Docker.

```bash
git clone --recursive https://github.com/freesp16/juce-app-template.git
cd juce-app-template
```

The Linux desktop (packages: `libasound2-dev libx11-dev libegl-dev libgles-dev libfreetype-dev libfontconfig1-dev`):

```bash
cmake --preset linux && cmake --build --preset linux
build/linux/bin/app-template
```

The unit. The first run builds the cross-compiler image, which takes about 30 minutes.

```bash
scripts/build-unit.sh
```

This produces `out/app-template`, ready to copy to the stick.

## Make it your app

1. Set your app's name, display name, id and version at the top of [`CMakeLists.txt`](CMakeLists.txt), and replace
   [`resources/app-icon.png`](resources/app-icon.png) with a square PNG whose side is a multiple of 128. The executable
   takes the new name, and the boot menu shows the new name and icon.
2. Edit the demo in `app/`:

| File | What it is |
|---|---|
| [`app/Main.cpp`](app/Main.cpp) | starts everything, then one loop per screen frame: input → screen → lamps → display |
| [`app/Screen.*`](app/Screen.cpp) | the UI: a `juce::Component`. `handle()` gets every control event, `lamps()` sets the LEDs |
| [`app/Synth.*`](app/Synth.cpp) | the audio: `process()` runs once per audio period on the real-time thread |
| [`app/Midi.*`](app/Midi.cpp) | MIDI ports: reader threads in, `send()` out |
| [`app/Theme.*`](app/Theme.cpp) | freesp16's colours, Inter fonts, layout grid and drawing helpers |
| [`app/GlDisplay.*`](app/GlDisplay.cpp) | OpenGL ES output: the UI as a texture, plus your own GL drawing (the scope) |
| [`app/Log.*`](app/Log.cpp) | log lines and crash reports |
| [`hw/`](hw) | the hardware layer: [`hw/Platform.h`](hw/Platform.h) is all your app needs to know about it |

`hw/` holds the unit's drivers in user space (`hw/imx6`: the SPI panel and strip, the AK4458 codec, the GPU), the
desktop stand-in (`hw/linux`), and shared code (`hw/common`: ALSA audio and MIDI, X11, the GL loader;
`hw/controls`: the control and LED vocabulary). Every input arrives as an `io::ControlEvent`, whichever device it
came from. Every LED is an `io::LampState` in an `io::LampFrame`.

## How it works

- **Threads:** the main thread runs the loop and JUCE. The audio thread (real-time priority) only calls
  `Synth::process()`. The panel, strip and MIDI each have a reader thread that sends pad hits, strip moves and notes
  straight to the synth through a lock-free queue, so a pad sounds one panel frame after it is hit, without waiting
  for the screen.
- **Screen:** the UI is drawn on freesp16's 1280 × 768 grid and scaled to the 800 × 480 LCD. JUCE's software renderer
  repaints only what called `repaint()`. The changed rows go to a texture, which GL draws over its own content,
  paced by the display at 60 Hz. JUCE never opens a window. Its X11 windowing crashes on the unit, so keep
  `LookAndFeel::setDefaultLookAndFeel` and `addToDesktop` out of your code; [`app/Theme.h`](app/Theme.h) explains.
- **GPU driver:** the unit ships its driver in variants for the framebuffer and for X11, and they must not be mixed in
  one process. The app therefore links no GL library. Every GL call goes through a stub table (generated at build
  time by [`tools/gen-gl-dispatch.py`](tools/gen-gl-dispatch.py)) that is bound to the framebuffer driver at start-up.
  On a desktop it is bound to Mesa.
- **Performance:** the unit has four Cortex-A9 cores at 1 GHz. Keep `process()` free of allocation, locks and I/O, and
  repaint small areas. The `STAT` lines in the log show the frame rate, DSP load and xruns.

## Licence

The template's code is free software under the **GNU General Public License v3.0 or later** ([LICENSE](LICENSE)).

It is built on [JUCE](https://juce.com) 9, included as a git submodule, which is licensed under the
**AGPLv3** or a commercial JUCE licence. If you distribute an app built from this template under JUCE's free licence,
you must publish its complete source code under the AGPLv3 (the GPLv3 code here may be combined with it). The Inter
font is under the [SIL Open Font License 1.1](resources/fonts/INTER-LICENSE.txt).

This is an independent community project. It is not affiliated with or endorsed by Pioneer DJ, AlphaTheta or Raw
Material Software. TORAIZ and SP-16 are trademarks of their owners and appear here only to name the hardware this runs
on. JUCE is a trademark of Raw Material Software Limited. No firmware is included.
