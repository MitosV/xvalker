# Linux (Proton): the UI flicker and RenderDoc

Notes for running a Series title (Minecraft) with `tools/xvalker` under Proton and
vkd3d-proton.

## The flicker

On Linux the title's UI flickered by layers: backgrounds of buttons over their text, panels
drawn in the wrong order. The title runs without a frame limit (about 240 FPS; on the
console it is held to the display by frame events) and rewrites dynamic buffers while the
GPU still reads them.

With RenderDoc's Vulkan layer loaded the flicker is gone. RenderDoc serializes the work
it intercepts, so the CPU no longer runs ahead of the GPU, and that was enough. The cause
was not found: no code was changed that fixes the ordering itself.

### What is done

`tools/xvalker` always starts the title with RenderDoc's layer, without capturing and
without its overlay:

- `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` loads the layer
  (`/etc/vulkan/implicit_layer.d/renderdoc_capture.json`).
- `tools/renderdoc_quiet.c` is preloaded into Proton's wine only (`LD_PRELOAD`). Once the
  layer is in the process it hides the overlay (`MaskOverlayBits`) and clears the capture
  keys (`SetCaptureKeys(0, 0)`), so F12 and PrtScn do nothing. It never loads RenderDoc
  itself. It is built on demand into the cache folder.
- `XV_RENDERDOC=0` starts without it. If the layer is not installed or the shim does not
  build, `xvalker` says so and starts without it; the flicker then comes back.

This is a workaround, not a fix. A title that runs without RenderDoc is not paced the way
it is with it.

### What the code does without it

`present.cpp` and `device.cpp` signal the title's frame events from the CPU, by clock, and
write and fill GPU addresses on the CPU when a list is recorded (waits on them are
ignored). Nothing a title can ask about its frames waits for the host's GPU. Options to
tell whether it is a race of the CPU and the GPU, each off by default:

| Variable | Does |
|---|---|
| `XV_SYNC_PRESENT=1` | after the host's Present, signals the fence and waits for it on the CPU |
| `XV_FRAME_PACE=1` | frame events at the interval the title asked for, by `QueryPerformanceCounter`, never before the host's fence of the last frame has completed |
| `XV_D3D_SYNC` | waits for the GPU after each `ExecuteCommandLists` |

Whether any of these removes the flicker without RenderDoc has not been checked.

## The user (XUser)

The user of the title (signed in, not a guest, gamertag, `xuid`) only works when the title
is started from the UI (`tools/xvalker gui`, `xvalkyrie.exe`), not with `tools/xvalker run`.
That is what was seen; why was not looked into. The user is made in
`gameruntime/user.cpp` from the profile of Options > Profiles.

`XV_LOCAL_USER=1` gives a made-up local user when there is no profile to take one from: signed
in, not a guest, `xuid` 2535400000000001, local id 1, gamertag `Player` (or `XV_GAMERTAG`),
every privilege allowed, and a token request that fails with `0x80072EFD` so that the title
goes offline. It replaces the profile's xuid and gamertag when set. Not tried with a title.
