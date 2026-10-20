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
- A frame generation (FSR 3.1) switch in the Graphics tab, and a Toggle Frame Generation key in the
  Hotkeys dialog (default `1`; a single key, a combination like `Ctrl+1`, or a controller button).
- The controller and Hotkeys dialogs read a click on the DualSense / DualShock 4 touchpad as
  `touchpad_left`, `touchpad_center` or `touchpad_right`, by the third of the pad the finger is on
  (it used to be ignored, leaving the field on `back`). The emulator takes these as inputs, so they
  work for any output and for the hotkeys.
- File > Scan for PS4 games: scans every drive in the background, skips the Windows folder and
  system folders, and adds the PS4 games it finds to the library without asking for a folder.
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
