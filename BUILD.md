# Source-only build notes

This staging tree intentionally contains the QuestRetroDepth application source,
build configuration, and only the local changes made to bundled emulator trees.
Unchanged upstream emulator code, APKs, ROMs, generated assets, logs, backups,
and device/deployment scripts are not copied.

## Restore upstream emulator sources

Before building, clone each upstream repository listed in `.source-revisions.txt`
into the matching `third_party` folder and check out its recorded `HEAD` revision.
Then copy the files from this export's matching `third_party` folders over those
checkouts. The copied files are the project's local emulator patches.

The `third_party/cgltf` and `third_party/imgui` folders are small source-only
dependencies and are included in full.

## Android build prerequisites

- JDK 21 on `PATH`
- Android SDK with API 36
- Android NDK `30.0.14904198`
- CMake `4.1.2`
- Gradle 8.7 or a compatible Gradle installation

The full working tree also supplies runtime data and asset-generation inputs
that are deliberately outside this source-only export. Restore those project
inputs from the original workspace before packaging an APK.

From the project root, run:

```bat
gradle --no-daemon assembleDebug
```

The debug APK is produced at `app/build/outputs/apk/debug/app-debug.apk`.
