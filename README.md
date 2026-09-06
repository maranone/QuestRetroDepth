# QuestRetroDepth

QuestRetroDepth (QRD) is an open-source Meta Quest VR emulator frontend that renders retro games in stereoscopic 3D. It places emulator layers at different depths instead of displaying one flat screen. ROMs are not included; users provide their own legally owned games.

## Credits and Acknowledgements

QuestRetroDepth is based on and adapted from the [RetroDepth project](https://github.com/maranone/RetroDepth).

The project incorporates and modifies open-source code from the following emulator projects:

- [MAME](https://github.com/mamedev/mame) — arcade emulation framework
- [Libretro MAME](https://github.com/libretro/mame) — Libretro MAME core
- [MAMEdev.org](https://www.mamedev.org) — official MAME project site
- [MAME licensing](https://github.com/mamedev/mame/blob/master/COPYING) — license information
- [Snes9x](https://github.com/snes9xgit/snes9x) — SNES emulation core
- [mGBA](https://mgba.io) — GBA, GB, and GBC emulation core
- [PicoDrive](https://github.com/libretro/picodrive) — Genesis, SMS, and Game Gear emulation core
- [FCEUmm](https://github.com/libretro/libretro-fceumm) — NES emulation core
- [Beetle PCE Fast](https://github.com/libretro/beetle-pce-fast-libretro) — PC Engine emulation core
- [SwanStation](https://github.com/libretro/swanstation) — PlayStation emulation core
- [DuckStation](https://github.com/stenzek/duckstation) — SwanStation upstream project
- [PGXP](https://github.com/iCatButler/pgxp) — precision geometry transformation pipeline

QuestRetroDepth is also built with these open-source libraries and APIs:

- [libretro](https://www.libretro.com) — emulator core integration API
- [Khronos OpenXR](https://www.khronos.org/openxr/) — VR runtime API
- [zlib](https://zlib.net) — compression library

Other project credits:

- [FireWarden](https://opengameart.org/content/lowpoly-pistol) — CC0 low-poly pistol model
- [8-BIT Adventure Music 2](https://elv-games.itch.io) — music by ElvGames and pegonthetrack

If you enjoy QuestRetroDepth and would like to support development, you can [buy me a coffee on Ko-fi](https://ko-fi.com/retrodepth). Support is completely optional; the project remains free and open source.

No games or ROMs are included in the APK.

## Build

The source-only GitHub export intentionally omits unchanged upstream emulator trees and generated/runtime data. Read [BUILD.md](BUILD.md) first for dependency restoration instructions.

After restoring the omitted source dependencies and project inputs, build with:

```bat
gradle --no-daemon assembleDebug
```

You need an Android SDK/NDK setup that matches the Gradle project, plus JDK 21 on `PATH`. The Android SDK is normally found through:

- `ANDROID_HOME`
- `ANDROID_SDK_ROOT`
- `%LOCALAPPDATA%\Android\Sdk`

The debug APK is written to:

```text
app/build/outputs/apk/debug/app-debug.apk
```

## Install on Quest

Connect the headset with USB, allow USB debugging in the headset prompt, then run:

```bat
install_apk.bat
```

The script installs `apk/QuestRetroDepth-debug.apk`. To install another APK:

```bat
install_apk.bat path\to\QuestRetroDepth.apk
```

ADB is detected from `ANDROID_HOME`, `ANDROID_SDK_ROOT`, `%LOCALAPPDATA%\Android\Sdk`, or `PATH`.

## Copy ROMs to Quest

No ROMs are included in this repository. Put your legally owned ROMs under the local `roms\` folder, using the system folders already provided, then run:

```bat
copy_roms.bat
```

That copies the local ROM tree to:

```text
/sdcard/QuestRetroDepth/roms
```

To install the APK and copy ROMs in one step:

```bat
install_apk_and_roms.bat
```

You can also copy from another local folder without moving files into the repo:

```bat
copy_roms.bat D:\MyRoms
```

## Dynamic Homebrew Catalogs

The app can discover downloadable homebrew catalogs directly from the repository instead of hardcoding a fixed list in the APK.

At runtime it queries:

```text
https://api.github.com/repos/maranone/QuestRetroDepth/contents/homebrew?ref=main
```

The selector in the app shows every JSON file in `homebrew/` that is a valid catalog. A valid catalog is a JSON object with a top-level `roms` array.

This means:

- If you edit an existing catalog JSON in `homebrew/`, the app will see the updated contents the next time it fetches that file.
- If you add a new valid catalog JSON to `homebrew/`, it will appear in the selector without rebuilding the APK.
- If GitHub is temporarily unavailable, the app reuses the last validated remote feed list for the session and falls back to built-in defaults only when no validated remote list has been loaded yet.

### Catalog Format

Each catalog file should look like this:

```json
{
  "feed": "Featured",
  "roms": [
    {
      "name": "Example Game",
      "author": "Example Author",
      "license": "MIT",
      "website": "https://example.com/game",
      "download": "https://example.com/game.zip",
      "system": "nes",
      "filename": "example-game.nes",
      "license_url": "https://example.com/license",
      "source": "Homebrew Hub",
      "source_entry_url": "https://example.com/game-page",
      "distribution_mode": "official",
      "mirror_allowed": false,
      "notes": "Optional free-form notes"
    }
  ]
}
```

Supported `system` values for downloadable homebrew entries are:

- `nes`
- `gb`
- `gbc`
- `gba`
- `sms`
- `gg`
- `snes`
- `genesis`
- `pce`

Required per entry:

- `name`
- `author`
- `license`
- `download`
- `system`

Optional per entry:

- `website`
- `filename`
- `license_url`
- `source`
- `source_entry_url`
- `distribution_mode`
- `mirror_allowed`
- `notes`

### Download Behavior

When downloading a homebrew entry, the app resolves the saved file name in this order:

1. `filename` from the catalog
2. `Content-Disposition` from the HTTP response
3. Final redirected URL path
4. Original `download` URL path

If the resolved file name does not end in a supported ROM or archive extension, the download is rejected instead of saving an unusable file.

## Project Layout

```text
app/          Android app, Kotlin shell, native C++ OpenXR renderer
homebrew/     Remote JSON catalogs discovered by the in-app homebrew selector
rumble/       Rumble source datasets, merged inventory, and manual gap-fill profiles
scripts/      Data import, merge, and asset packaging scripts
third_party/  Emulator source used by the native build
```

## Rumble Data

QuestRetroDepth has an experimental memory-based rumble system for SNES and Genesis / Mega Drive.

It does not ship ROMs. It ships small trigger profiles that watch emulator RAM values and fire haptics when a value changes in a useful way, for example:

- life loss: `prev > curr`
- damage taken: `prev > curr`
- pickup or ring change: `prev != curr`
- score increase: `prev < curr`

Those profiles are packaged into the APK as:

- `app/src/main/assets/rumble/catalog.tsv`
- `app/src/main/assets/rumble/profiles/*.qrr`

At runtime the native rumble manager loads that catalog and matches a ROM or game name to a packaged profile.

### Data Sources

The rumble mappings come from a mix of semantic RAM labels and community cheat or rumble datasets:

- OpenAI Retro: https://github.com/openai/retro
- Stable Retro: https://github.com/Farama-Foundation/stable-retro
- libretro database: https://github.com/libretro/libretro-database
- Action Replay MK3 documentation and built-in SNES cheat database: https://github.com/timboettiger/action-replay-mk-iii

In the local workspace these are mirrored under:

- `rumble/openai_retro`
- `rumble/stable_retro`
- `rumble/libretro_database`
- `rumble/action_replay_mk_iii`

### Mapping Strategy

There are two broad classes of source data:

- semantic fields: labeled RAM values such as `lives`, `health`, `score`, `coins`
- raw cheat or rumble entries: addresses and community descriptions without clean semantic labels

The project currently prefers sources in this order:

1. `stable_retro` for labeled RAM fields
2. `openai_retro` for already-derived rumble trigger docs
3. `libretro_database` `(... Rumbles).cht` files for missing games only
4. `manual_profiles` for hand-authored fixes or one-off additions

This matters because a labeled field like `lives` is much safer to turn into a trigger than a generic cheat-code blob.

### Scripts

Main scripts used for the rumble pipeline:

- `scripts/merge_rumble_sources.py`
- `scripts/import_rumbles_profiles.py`
- `scripts/package_rumble_assets.py`

The Gradle build runs `packageRumbleAssets` before `preBuild`, so the APK assets are regenerated from the current rumble data when you build.


The app is not for bundling commercial ROMs or selling emulator cores. Bring your own legally obtained games.
