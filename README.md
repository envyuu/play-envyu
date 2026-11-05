# play-envyu

Unofficial, community-made extensions for the Polyend Play. This repository contains no Polyend firmware: you download the official firmware yourself, and a small script applies the mod to it on your computer.

> **Beta, at your own risk.** Flashing modified firmware can make your device unusable and may affect your warranty. So far it has been tested on one single Rev A device, and testing of this build is still in progress. It is not affiliated with or endorsed by Polyend. "Polyend" and "Play" are used only to say which hardware this is for.

## What it adds

- **MIDI effects** including arpeggiator and echo
- **USB audio:** stereo and multitrack output, and input from the computer
- **Recording of incoming USB audio** to the SD card: hold Live Rec for about one second
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

## Going back

Put only the original `PolyendPlay_v1.6.0.ppf` in `/Firmware` and install it from the update menu. If the device hangs, a short press of the reset button while it runs started the emergency flash on our test device. Keep a copy of the original firmware on your computer.

## Building from source

You need `arm-none-eabi-gcc`, Python 3 and your own stock firmware at `stock/PolyendPlay_v1.6.0.ppf`.

```
python3 tools/hex2bin.py stock/PolyendPlay_v1.6.0.ppf stock/firmware.bin
pip install unicorn numpy pillow     # for the emulator tests, ideally in a venv
UNICORN_PY=path/to/venv/bin/python fw/test_all.sh
tools/release.sh VERSION             # builds and writes manifests/latest.json
python3 tools/check_clean.py stock/firmware.bin
```

`stock/` is ignored by git. Never commit firmware files, decompiler output or SD card contents. Code comments are still mostly German.

## How it works

The mod's compiled code is appended directly behind the stock image at `0xB069C`. Small patches at 66 places in the stock code redirect to it. The file `manifests/latest.json` holds that code and the replacement bytes.

## Credits and license

The code is MIT licensed, see [LICENSE](LICENSE). The license covers this project's own code only, not Polyend's firmware. The reverse engineering and much of the code were done with heavy use of AI assistance (Claude Code) and tested on real hardware by the maintainer.

There is no support promise. Bug reports are welcome and handled when time allows.
