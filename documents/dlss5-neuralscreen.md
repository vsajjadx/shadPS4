# NeuralScreen integration

The Windows package can ship [NeuralScreen](https://github.com/vsajjadx/NeuralScreen)
next to the emulator. NeuralScreen is a separate process for neural rendering and
Boost. The emulator starts it for the game window, sends its configuration and
forwards its neural-rendering and Boost hotkeys. FSR 3.1 frame generation is owned
by the emulator's Vulkan presenter, not NeuralScreen. The emulator presents each
real or generated frame to its window before NeuralScreen captures that output.

## What runs where

| Piece | Where it lives |
| --- | --- |
| DLSS5 settings | `src/core/neural_screen.{h,cpp}` in the emulator, own files, no `emulator_settings` keys |
| Emulator UI rows | Graphics tab of the in-game settings dialog (`settings_dialog_imgui.cpp`) |
| Launcher UI | `launcher/patches/0011-*` (Graphics tab), `0012-*` (Hotkeys dialog) and `0013-*` (Model + Fine Tuning group) on top of shadps4-qtlauncher |
| Hotkeys | The emulator's regular hotkey system (`input_handler`), keys `1` / `2` / `3` by default |
| Frame generation | The emulator's built-in Vulkan FSR 3.1.6 presenter (`ENABLE_FSR_FRAMEGEN`, enabled by default on Windows) |

## Files

- `<user>/neural_screen.json` - the DLSS5 options. Written by the in-game dialog
  and by the launcher's Graphics tab, read by the emulator when a game starts.
  Keys: `Enabled`, `Boost`, `NetworkResolutionPercent` (30-100), `Passes` (1-4),
  `Profile` (0-3: Faithful, Natural, Strong / Cinematic, Extreme / Overdrive),
  `Model` (0-2: Default, Natural, Cinematic - which network produces the picture),
  `Intensity` (0-1), and the fine tuning `LocalTone` (0-2), `LocalStructure` (0-2),
  `SkinStructure` (-1 to 2.5), all three defaulting to 1.5.
- `<user>/neural_screen_app.json` - generated for each run. The emulator writes it
  before spawning NeuralScreen and after every settings change; a running
  NeuralScreen reloads it on the `reload` command. It carries
  `external_tuning: true` and `style` (the model): NeuralScreen's compact-ui
  build otherwise pins the three fine tuning values to 1.5, because its own
  menu has no sliders for them. `launcher/neuralscreen/0003-*` makes it keep
  the values the emulator writes and apply a changed model, intensity or fine
  tuning live on `reload`.
- Frame generation is an emulator setting (`frame_generation_enabled`) and is
  never enabled in NeuralScreen's generated app config. Its hotkey toggles the
  emulator's Vulkan FSR 3.1 presenter independently of NeuralScreen.

## Runtime protocol

The emulator starts the bundled `runtime/pythonw.exe` on `main.py` itself (looked up in
`qtplugins/NeuralScreen/` next to the emulator), with

```
main.py --config <user>/neural_screen_app.json --window <hwnd> --ctl 53194 --instance shadPS4
```

and talks to it on localhost TCP port 53194, one command per line. NeuralScreen
receives neural-rendering, Boost, reload and quit controls; the FSR frame-generation
hotkey stays in the emulator and does not depend on this process. Commands are sent
on detached threads, so a dead NeuralScreen never stalls a frame.

NeuralScreen is stopped (`quit`, then terminate after 5 s) when the game window
closes and before a relaunch.

## Updating across emulator updates

The integration touches a small number of places; when merging an upstream
update, rebase this branch on it (do not merge in place) and keep:

- `src/core/neural_screen.{h,cpp}` (standalone module) and its two lines in
  `CMakeLists.txt` (CORE sources, `ws2_32`)
- the `NeuralScreen::StartForGameWindow` / `Stop` calls in `emulator.cpp`
- the three hotkey cases in `input_handler` and `sdl_window.cpp`
- the emulator's Vulkan FSR 3.1 frame generator enabled independently of NeuralScreen
- `launcher/patches/0011-*` to `0013-*`, `launcher/neuralscreen/0001-*` to `0003-*`; rebase them if the launcher's
  `UPSTREAM_COMMIT` moves

Everything the user sees (both config dialogs) and the whole NeuralScreen app
live outside the emulator's settings code, so upstream changes to
`emulator_settings` cannot break the integration.

## Licence note

NeuralScreen is PolyForm Strict (non-commercial); the emulator and launcher are
GPL-2.0-or-later. The two programs talk over a socket and share no code, and
NeuralScreen is an optional download beside the emulator, not part of this
repository.
