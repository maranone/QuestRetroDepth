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
third_party/  Emulator source used by the native build
```

## Third-Party Code And Licenses

This project includes third-party emulator code. Keep their license files with the source.

- Snes9x: https://github.com/snes9xgit/snes9x
- Snes9x license in this repo: `third_party/snes9x/LICENSE`
- PicoDrive current fork mentioned by upstream: https://github.com/irixxxx/picodrive
- PicoDrive original repo: https://github.com/notaz/picodrive
- PicoDrive license in this repo: `third_party/picodrive/COPYING`

Some bundled emulator subfolders have their own extra license files too, for example libchdr/zstd/lzma/zlib pieces under PicoDrive and some Snes9x filter/helper code. Check the license files in `third_party/` before redistributing builds.

The app is not for bundling commercial ROMs or selling emulator cores. Bring your own legally obtained games.
