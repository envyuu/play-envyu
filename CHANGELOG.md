# Changelog

All notable changes to play-envyu. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), versions follow [Semantic Versioning](https://semver.org/):

- **Feature releases** (0.**x**.0) bring new features and get a video, a release post and the headline on the website.
- **Small updates** (0.x.**y**) fix bugs or polish things. They appear here, in a GitHub release and as one short line on the website.

The website always patches the latest version, and you can update over USB from the website.

## [0.2.1-beta] – 2026-11-18

Small update: USB audio on Windows.

### Changed
- **Audio Out is now Stereo by default.** This applies only if you never changed the setting; a saved setting is kept.
- Each Audio Out mode reports its own USB device version, so Windows sets the Play up fresh instead of reusing settings from an earlier firmware.

### Added
- New Audio Out mode **Stereo+Multi (Win)**: stereo and multitrack appear as two separate audio devices, so stereo keeps working even if Windows doesn't accept the multitrack part. **Stereo + Multi** stays a single audio device with all channels (recommended on Mac).
- [Troubleshooting](https://envyuu.github.io/play-envyu/#help) section on the website, with steps for Windows.

### Notes
- We can't test on Windows ourselves. If the Play still doesn't show up as an audio device, follow the [Windows steps](https://envyuu.github.io/play-envyu/#help-windows) and tell us in [issue #2](https://github.com/envyuu/play-envyu/issues/2).

## [0.2.0-beta] – 2026-11-17

Beat Slicer. See the [release notes](https://github.com/envyuu/play-envyu/releases/tag/v0.2.0-beta) and the [video](https://youtu.be/DHh3lllCfoM).

### Added
- Beat Slicer, sampling over USB (Stereo + In, Live Rec), tap tempo, new randomize types, Color Mode, Audio In sends, waveform view for sample tracks, safety net.

### Changed
- Perform keeps the track selection; engines sit in the main menu.

### Fixed
- USB audio was not detected as an audio device on Windows (missing interface association).

## [0.1.1-beta] – 2026-11-06

### Added
- First public release. Echo, Loop and CC Motion act only on the tracks selected in Perform. Website and video page.

## [0.1.0-beta] – 2026-11-05

First version (not published on its own; 0.1.1 was the first public release): MIDI effects with arpeggiator and echo, USB audio (stereo, multitrack, input), Note In Scale randomize, USB file transfer and firmware upload.

[0.2.1-beta]: https://github.com/envyuu/play-envyu/compare/v0.2.0-beta...v0.2.1-beta
[0.2.0-beta]: https://github.com/envyuu/play-envyu/releases/tag/v0.2.0-beta
