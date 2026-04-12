# QuestRetroDepth

QuestRetroDepth is a VR emulator experiment for Meta Quest.

Instead of placing the whole game on one flat virtual screen, it tries to separate layers in depth so the scene feels more spatial inside the headset.

It is based on RetroDepth:
https://github.com/maranone/RetroDepth

## What It Does

- Plays supported retro games directly on Quest.
- Renders emulator content with a depth-based VR presentation.
- Includes experimental rumble support for some games.

This project is currently focused on:

- SNES
- Genesis / Mega Drive

No games or ROMs are included.

## What To Expect

This is an experimental app, not a polished commercial emulator front end.

- Some games will look more convincing in depth than others.
- Performance and compatibility can vary by core, game, and content.
- Rumble support exists for some titles, but coverage is not complete.

## Basic Use

1. Install the APK on your Quest.
2. Launch QuestRetroDepth from the headset.
3. Use your own legally obtained game files.
4. Start a supported game and view it in the VR depth layout.

## Features

### VR Depth Presentation

The main idea of QuestRetroDepth is to make retro games feel less like a floating TV and more like layered artwork inside VR space.

### Experimental Rumble

QuestRetroDepth includes an experimental memory-based rumble system for SNES and Genesis / Mega Drive.

It uses per-game trigger profiles to detect useful gameplay events such as:

- damage taken
- life loss
- pickups
- score changes

Support varies by game.

## Important Notes

- Bring your own legally obtained games.
- No ROMs are bundled with the app.
- This app is an experiment and some rough edges are expected.

## Source Code

Source code:
https://github.com/maranone/QuestRetroDepth

## Third-Party Code And Licenses

This app uses third-party emulator code and related source material.

- QuestRetroDepth source: https://github.com/maranone/QuestRetroDepth
- RetroDepth base project: https://github.com/maranone/RetroDepth
- Snes9x: https://github.com/snes9xgit/snes9x
- PicoDrive current fork mentioned by upstream: https://github.com/irixxxx/picodrive
- PicoDrive original repo: https://github.com/notaz/picodrive
- OpenAI Retro: https://github.com/openai/retro
- Stable Retro: https://github.com/Farama-Foundation/stable-retro
- libretro database: https://github.com/libretro/libretro-database
- Action Replay MK3 documentation and cheat database: https://github.com/timboettiger/action-replay-mk-iii

License files included in this repo:

- `third_party/snes9x/LICENSE`
- `third_party/picodrive/COPYING`

Some bundled third-party subcomponents may also include their own additional license files.
