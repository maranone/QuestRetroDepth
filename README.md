# QuestRetroDepth

QuestRetroDepth is a Meta Quest VR emulator experiment.

It runs as an Android/OpenXR app on Quest and tries to render emulator layers at different depths in VR instead of showing one flat screen.

This is based on RetroDepth https://github.com/maranone/RetroDepth

Source code: https://github.com/maranone/QuestRetroDepth

Right now it has source for:

- SNES, using Snes9x/libretro code
- Genesis/Mega Drive, using PicoDrive/libretro code

No games or ROMs are included.

## Build

On Windows:

```bat
build.bat
```

You need an Android SDK/NDK setup that matches the Gradle project. The script looks for the SDK in this order:

- `ANDROID_HOME`
- `ANDROID_SDK_ROOT`
- `%LOCALAPPDATA%\Android\Sdk`

It also needs Java on `PATH`. JDK 21 is expected.

The debug APK is written to:

```text
apk/QuestRetroDepth-debug.apk
```

## Project Layout

```text
app/          Android app, Kotlin shell, native C++ OpenXR renderer
rumble/       Rumble source datasets, merged inventory, and manual gap-fill profiles
scripts/      Data import, merge, and asset packaging scripts
third_party/  Emulator source used by the native build
```

## Rumble Data

QuestRetroDepth has an experimental memory-based rumble system for SNES and Genesis/Mega Drive.

It does not ship ROMs. It ships small trigger profiles that watch emulator RAM values and fire haptics when a value changes in a useful way, for example:

- life loss: `prev > curr`
- damage taken: `prev > curr`
- pickup or ring change: `prev != curr`
- score increase: `prev < curr`

Those profiles are packaged into the APK as:

- `app/src/main/assets/rumble/catalog.tsv`
- `app/src/main/assets/rumble/profiles/*.qrr`

At runtime the native rumble manager loads that catalog and matches a ROM/game name to a packaged profile.

### Data Sources

The rumble mappings come from a mix of semantic RAM labels and community cheat/rumble datasets:

- OpenAI Retro: https://github.com/openai/retro
- Stable Retro: https://github.com/Farama-Foundation/stable-retro
- libretro database: https://github.com/libretro/libretro-database
- Action Replay MK3 documentation and built-in SNES cheat database: https://github.com/timboettiger/action-replay-mk-iii

In local workspace these are mirrored under:

- `rumble/openai_retro`
- `rumble/stable_retro`
- `rumble/libretro_database`
- `rumble/action_replay_mk_iii`

### Mapping Strategy

There are two broad classes of source data:

- semantic fields: labeled RAM values such as `lives`, `health`, `score`, `coins`
- raw cheat/rumble entries: addresses and community descriptions without clean semantic labels

The project currently prefers sources in this order:

1. `stable_retro` for labeled RAM fields
2. `openai_retro` for already-derived rumble trigger docs
3. `libretro_database` `(... Rumbles).cht` files for missing games only
4. `manual_profiles` for hand-authored fixes or one-off additions

This matters because a labeled field like `lives` is much safer to turn into a trigger than a generic cheat code blob.

### Scripts

Main scripts used for the rumble pipeline:

- `scripts/merge_rumble_sources.py`
  Builds `rumble/merged_inventory` from the downloaded datasets and records which source is best for each game.
- `scripts/import_rumbles_profiles.py`
  Imports SNES/Genesis `(... Rumbles).cht` files only for games that do not already have a profile.
- `scripts/package_rumble_assets.py`
  Converts the merged/manual rumble docs into packaged `.qrr` assets under `app/src/main/assets/rumble`.

The Gradle build runs `packageRumbleAssets` before `preBuild`, so the APK assets are regenerated from the current rumble data when you build.

### Notes

- `libretro_database` contributes a lot of coverage, but many entries are heuristic and should be treated as `needs_review`.
- Some titles use odd, truncated, reordered, or regional names. The importer normalizes names and applies a small alias table to avoid duplicate profiles.
- Manual profiles are stored in `rumble/manual_profiles` when a game needs a specific hand-authored mapping, such as Contra III.

## Third-Party Code And Licenses

This project includes third-party emulator code. Keep their license files with the source.

- Snes9x: https://github.com/snes9xgit/snes9x
- Snes9x license in this repo: `third_party/snes9x/LICENSE`
- PicoDrive current fork mentioned by upstream: https://github.com/irixxxx/picodrive
- PicoDrive original repo: https://github.com/notaz/picodrive
- PicoDrive license in this repo: `third_party/picodrive/COPYING`

Some bundled emulator subfolders have their own extra license files too, for example libchdr/zstd/lzma/zlib pieces under PicoDrive and some Snes9x filter/helper code. Check the license files in `third_party/` before redistributing builds.

The app is not for bundling commercial ROMs or selling emulator cores. Bring your own legally obtained games.
