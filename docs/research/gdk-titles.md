# GDK titles: Xbox One GDK and Xbox Series X|S

Notes from getting a Series program built from public sample source to draw and take input,
and from starting one retail Xbox One GDK title. The code is in `src/runtime/modules`.

A GDK title is the same kind of program on both consoles: a Win32 executable with `WinMain`
that imports API sets, the console's Direct3D 12 (`d3d12_x.dll` or `d3d12_xs.dll`),
`GameInput.dll`, `xaudio2_9.dll`, `xmem.dll`, and reaches the Gaming Runtime through
`xgameruntime.dll`. There is no `kernelx.dll` and no WinRT.

## Test programs

- **A Series sample.** The SimpleTriangle sample of the public GDK samples (MIT), built with
  the June 2020 kit installed on this machine and the Visual Studio 2022 compiler. The kit's
  MSBuild platform is for older Visual Studio versions, so it was built by hand:
  `cl` with the kit's `gameKit\Include`, `gameKit\Include\Scarlett` and the Gaming Runtime's
  include folder first on the include path, `/D_GAMING_XBOX /D_GAMING_XBOX_SCARLETT
  /DWINAPI_FAMILY=WINAPI_FAMILY_GAMES /D__WRL_NO_DEFAULT_LIB__ /D__WRL_CLASSIC_COM_STRICT__`,
  and linked with `/NODEFAULTLIB:kernel32.lib` (and the other desktop defaults) against
  `pixevt.lib d3d12_xs.lib xgameplatform.lib xgameruntime.lib`. The shaders were compiled
  with the kit's `bin\Scarlett\dxc.exe` (`-T vs_6_0`, `-T ps_6_0`). The telemetry call of the
  sample was removed; nothing else was changed.
- **A retail Xbox One GDK title** (a racing game, unpacked, with the console's `System32`
  next to it in `EmbeddedXvd`). Its `MicrosoftGame.config` says `TargetDeviceFamily="XboxOne"`.

No retail Series title was at hand.

## What a GDK executable imports

The sample imports 33 DLLs: 29 API sets, the three CRT DLLs, `d3d12_xs.dll`
(`D3D12XboxCreateDevice` only) and `GameInput.dll` (ordinal 1, `GameInputCreate`). The
retail title adds `xmem.dll`, `xaudio2_9.dll` by ordinal (1 `XAudio2Create`, 2
`CreateAudioReverb`, 5 and 6 the X3DAudio functions), `dxcompiler_x.dll`, `xg_x.dll`,
`winhttp.dll`, `bcrypt.dll`, `crypt32.dll` and its own `XCurl.dll`.

`xgameruntime.dll` is in no import table. The title's half of the Gaming Runtime calls
`LoadLibrary("xgameruntime.dll")` and `GetProcAddress` for `QueryApiImpl`,
`InitializeApiImpl`, `InitializeApiImplEx`, `UninitializeApiImpl` and `XErrorReport`. On a
Windows 11 host a DLL of that name exists in System32 (the PC runtime), so the title's
`LoadLibrary` has to be answered by us before the host sees it.

## The Gaming Runtime

Every GDK function (`XUserGetId`, `XTaskQueueCreate`...) is a few instructions in the
title: `QueryApiImpl(api, interface, &table)`, then a call of one slot of the table with
the table first and the function's own arguments after it, then `Release` (slot 2).

The slot of every function was read from the disassembly of the public static library
(`xgameruntime.lib`, which keeps its symbols) of two kits, June 2020 and October 2026:
309 and 416 functions in 24 APIs. What that shows:

- An API is one table that only grows. A later kit asks for the same API under another
  interface ID and finds the older functions in the same slots. So one table per API
  answers for every interface ID.
- Two APIs have a second, separate table under one interface ID: `XUserGetGamertag` with a
  component argument (slot 3 of its own table; slot 15 of the XUser table is taken to be
  the first kits' form without the component), and the experimental streaming functions.
- XPackage is the one exception to "only grows": `XPackageEnumeratePackages`,
  `XPackageRegisterPackageInstalled` and `XPackageEnumerateFeatures` moved between the two
  kits. The method table lists both places.
- Slot 8 of the async table is in neither library. It sits between `XAsyncBegin` and
  `XAsyncSchedule` and is taken to be `XAsyncBeginAlloc`. Nothing has called it yet.
- The retail title asks for XPackage under an interface ID neither kit has, and calls slot
  36, which neither names.

`gameruntime/slots.cpp` is that table. Slots nothing implements print the function's name
the first time they are called and return `E_NOTIMPL`.

Implemented: task queues (all four dispatch modes, delayed callbacks, waiters, monitors,
termination, the process queue), XAsync (providers of the title's as well as ours), XUser
(one signed-in user from Options > Console), XGameSave and XGameSaveFiles (files under the
title's save folder, the same store ERA connected storage uses), and the small ones: title
id from `MicrosoftGame.config`, console and sandbox ids, package identity and mount path,
persistent local storage, connectivity hint, feature availability, error callbacks.
Events (user change, invites, connectivity, package installs) hand out a token and are
never raised. Xbox Live tokens fail. XStore, XGameUi, XAppCapture, speech and the
installation monitors are not implemented.

Only the task queue, the title id and the save provider have been called by a title so
far. The rest follows the public headers and is untested.

`XSystemGetDeviceType` does not go through a table: it reads `ConsoleMode` under
`HKLM\Software\Microsoft\Windows NT\CurrentVersion\OEM` (0x50 is an Xbox One X, 0x64 a
Series X; a missing key means a PC). The retail title indexes a jump table with the result
and crashes on "PC", so the registry functions answer for that key.

## Direct3D 12 on Xbox Series

The Series header is the Xbox One GDK header with more at the end. Compared table by
table (the function tables of both console headers, 62 in the Series one):

- Device: identical up to slot 82. Xbox One then has four console methods (83 to 86) before
  `ID3D12Device1` to 3; Series has `ID3D12Device1` to 8 right away (83 to 112, with the
  console's `StoreDXRStateObjectDescX` at 104 and three more console methods at 113 to 115).
- Command list: identical up to 97. 98 and 99 are `DispatchMeshX` and `DriverHintX` on
  Series (two prefetch methods on Xbox One), 103 and 104 are batch binning methods. The
  later list interfaces start at 105 (104 on Xbox One) and go to 128 on Series, with console
  methods at 122, 123, 126 and 128.
- Command queue, heaps, fences, allocators: identical. `PresentX` is queue slot 30 on both.
- Resource and pipeline state have two more console methods each on Series.
- Interface IDs are the desktop's, except `ID3D12Device8` and
  `ID3D12GraphicsCommandList5` and 6, which had other IDs in the June 2020 kit.

Object layouts (from `offsetof` in a program built against each header):

| | Xbox One GDK | Series |
|---|---|---|
| Command list size | 0x360 | 0x560 |
| Write pointer and limits | +0x38 | +0x18 |
| Device flags byte, null buffer address | +0x18, +0x10 | the same |
| Descriptor sizes | 32, 16, 112, 128 | the same |

Inline methods are the same set on both, with one addition on Series: `SetPipelineState`
copies 32 bytes from the pipeline state object (+0x10) into the command list (+0x540)
before it looks at the write pointer. The list proxy is therefore 0x600 bytes; what it
copies from the host's pipeline state is never read back.

Titles built against the GDK header call methods as `mov rax, [rax + slot * 8]` then
`call rax` (or `call rdx`), not `call [rax + slot * 8]`. The device's check of who called
a slot looks for that load in the 64 bytes before the call. Without it the sample crashed
inside the host's runtime on its first console method.

### Showing frames

There is no swap chain. The title creates its back buffers as committed resources, gives
one to `ID3D12CommandQueue::PresentX` in a `D3D12XBOX_PRESENT_PLANE_PARAMETERS` (0x48
bytes; the resource array is at +0x10), and paces itself with the device's
`SetFrameIntervalX`, `ScheduleFrameEventX` and `WaitFrameEventX` (slots 78 to 80).

Here a swap chain of the size and format of what is presented is made on the title
surface, and each `PresentX` copies the title's resource into its back buffer on a list of
ours and presents it, with one display refresh per 16.7 ms of the title's frame interval.
`WaitFrameEventX` only waits when the title did not present since the last wait. The
title's own window is created but never shown.

On Linux, under Proton, a UI that flickers by layers is gone with RenderDoc's layer loaded:
see `linux-renderdoc-flicker.md`.

### Shaders

The sample's shaders, from the kit's compiler, are DXIL containers with the desktop's
chunks (`SFI0 ISG1 OSG1 PSV0 RTS0 STAT HASH DXIL`). The host takes them as they are.

The retail Xbox One GDK title's are not usable. Its containers have the same chunk names,
but the `DXIL` chunk holds a program header followed by `XDAT` data instead of bitcode,
and the module in `STAT` has declarations only (the entry point is declared, not defined)
with the metadata `__XBOX_STRIP_DXIL = 1`. The code is only there as compiled for the
console's GPU. The host answers `E_OUTOFMEMORY` to every pipeline made from them (123 in
the first seconds). Running such a title needs a translator from the console's GPU code to
something the host compiles, which does not exist here. Whether retail Series titles ship
their shaders the same way was not checked: there was none to look at.

## Results

- The Series sample runs with `xvboot exec <folder>`: 3840x2160 frames; the picture at
  frame 120 (`XV_D3D_PROBE`) is the sample's triangle on its grey background; the scripted
  View button (`XV_INPUT_PRESS=view@2`) ends the program, as the sample's code does on that
  button. `xvboot bind` shows no missing import. Opening it from the window, and a real
  gamepad or the keyboard, were not tried.
- Three more samples of the same repository, built the same way, present frames with
  `xvboot exec`: SimpleTexture (the picture at frame 120 is its photograph), Gamepad and
  SimpleMeshShader (non-black averages; their pictures were not looked at). They needed
  three things the triangle did not: the caller check has to look further back than 64
  bytes for the load of the slot (160 now); `D3D12XboxCreateDevice` has to answer the
  kit's `ID3D12Device8` interface ID; and `CreatePipelineState` gets a stream in which the
  Series kit numbers the amplification and mesh shader subobjects 23 and 24 (the desktop
  has them at 24 and 25) and adds console subobjects from 25 on, which are cut off.
  SimplePlaySound needed `ApuAlloc` and `ApuFree` of `AcpHal.dll` on Series as well, and
  COM initialized on its thread: the host's XAudio2 makes its voices through COM, which a
  console title never starts. It presents frames; whether it is heard was not checked (the
  runs were muted).
- The retail Xbox One GDK title starts, initializes the Gaming Runtime, audio and the
  device, loads its packs, and stops at its first pipelines for the reason above. It never
  presents a frame.
- Regression titles after the Direct3D 12 layer, placement textures, console memory and
  the Win32 overrides moved to `runtime/modules`, 1000 frames each with `XV_D3D_PROBE`:
  four present a non-black back buffer. The fifth (the Frostbite one) gives an average of
  1.1 0.0 1.1 at frames 1000 and 3000, three runs. Two logs from before this work, both
  runs from the window, differ in one thing: the one with 1.1 0.0 1.1 has no input, the
  one with 9.1 10.9 12.4 has a gamepad button reaching the title. The `exec` logs are
  otherwise line for line what those are, so nothing points at a change, but without input
  that title is not a non-black check. It was not run again from the window with a button.

## The samples in a batch

66 samples of the public repository have a Series configuration. Built the same way, 40
compile and 26 do not (why was not looked at for each; the first ones miss headers of kits
the build script does not set up). Each of the 40 was run with `xvboot exec` for 12
seconds and `XV_D3D_PROBE=120`, and the average of the back buffer read. Pictures were
looked at for SimpleTriangle, SimpleTexture, SimpleMSAA and SmokeSimulation only.

36 present frames with a non-black average. What the ones that did not at first needed:

- ExecuteIndirect and ComputeParticles bind descriptor tables of a heap they never gave to
  `SetDescriptorHeaps`. A console GPU handle is the descriptor's address and is good
  whatever heaps are set; the host reads a table from the heap that is bound, and its
  driver faults otherwise. The heap a table's handle is in is now bound first. It replaces
  the bound heap of its kind, so tables of two view heaps for one draw still cannot work.
- ExecuteIndirect also times itself with `Write64BitValueBottomOfPipeX` (list slot 70),
  which has the console's GPU write a value or its clock to the title's memory when it gets
  there. Here the write happens when the list is recorded, with the host's clock: the
  times are CPU times.
- SimpleMSAA was broken by `XV_D3D_PROBE` itself, not by the layer: the probe read every
  render target back as if it were in the render target state, and tried to copy a
  multisampled one to a buffer, after which the host removed the device. It now uses the
  state the title's last barrier left and skips multisampled targets. The same mistake
  was behind the one barrier message every sample got from the host's debug layer.
- HlslCompile and SimpleSamplerFeedback missed shader files the build script did not make
  (three come from a tool of the sample's own, one needs `-D__XBOX_DISABLE_PRECOMPILE=1`
  with the June 2020 compiler). Compiled by hand with the kit's `dxc`.
- The front panel samples import `XFrontPanelDisplay.dll`: `XFrontPanelIsSupported`
  answers false and the rest `E_NOTIMPL`. mDNS imports `ws2_32.dll` by ordinal, which is
  forwarded to the host's.
- SmokeSimulation's nearly black average (4.5) is its picture: smoke in a dark box.

Four do not run:

- PointSprites makes pipelines with the console's own topologies
  (`D3D12XBOX_PRIMITIVE_TOPOLOGY_TYPE_QUAD` and `_RECT`), which the host turns down. Quad
  and rectangle lists would have to be drawn as triangles.
- SimpleSamplerFeedback gets as far as its first frame. Found on the way: the first kit
  numbers the two feedback formats 187 and 188 (189 and 190 on the desktop); the console's
  `CreateSamplerFeedbackUnorderedAccessViewX` (device 111) has a view description before
  the handle; the console takes a mip region as large as the texture and one mip, where
  the host wants at most half the texture and as many mips as the paired texture (checked
  with a small program on the host: a map with one mip paired with a ten-mip texture
  removes the device). The sample then reads the map with `CopyTextureRegion` straight
  into a buffer, in a fixed point form of the console's; the host only decodes a map with
  `ResolveSubresourceRegion`. Such a copy is now a resolve into a texture of ours and a copy
  of that one; the bytes are whole mip levels, so a title that asks for the console's form
  (a footprint of `R8_TYPELESS`, five bits of level and three of fraction) reads an eighth
  of the level. It also clears the map through a CPU handle of a shader-visible heap, which
  the host cannot read: the clear gets a view of the map in a heap of ours. The view the
  title makes was not marked as the host's in the heap's shadow, and was written over
  before the first draw. The sample draws its picture and says mip 0, which is what its
  512-pixel texture on a quad that large asks for; a level other than 0 was not seen.
- FrontPanelDemo and FrontPanelDolphin ask for the panel's screen size without asking
  whether there is a panel, and throw on the failure. What a retail console answers there
  is not known.

The regression titles were run again after these changes, 1000 frames each: the Unreal
one 184 117 41 twice, the two Direct3D 11 ones and the first-kit one as before (97 115
122, 76 86 103, 69 125 162), the Frostbite one 1.1 0.0 1.1 as before. One earlier run of
the Unreal title, on a build with the heap binding already in, gave 0.7 0.3 0.0 with
fewer draws; the next run of the same build gave 184 117 41. It did not come back in
three more runs and was not explained.

From the window a title runs in a process of its own that draws into the window's
surface, so that File > Close Title ends it and the window stays; closed that way the
window goes straight back to the library. Menus and popups are cut out of the title's
surface instead of hiding it. The child process was tried once from the window, with the
triangle sample; the cut-outs, a second title after a first, and input in that mode were
not tried.

## Not done

- Console shader code (see above). This is what stands between the retail GDK title and a
  picture.
- Texture layouts on Series: `xg_xs.dll` has another interface than `xg_x.dll`, and
  placement textures are read as linear there.
- `CreatePlacedShaderResourceViewX` (device 46) and `GetDefaultMSAAParametersX` (59), which
  the retail title calls, and the page mapping methods of the queue.
- Raytracing and variable rate shading go to the host's methods of the same name without
  their arguments being looked at; nothing has called them. `CheckFeatureSupport` is the
  host's answer, unchanged. The fraction of a sampler feedback map read back, and the
  console's quad topology (see the samples above).
- `DispatchMeshX` only in its plain form.
- `dstorage_xs.dll`, `GameInput` keyboards and mice, rumble, XStore, XGameUi.
- DXGI outputs past the desktop's methods (`GetRefreshRate` and the like).
- Titles built with control flow guard call through a dispatch function; the device's
  caller check does not know that form.
- The shader compiler is the host's `dxcompiler.dll` when one is next to `xvalkyrie.exe`;
  none is shipped or fetched by the build.
