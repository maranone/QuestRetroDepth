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


