# Release checklist

## Must be done before going public

- [x] Flash this build (without slicer and granular) on a real device, 05.11.2026 via USB
- [ ] Switch the device off and on once to confirm a cold start
- [ ] Device test: MIDI effects, arpeggiator, Note In Scale, USB audio out and in, Live Rec short and long press, USB REC status display and messages, USB upload
- [ ] Decide on Rev B: test on a real device, or have the docs exclude it
- [ ] Decide whether USB `FLASH`, `PUT`, `DEL` and `PEEK` should be off by default, since any connected computer can use them
- [ ] Confirm the current download from Polyend has the SHA-256 in `manifests/latest.json`
- [ ] Have someone install from a clean checkout using only the README and the website
- [ ] Check the website's operating steps against the device: Arp and Randomize Type by double tap, Audio/MIDI page, reset button
- [x] After going public: enable GitHub Pages from `main` and `/docs`, then update the README link
- [x] Project name: play-envyu
- [ ] Decide on contact with Polyend and legal review

## Every release

- [ ] `fw/test_all.sh` passes, also with `BASELINE_DIR`, `BEAT_WAV` and `BEAT_REC` set
- [ ] `tools/release.sh VERSION` reports `patcher output == build`
- [ ] `tools/test_patchers.py` reports ok for the Python and the browser patcher (needs macOS `jsc` or Node)
- [ ] Safety net: `fw/emu_guard.py` passes; on the device, PEEK `0x1FFF9A38` reads 0 after a cold start
- [ ] `tools/check_clean.py stock/firmware.bin` reports 0 problems; review its overlap list
- [ ] Build flashed and cold-started on a real device

## Later

- [ ] Translate code comments and test output to English
- [ ] Move selected notes from the private vault into `docs/`
- [ ] Remove or keep the unused weak extension hooks `sl_menu_add` and `sv_assign_sample`

## Small updates (0.x.y)

Feature releases (0.x.0) get the headline, a video and a release post. Small updates stay quiet:

- [ ] `ENVYU_VERSION` in the source's `version.h`, `tools/sync_fw.sh`, full test run, then `tools/release.sh 0.x.y-beta` (manifest, patcher == build)
- [ ] `CHANGELOG.md`: new entry on top (Added / Changed / Fixed / Notes), compare link at the bottom
- [ ] Website hero: replace the single `<p class="update">` line (tag, date, one sentence, links). Don't touch the eyebrow, headline or video
- [ ] README: replace the `Latest:` line under the intro
- [ ] Tag `v0.x.y-beta`, GitHub release as pre-release, title `0.x.y-beta – <what>`, body = changelog entry + install link
- [ ] Point open issues to it; no new video
