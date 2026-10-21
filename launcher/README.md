# Qt launcher

The Windows package ships the official [shadPS4 Qt launcher](https://github.com/shadps4-emu/shadps4-qtlauncher)
together with this emulator. CI checks out the launcher at the commit in `UPSTREAM_COMMIT`,
applies the patches in `patches/` and builds it with `FORK_BUNDLE_REV` set to this repository's
commit.

The patches add:

- Update notifications from this repository's latest GitHub release. On confirmation the zip is
  downloaded and replaces the launcher and the emulator, then the launcher restarts.
- The included `shadPS4.exe` is listed and selected as the emulator version.
- A list of common window sizes instead of typing the width and height.
- A frame generation (FSR 3.1) switch in the Graphics tab, and a Toggle Frame Generation hotkey in
  the Hotkeys window that turns it on and off in game (default: 1).
- File > Scan for PS4 games searches every local and removable drive at once, without asking for a
  folder, and adds the folders that hold the games it finds to the library. The Windows folder and
  the other system folders are skipped.
- The compatibility list comes from the official database, with the games listed in
  `compatibility/overrides.json` on the `main` branch shown with the fork's status.
- About links only to this repository.
- File > Install Packages (PKG), from shadPS4 v0.7.0. Fake PKGs made with the default passcode
  (32 zeros) install without keys, with keys derived as in LibOrbisPkg. For other PKGs the Fake keyset and
  the PKG Derived Key3 keyset are read from the launcher's Crypto Key Manager.
- Existing `user` and `launcher` folders next to the launcher (or in the working directory) are
  used before the standard ones, and the emulator runs in the same place, so saves and settings
  that already exist keep being used. New folders are only made when none exist.

To update the launcher, change `UPSTREAM_COMMIT` and refresh the patches against it.
