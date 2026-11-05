<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

<h1 align="center">
  <br>
  <img src="./.github/shadps4.png" width="200">
  <br>
  <b>shadPS4 Anti-Crash</b>
  <br>
</h1>

<p align="center">
A PlayStation 4 emulator for Windows, built to crash less and run more games.
</p>

## Download and play

1. Download the latest zip from [**Releases**](https://github.com/vsajjadx/shadPS4/releases).
2. Unzip it and run `shadPS4QtLauncher.exe`.
3. Add your games folder and play.

**No separate launcher is needed.** The launcher and the emulator come together in one zip.

**No extra settings are needed.** You don't have to change anything in the config or make
per-game settings. The default settings work.

> The first time you start a game it can be a little slower while shaders are built. After
> that it runs normally.

## What's new

### Direct PKG execution: launch games faster than on the console

No installation. No extraction. No waiting. Launch your PS4 games straight from PKG files,
even faster than the console's usual installation process.

You no longer need to extract game files, create installation folders, or convert anything to
ZAR:

1. Put your **Base Game PKG**, **Update PKG** and **DLC PKG** files together in one folder.
2. Select that folder in the launcher.
3. Launch the game.

The emulator reads the original PKG files directly and handles the base game, updates and DLCs
separately.

- Much less preparation time.
- Much less storage space needed, with no duplicate game files.
- No waiting for large games to finish extracting.
- Updates and DLCs stay separate.
- No noticeable FPS loss during gameplay in my tests.

### FC 27 runs smoothly

FC 27 was tested with direct PKG execution and runs remarkably smoothly.

Recommended settings:

- **Redback Mode:** set it to `Precise` for the best image quality.
- **Pitch colors:** if the pitch colors look wrong, set the stadium weather to
  `Sunny (Summer)` for now.

You may still see some visual issues in cutscenes, such as excessive brightness or visible lines.
These and the pitch color problem are expected to be fixed in upcoming updates. Gameplay itself
stays smooth, with no noticeable FPS loss from direct PKG execution.

### More performance optimizations

This update also improves performance and compatibility in several games, including
**The Order: 1886**, **UFC 4**, **NHL 24** and many others. The goal is not only to run more
games, but to make the playable ones run better and more consistently.

### Earlier: DLSS5, frame generation and Boost

Release: [**DLSS5 arrives: frame generation, Boost and Auto Quality**](https://github.com/vsajjadx/shadPS4/releases/tag/dlss5-2026-11-01)

- **DLSS5, one key away.** Press `2` in game for sharper, cleaner images.
- **Frame Generation** on `1` and **Boost** on `3`: smoother motion and extra performance when
  you need them.
- **Everything in Settings → Graphics.** DLSS5, Boost, Frame Generation and Auto Quality all have
  their switches there, and they start off.
- **On an RTX 20 or 30 series card?** Turn on **Boost**, **Frame Generation** and **Auto
  Quality** together for a smooth, steady experience.
- **Alt+Enter** gives a borderless fullscreen window when a game runs in a window.

## The launcher

The zip includes the latest official shadPS4 Qt launcher, with a few additions:

- **Automatic updates.** When a new version is released here, the launcher tells you.
- **Your saves are safe.** The launcher uses the save and settings folders you already have.
  It only makes new ones if none exist, and updates never touch them.
- **Easy window size.** Pick the size from a list (720p, 1080p, 1440p, 4K and more) instead of
  typing numbers.
- **Graphics switches.** DLSS5, Boost, Frame Generation and Auto Quality are all in **Settings → Graphics**.
- **Compatibility list.** Shows the official list, plus the games this build runs that the
  official one doesn't.
- **Direct PKG execution.** Point it at a folder with your base game, update and DLC PKGs and
  launch, with no extraction.

> Already using the official launcher with a `user` folder next to it? Unzip into that same
> folder and replace the files. Your games, saves and settings stay as they are.

## Screenshots

| EA SPORTS FC 27 |
| :---: |
| ![EA SPORTS FC 27](./documents/Screenshots/games/fc27.jpg) |

| inFAMOUS Second Son | DRIVECLUB |
| :---: | :---: |
| ![inFAMOUS Second Son](./documents/Screenshots/games/infamous-second-son.jpg) | ![DRIVECLUB](./documents/Screenshots/games/driveclub.jpg) |

| The Grand Tour Game | The Order: 1886 |
| :---: | :---: |
| ![The Grand Tour Game](./documents/Screenshots/games/grand-tour.jpg) | ![The Order: 1886](./documents/Screenshots/games/the-order-1886.jpg) |

| NHL 24 | EA SPORTS UFC 4 |
| :---: | :---: |
| ![NHL 24](./documents/Screenshots/games/nhl24.jpg) | ![UFC 4](./documents/Screenshots/games/ufc4.jpg) |

| Need for Speed Hot Pursuit Remastered | inFAMOUS First Light |
| :---: | :---: |
| ![Need for Speed Hot Pursuit Remastered](./documents/Screenshots/games/nfs-hot-pursuit.jpg) | ![inFAMOUS First Light](./documents/Screenshots/games/infamous-first-light.jpg) |

| Life Is Strange | Murdered: Soul Suspect |
| :---: | :---: |
| ![Life Is Strange](./documents/Screenshots/games/life-is-strange.jpg) | ![Murdered: Soul Suspect](./documents/Screenshots/games/murdered-soul-suspect.jpg) |

**DualSense support:** If you use a DualSense controller, games that have a separate controller audio channel will play it through the controller's speaker, with no special settings required.

## What is different from the official shadPS4

- **Fewer crashes.** Many errors that used to close the emulator are now caught, and the game
  keeps running.
- **Fewer freezes.** Fixes for games that got stuck on loading screens.
- **Better graphics.** Fixes for dark scenes, lighting and shadows, especially on NVIDIA cards.
- **Faster.** Better use of your CPU and GPU.
- **DLSS5, Boost and frame generation (optional).** In game, `1` toggles frame generation, `2`
  DLSS5 and `3` Boost, or use the switches in the launcher settings. All are off by default.
- **Direct PKG execution.** Launch games from base, update and DLC PKGs with no extraction.
- **More games run.** Games like NHL 24, The Order: 1886, The Grand Tour Game and Need for Speed
  Hot Pursuit Remastered, which do not run on the official version, work here.
- **Launcher included.** One download, with automatic updates.
- **Always up to date.** The latest changes from the official shadPS4 are included.

## Made with the help of Claude

This fork was made with the help of **Claude**, the AI assistant by Anthropic. Claude studied
crash and freeze logs, found the causes, wrote the fixes, and brought in good fixes from other
open-source projects.

## Thanks

- The [**shadPS4**](https://github.com/shadps4-emu/shadPS4) team, for the emulator this fork is
  built on.
- Community forks and other open-source projects, for fixes and reference code.
- AMD [**FidelityFX**](https://github.com/FireBurn/FSR-Vulkan), for FSR frame generation.

## License

GPL-2.0-or-later, like the official shadPS4. This project does not include any games or
PlayStation system files. You need your own legally dumped games.
