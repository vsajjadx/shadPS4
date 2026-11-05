# Native neural pass inside the emulator (plan)

Goal: no NeuralScreen process, no window capture, no overlay. The neural pass runs
inside the emulator's presenter, on the game image only, **after** frame generation,
so real and generated frames both get it.

## Why it is feasible

`nvngx_dlssnr.dll` serves a call only when the path of the *calling module* contains
`nvngx.dll` (see NeuralScreen `native/ns_forwarder.cpp`). The process name is not
checked. So shadPS4.exe can load the small forwarder module
`nvngx.dll_ns-forwarder.dll` and route Init_Ext / CreateFeature(18) / Evaluate /
Release through it, in-process.

## Why it will not fix FPS by itself

NeuralScreen's own measurement (RTX 5070 Ti): NGX evaluate 16.6 ms of a 17.9 ms
frame; capture + upload + present together 1.3 ms. Going native removes the 1.3 ms
and the overlay window; the NGX cost stays. FPS work is therefore separate
(Boost / work_scale, an FPS cap for the neural loop).

## Pieces

| Piece | Notes |
| --- | --- |
| Loader | Load forwarder + `nvngx_dlssnr.dll`, resolve the four NGX entry points. Windows only. |
| D3D12 device | Created on the same adapter as the Vulkan device (match LUID via `VkPhysicalDeviceIDProperties`). NGX is D3D12-only for this feature. |
| Interop | Vulkan -> D3D12 shared image (`VK_KHR_external_memory_win32`, already exposed by the driver) plus a shared timeline semaphore / D3D12 fence. |
| Inputs | The feature wants colour plus RG16F motion. NeuralScreen builds motion from NVOFA guides; open question whether the emulator's FSR 3.1 optical-flow output can be reused. |
| Hook | In `Presenter`, after the generated image exists and before the swapchain blit (`PresentImage`), for both real and generated frames. |
| Settings | Existing `neural_screen.json` keys stay; `Enabled`, `Boost`, `NetworkResolutionPercent`, `Passes`, `Profile`, `Model`, tuning. Hotkeys 2 / 3 keep their meaning. |

## Milestones

1. **Load + init (log only)**: forwarder loads, D3D12 device on the right adapter,
   `NVSDK_NGX_D3D12_Init_Ext`, `CreateFeature(18)` succeed; result logged. Behind a
   CMake option that is OFF by default.
2. **Interop**: export the final Vulkan image, import in D3D12, evaluate one frame,
   copy the result back, fences in both directions. Single frame, DLSS5 hotkey only.
3. **Pipeline hook**: run after FG for every presented frame; drop WGC/overlay use.
4. **Cleanup**: remove the external-process start/stop and the control port.

Needs real hardware to verify each milestone (RTX 30 target); none of this can be
exercised on CI, only compiled.
