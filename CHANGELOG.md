# Changelog

## April 17, 2026

### New: downloadable Homebrew library
You can now browse downloadable game catalogs from GitHub instead of relying on a fixed list inside the app.

What to try:
- Open the Homebrew section and browse the available JSON catalogs
- Add or update a catalog on GitHub and refresh the app to see it appear
- Download a few games directly from the in-app catalog

### Better first-time setup
If you launch the app without any ROMs installed, the app is now better at guiding you toward Homebrew instead of leaving you at a dead end.

What to try:
- Fresh install or empty ROM folder
- Open the app and check the new first-run flow

### APK now available in the GitHub repo
The project repo now includes the APK in the `apk/` folder, making it easier to grab the latest build without waiting for a separate release.

What to try:
- Download the APK directly from GitHub
- Compare it with the latest local build if you are testing often

### Much larger rumble/profile library
The app received a big update to its rumble and profile data across multiple systems.

What to try:
- Test rumble-enabled games you had tried before
- Try more GB and GBA titles, since a lot of profile data was added there

### Early mobile fallback work
There was a first pass at getting the app to run on non-VR Android devices. This was mainly groundwork and may still feel rough compared to the Quest experience.

What to try:
- Launch on Android phone/tablet if you are curious
- Expect experimentation rather than full polish

### Shared VR/mobile groundwork
A lot of internal work was done to move VR and mobile toward sharing more of the same rendering and UI systems. This is mostly future-facing, but it should make later improvements easier to bring across both modes.

What to try:
- If you use both Quest and Android, compare behavior between them
- Watch for newer builds where features arrive in both places more consistently

## April 13, 2026

### Cleanup pass
Removed unfinished controller-related docs and planning files from the public repo to keep things cleaner and more focused.

What changed for users:
- No new feature here
- Just less unfinished clutter in the project files

## Notes

This changelog is written for players and testers, not for developers. It focuses on visible changes and useful things to try.
