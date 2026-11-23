# s3-r2 M4a: the XCL graphics device loss

The stock GE-Proton vkd3d has `VKD3D_ENABLE_BREADCRUMBS` compiled out
(`meson.build`: `enable_breadcrumbs = enable_trace`). Rebuilding the worktree's
vkd3d with `meson configure build/vkd3d/build -Denable_trace=true` and copying
`d3d12.dll`/`d3d12core.dll` into `build/linux` makes the breadcrumb report appear.

Reproduction (XCL graphics, menu, 45 s timeout):

```
XV_STATE=$HOME/.cache/xvalker/state-s3-bread4 XV_PREFIX=$HOME/.cache/xvalker/prefix-s3 \
XV_RENDERDOC=0 XV_SHADER_BACKEND=xcl VKD3D_CONFIG=breadcrumbs VKD3D_DEBUG=info \
VKD3D_SHADER_CACHE_PATH=$HOME/.cache/xvalker/cache/vkd3d-shader \
flock ~/.cache/xvalker/gpu.lock timeout 45 tools/xvalker run ~/.cache/xvalker/mc-s3
```

First fault (log `~/.cache/xvalker/state/s3-bread4.log`):

```
vkd3d_breadcrumb_tracer_init: Enabling NV_device_diagnostics_checkpoints breadcrumbs.
vkd3d_breadcrumb_tracer_report_queue_nv: Found pending command list context [340, 341] in executable state, TOP_OF_PIPE marker 1, BOTTOM_OF_PIPE marker 1.
... Tag: SAMPLER heap / Tag: CBV_SRV_UAV heap (the only heaps bound to the draw)
... Command: set_shader_hash hash: 6479c307f3df5b74, stage: 1 (VS); hash: 30cc1baeae636d1a, stage: 10 (PS)
... Command: draw_indexed with 44 indices, 1 instance
```

The translated PS reads (`/tmp/opencode/s3-hlsl/ps-...hlsl`, XCL recompiler output):

```
Texture2D<float4> t10_T = ResourceDescriptorHeap[NonUniformResourceIndex(s0)];
SamplerState t10_S = SamplerDescriptorHeap[NonUniformResourceIndex(s11 & 0xFFFu)];
uint t7_a = s2 + 0u; s0 = mem.Load(t7_a);       // T# word 0 from the title's table
uint t6_a = s8 + 0u; s11 = mem.Load(t6_a + 12u); // S# word 3 from the title's table
```

The title's descriptor memory here is the runtime's low 4 GiB shadow heap, so
those loads produce raw console T#/S# words: T# word 0 is a GPU address (or
zero), not an index into the bound shader-visible PC heap. Direct indexing then
reads an out-of-range `ResourceDescriptorHeap`/`SamplerDescriptorHeap` entry and
the GPU faults during that indexed draw. The host descriptors the runtime makes
from the same words are CBVs, not the SRVs/samplers a sampled image needs.

Fix (M4b): allocate the title's descriptor slots in UMA window 0 (so 32-bit
table addresses still work), store XCL-format words in them (T# word 0 = SRV
index in one shared shader-visible PC heap, S# word 3 [11:0] = sampler index),
and bind that heap for translated draws.
