# QuestRetroDepth

QuestRetroDepth is a Meta Quest VR emulator experiment.

It runs as an Android/OpenXR app on Quest and renders emulator hardware layers at different depths in stereo VR instead of showing one flat screen.

This project is based on RetroDepth: https://github.com/maranone/RetroDepth

Source code: https://github.com/maranone/QuestRetroDepth

## Emulator Support

| System | Core | ROM formats | Status |
|---|---|---|---|
| SNES | Snes9x | `.smc` `.sfc` `.fig` `.swc` | Working |
| Genesis / Mega Drive | PicoDrive | `.md` `.gen` `.smd` `.bin` | Working |
| SMS / Game Gear | PicoDrive | `.sms` `.gg` | Working |
| GBA / GB / GBC | mGBA | `.gba` `.gb` `.gbc` | Work in progress |
| NES | FCEUmm | `.nes` `.unf` `.unif` | Work in progress |
| PC Engine | Beetle PCE | `.pce` `.sgx` | Work in progress |

Archives (`.zip`, `.7z`) are extracted automatically before loading.

No games or ROMs are included in the APK.

## Build

On Windows:

```bat
build_apk.bat
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

### Notes

- `libretro_database` contributes a lot of coverage, but many entries are heuristic and should be treated as `needs_review`.
- Some titles use odd, truncated, reordered, or regional names. The importer normalizes names and applies a small alias table to avoid duplicate profiles.
- Manual profiles are stored in `rumble/manual_profiles` when a game needs a specific hand-authored mapping.

## Third-Party Code And Licenses

This project includes third-party emulator code. Keep their license files with the source.

- Snes9x: https://github.com/snes9xgit/snes9x
- Snes9x license in this repo: `third_party/snes9x/LICENSE`
- PicoDrive current fork mentioned by upstream: https://github.com/irixxxx/picodrive
- PicoDrive original repo: https://github.com/notaz/picodrive
- PicoDrive license in this repo: `third_party/picodrive/COPYING`
- mGBA: https://github.com/mgba-emu/mgba
- mGBA license in this repo: `third_party/mgba/LICENSE`
- FCEUmm: https://github.com/libretro/libretro-fceumm
- FCEUmm license in this repo: `third_party/fceumm/Copying`
- Beetle PCE: https://github.com/libretro/beetle-pce-libretro
- Beetle PCE license in this repo: `third_party/beetle-pce/COPYING`

Some bundled emulator subfolders have their own extra license files too, for example `libchdr`, `zstd`, `lzma`, and `zlib` pieces under PicoDrive and some Snes9x helper code. Check the license files in `third_party/` before redistributing builds.

The app is not for bundling commercial ROMs or selling emulator cores. Bring your own legally obtained games.
