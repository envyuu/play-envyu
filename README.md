# play-envyu

Unofficial, community-made extensions for the Polyend Play. This repository contains no Polyend firmware: you download the official firmware yourself, and a small script applies the mod to it on your computer.

> **Beta, at your own risk.** Flashing modified firmware can make your device unusable and may affect your warranty. So far it has been tested on one single Rev A device, and testing of this build is still in progress. It is not affiliated with or endorsed by Polyend. "Polyend" and "Play" are used only to say which hardware this is for.

**Website with interactive demos and a browser patcher: https://envyuu.github.io/play-envyu/**

## What it adds

**New in 0.2**

- **Beat Slicer:** set a sample track to *Slicer* and its sample is cut into 4 to 32 slices, on an even grid or at the detected beats. The pads play the slices, steps choose them. Grid width, offset and beat sensitivity are adjustable, single slice starts can be moved by hand (hold a slice pad and turn *Sample Start*), and each step can shift its slice start a little (hold the step and turn *Sample Start*).
- **Waveform view** for sample tracks: start, end, attack and decay are drawn over the sample while you turn the knobs, also while browsing samples.
- **Tap tempo** in the menu: touch the tempo knob in rhythm; the tempo is set from the sixth tap on and stays steady.
- **Recording of incoming USB audio** with *Wait For Signal* and *Pause On Silence*, a REC display with level meter, and a clear message when the USB mode does not allow recording.
- **Randomize:** Swap Steps, Scatter Steps, Thin Out (with a visible preview on the pads), Melodic Walk, Accent, Call & Response, Humanize, Octave Jump, Note Length, for MIDI and sample tracks.
- **Perform:** the track selection survives switching between Perform and the pattern view (menu *Keep Selection*), and the mute pads flash on every step in the colour of the track's engine.
- **Color Mode** for red-green and blue-yellow colour blindness.
- **Audio In** page: delay and reverb send for the USB input.
- **Fix: USB audio on Windows.** 0.1 was not detected as an audio device on Windows. 0.2 is now detected with the built-in Windows driver (tested on Windows 11, stereo input). Please report whether audio comes through on your Windows machine.
- **Safety net:** the mod checks itself at every start and falls back to stock behaviour if its code is damaged (see below).

**From 0.1**

- **MIDI effects** including arpeggiator and echo, per selected track
- **USB audio:** stereo and multitrack output, and input from the computer
- **Randomize:** Note In Scale
- **USB file transfer** and firmware upload from a computer

## Supported devices

| Device | Status |
|---|---|
| Play, Rev A (Kinetis K66), firmware 1.6.0 | Target device, tested on one device, testing in progress |
| Play, Rev B (i.MX RT) | The Rev B part of the file stays stock, untested |
| Play+ | Not supported |

## Install

You need Python 3 and the official firmware 1.6.0 from Polyend's downloads page.

```
python3 patcher/play_patch.py PolyendPlay_v1.6.0.ppf -o PolyendPlay_v1.6.0_mod.ppf
```

The script refuses any input other than the exact stock file, and it only writes a result that matches the tested build byte for byte.

1. Back up your whole SD card.
2. Copy the new `.ppf` into the card's `Firmware` folder. Keep few files there: a device with 31 `.ppf` files stopped at "Preparing".
3. Install it from the device's firmware update menu.
4. Switch the device off and on once to confirm a cold start works.

## Safety net

At every start, a small block at the beginning of the mod checks a CRC-32 over the mod's whole code. If the check fails, for example because a firmware update stopped halfway, every patched place in the stock code jumps back to the stock routine instead of into the mod, and the effect names and the USB descriptor get their stock contents. The device then behaves like stock firmware, and you can install a working file from the update menu. Only if that first block itself is damaged does the device not start; then use the emergency flash described below.

## Going back

Put only the original `PolyendPlay_v1.6.0.ppf` in `/Firmware` and install it from the update menu. If the device hangs, a short press of the reset button while it runs started the emergency flash on our test device. Keep a copy of the original firmware on your computer.

## Building from source

You need `arm-none-eabi-gcc`, Python 3 and your own stock firmware at `stock/PolyendPlay_v1.6.0.ppf`.

```
python3 tools/hex2bin.py stock/PolyendPlay_v1.6.0.ppf stock/firmware.bin
pip install unicorn numpy pillow     # for the emulator tests, ideally in a venv
UNICORN_PY=path/to/venv/bin/python fw/test_all.sh
tools/release.sh VERSION             # builds and writes manifests/latest.json
python3 tools/test_patchers.py stock/PolyendPlay_v1.6.0.ppf manifests/latest.json fw/PolyendPlay_v1.6.0_mod.ppf   # Python and browser patcher == build
python3 tools/check_clean.py stock/firmware.bin
```

The 32-bit host tests run in a container on macOS (`tools/docker32/Dockerfile`, image `play-test32`). `stock/` is ignored by git. Never commit firmware files, decompiler output or SD card contents. Code comments are still mostly German.

## How it works

The mod's compiled code is appended directly behind the stock image at `0xB069C`. Small patches at 111 places in the stock code redirect to it. Every jump into the mod goes through a short gate in the first block (the "safety net" above), which decides between the mod and the stock routine. Above 64 KB the code continues in the next flash segment, written the same way the stock file does it. The file `manifests/latest.json` holds that code and the replacement bytes.

## Credits and license

The code is MIT licensed, see [LICENSE](LICENSE). The license covers this project's own code only, not Polyend's firmware. The reverse engineering and much of the code were done with heavy use of AI assistance and tested on real hardware by the maintainer.

There is no support promise. Bug reports are welcome and handled when time allows.

## Support

If this mod is useful to you, you can support my work on [Ko-fi](https://ko-fi.com/envyu). It stays free either way.
