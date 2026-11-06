# DLSS5 (Magpie, RTX 30)

The Windows package ships the Magpie build of
[DLSS5-RTX30](https://github.com/lmxxf/DLSS5-RTX-3080-laptop) v0.4.0 in `qtplugins\DLSS5`. It
runs NVIDIA's DLSS5 neural renderer (`nvngx_dlssnr.dll`, patched for Ampere `sm_86`) on a window
and then DLSS Super Resolution, so it needs an RTX 30 series card and a game running in a window.

## What runs where

| Piece | Where it lives |
| --- | --- |
| Settings | `user/dlss5.json`, read by `src/core/dlss5.{h,cpp}` |
| Emulator UI rows | Graphics tab of the in-game settings dialog (`settings_dialog_imgui.cpp`) |
| Launcher UI | `launcher/patches/0011-*` to `0014-*`, `0017-*` and `0025-*` (Graphics tab), `0012-*` (Hotkeys dialog) |
| Hotkeys | The emulator's hotkey system (`input_handler`), `2` toggles DLSS5 and `3` Boost by default |
| Package | `.github/workflows/build.yml`, from the `014` release of this repository (SHA256 checked) |

## How the emulator drives Magpie

When a game starts with DLSS5 enabled, or when the DLSS5 key is pressed, the emulator:

1. Writes the settings into the `DLSS5-RTX30-PTX` scaling mode of `qtplugins\DLSS5\config\config.json`
   (DLSSNR `style`, `intensity`, `localToneStrength`, `localStructureStrength`,
   `skinStructureStrength`, and with Boost `enableInputResolutionScaling` and
   `inputResolutionPercent`).
2. Adds a `shadPS4` profile for its own executable and window class with `autoScale` on, so Magpie
   scales the game window with that mode whenever it is in front.
3. Starts `Magpie.exe -t` in a job object, so Magpie ends with the emulator even after a crash.

Turning DLSS5 off ends Magpie. Boost and setting changes rewrite the config and restart it.

| `dlss5.json` key | Range | Magpie parameter |
| --- | --- | --- |
| `Enabled` | bool | starts Magpie with the game |
| `Boost` | bool | `enableInputResolutionScaling` |
| `NetworkResolutionPercent` | 30-100 | `inputResolutionPercent` |
| `Style` | 0-2 (Standard, Natural, Cinematic) | `style` |
| `Intensity` | 0-1 | `intensity` |
| `LocalTone` | 0-2 | `localToneStrength` |
| `LocalStructure` | 0-2 | `localStructureStrength` |
| `SkinStructure` | -1 to 2.5 | `skinStructureStrength` |
