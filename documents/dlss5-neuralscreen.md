# DLSS5 (NeuralScreen) integration

The Windows package can ship [NeuralScreen](https://github.com/vsajjadx/NeuralScreen)
next to the emulator. NeuralScreen is a separate process that hooks the game window
and provides neural rendering (DLSS5 in the UI), FSR frame generation and its boost
mode. The emulator starts it with the game window, feeds it its configuration and
forwards the three hotkeys to it; it never renders inside the emulator process, so
emulator updates and NeuralScreen updates stay independent.

## What runs where

| Piece | Where it lives |
| --- | --- |
| DLSS5 settings | `src/core/neural_screen.{h,cpp}` in the emulator, own files, no `emulator_settings` keys |
| Emulator UI rows | Graphics tab of the in-game settings dialog (`settings_dialog_imgui.cpp`) |
| Launcher UI | `launcher/patches/0011-*` (Graphics tab), `0012-*` (Hotkeys dialog) and `0013-*` (Model + Fine Tuning group) on top of shadps4-qtlauncher |
| Hotkeys | The emulator's regular hotkey system (`input_handler`), keys `1` / `2` / `3` by default |
| Frame generation | NeuralScreen's; the emulator's built-in FSR 3.1 frame generation is compiled out (`ENABLE_FSR_FRAMEGEN` defaults to `OFF`) |

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
- Frame generation itself stays an emulator setting (`frame_generation_enabled`,
  the launcher's Graphics switch) and is passed to NeuralScreen on start; the
  frame generation hotkey toggles it inside NeuralScreen.

## Runtime protocol

The emulator starts the bundled `runtime/pythonw.exe` on `main.py` itself (looked up in
`qtplugins/NeuralScreen/` next to the emulator), with

```
main.py --config <user>/neural_screen_app.json --window <hwnd> --ctl 53194 --instance shadPS4
```

and talks to it on localhost TCP port 53194, one command per line: `nr`, `fg`,
`boost`, `reload`, `quit`. Replies are `ok`/`err`. The three hotkeys map to
`hotkey_toggle_frame_generation` (default `1`), `hotkey_toggle_dlss5` (`2`) and
`hotkey_toggle_boost` (`3`) in the input config, so they can be rebound to
combinations or controller buttons in the launcher's Hotkeys dialog like every
other hotkey. All commands are sent on a detached thread; a dead NeuralScreen
never stalls a frame.

NeuralScreen is stopped (`quit`, then terminate after 5 s) when the game window
closes and before a relaunch.

## Updating across emulator updates

The integration touches a small number of places; when merging an upstream
update, rebase this branch on it (do not merge in place) and keep:

- `src/core/neural_screen.{h,cpp}` (standalone module) and its two lines in
  `CMakeLists.txt` (CORE sources, `ws2_32`)
- the `NeuralScreen::StartForGameWindow` / `Stop` calls in `emulator.cpp`
- the three hotkey cases in `input_handler` and `sdl_window.cpp`
- `ENABLE_FSR_FRAMEGEN` left `OFF` (NeuralScreen owns frame generation)
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
