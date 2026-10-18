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
- A frame generation (FSR 3.1) switch in the Graphics tab.
- Existing `user` and `launcher` folders next to the launcher (or in the working directory) are
  used before the standard ones, and the emulator runs in the same place, so saves and settings
  that already exist keep being used. New folders are only made when none exist.

To update the launcher, change `UPSTREAM_COMMIT` and refresh the patches against it.
