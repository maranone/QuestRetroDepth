# QuestRetroDepth

QuestRetroDepth is a VR emulator experiment for Meta Quest.

Instead of placing the whole game on one flat virtual screen, it tries to separate layers in depth so the scene feels more spatial inside the headset.

It is based on RetroDepth: https://github.com/maranone/RetroDepth

No games or ROMs are included.

---

## What's New

**Version 0.04**
- Bluetooth controller support (untested)
- SideQuest now uses the unsigned APK — should install straight from SideQuest without needing GitHub

**Version 0.03**
- Quick patch: removed emulator freeze/unfreeze temporarily (was causing delay in emulation)

**Version 0.02**
- Perspective Compensation: resizes later layers to match their depth positions
- Parallax effect: tilts layers toward the direction you turn (adjustable — can be distracting at high values)
- Bigger ambilight area
- Rumble system rebuilt from scratch — no more vibration queues, every event fires immediately
- Fixed: 90–120 Hz display modes should now work correctly
- Fixed: VR resolution (1.5x recommended and now the default)
- Work in progress: Environment sphere, upscale, Quick Edit presets

---

## Emulator Support

| System | Core | ROM Formats | Status |
|---|---|---|---|
| SNES | Snes9x | .smc .sfc .fig .swc | Working |
| Genesis / Mega Drive | PicoDrive | .md .gen .smd .bin | Working |
| SMS / Game Gear | PicoDrive | .sms .gg | Working |
| GBA / GB / GBC | mGBA | .gba .gb .gbc | Work in progress |
| NES | FCEUmm | .nes .unf .unif | Work in progress |
| PC Engine | Beetle PCE | .pce .sgx | Work in progress |
| ScummVM | ScummVM libretro | various | Experimental |

Archives (.zip, .7z) are extracted automatically before loading.

---

## What It Does

- Plays supported retro games directly on Quest
- Renders emulator content with a depth-based VR presentation — hardware layers placed at different depths in stereo instead of one flat screen
- Immersive audio processing with head-tracked spatial EQ
- Experimental rumble support for some games

---

## Immersive Audio

The settings panel includes audio modes that go beyond flat stereo:

- **Off** — standard audio passthrough
- **Wide** — M/S stereo widening for a broader sound stage
- **Spatial EQ** — 3-band EQ modulated by head pitch: looking up lifts the high frequencies, looking down lifts the bass
- **Spatial EQ + Haptics** — same spatial EQ, plus bass energy drives controller rumble in real time
- **Screen Lock** — anchors the stereo field to the screen's world position so the audio perspective shifts naturally as you turn your head

---

## VR Depth Presentation

The main idea of QuestRetroDepth is to make retro games feel less like a floating TV and more like layered artwork inside VR space. Each hardware layer (background, sprites, foreground) can be placed at a different depth, with controls for depth, width, visibility, and ambilight glow.

---

## Experimental Rumble

QuestRetroDepth includes an experimental memory-based rumble system for SNES and Genesis / Mega Drive.

It uses per-game trigger profiles to detect gameplay events such as:

- Damage taken
- Life loss
- Pickups
- Score changes

Support varies by game.

---

## Important Notes

- Bring your own legally obtained games. No ROMs are bundled with the app.
- This app is an experiment — some rough edges are expected.
- First launch will ask for storage permissions. Once granted, it will create /QuestRetroDepth/roms/ folders on your device.
- Some games will look more convincing in depth than others. Performance and compatibility can vary by core, game, and content.

---

## Source Code

https://github.com/maranone/QuestRetroDepth

---

## Third-Party Code and Licenses

This app includes third-party emulator code. License files are included with the source.

**Project base**
- QuestRetroDepth: https://github.com/maranone/QuestRetroDepth
- RetroDepth (base project): https://github.com/maranone/RetroDepth

**Emulator cores**
- Snes9x: https://github.com/snes9xgit/snes9x
- PicoDrive (current fork): https://github.com/irixxxx/picodrive
- PicoDrive (original): https://github.com/notaz/picodrive
- mGBA: https://github.com/mgba-emu/mgba
- FCEUmm: https://github.com/libretro/libretro-fceumm
- Beetle PCE: https://github.com/libretro/beetle-pce-libretro

**Rumble data sources**
- OpenAI Retro: https://github.com/openai/retro
- Stable Retro: https://github.com/Farama-Foundation/stable-retro
- libretro database: https://github.com/libretro/libretro-database
- Action Replay MK3 documentation and cheat database: https://github.com/timboettiger/action-replay-mk-iii

Some bundled third-party subcomponents may include their own additional license files. Check the license files in third_party/ before redistributing builds.
