# lvk-metal

[![CI](https://github.com/rokuz/lvk-metal/actions/workflows/ci.yml/badge.svg)](https://github.com/rokuz/lvk-metal/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/rokuz/lvk-metal/graph/badge.svg)](https://codecov.io/gh/rokuz/lvk-metal)

A thin, bindless-first **Metal 4** graphics wrapper built on [`metal-cpp`](https://developer.apple.com/metal/cpp/) and designed as an addon to [LightweightVK](https://github.com/corporateshark/lightweightvk) (lvk).

## Why

`lvk-metal` is **not** a full LVK backend, and it intentionally does not emulate Vulkan on top of Metal — for that, use [MoltenVK](https://github.com/KhronosGroup/MoltenVK) or [KosmicKrisp](https://docs.mesa3d.org/drivers/kosmickrisp.html).

The goal is a Metal API that is as comfortable as LVK's Vulkan API and stays close to LVK in shape, so the same rendering code can stay cross-platform. That parity takes deliberate effort: Metal 4 has moved much closer to Vulkan, but not in every aspect — and where Metal offers unique functionality, this wrapper keeps it available rather than hiding it behind a lowest-common-denominator layer.

SPIR-V is unsupported by design: use MSL for the few Metal-specific cases and [Slang](https://shader-slang.org/) (the preferred path) for the majority of shaders.

## Requirements

- macOS with a **Metal 4**-capable GPU (Apple7 family or newer).
- Xcode Command Line Tools (clang with C++20).
- [CMake](https://cmake.org/) ≥ 3.21.
- Python 3 — used by the dependency bootstrapper and by lvk's own dependency
  deployment.
- lvk as a dependency (bootstrapped automatically, or supplied via a path).

## Building

```sh
# Dependencies are fetched automatically on first configure (bootstrap.py),
# and lvk deploys its own deps too.
cmake -S . -B build -G Ninja # or -G Xcode
cmake --build build
```

Run the sample:

```sh
./build/samples/HelloTriangle/HelloTriangle
```

### CMake options

| Option | Default | Description |
| --- | --- | --- |
| `LVK_METAL_WITH_SAMPLES` | `ON` | Build the example apps. |
| `LVK_METAL_WITH_SAMPLES_SLANG` | `OFF` | Author sample shaders in [Slang](https://shader-slang.org/), compiled to MSL at runtime instead of inline MSL. Pulls in the bundled `slang` dependency. |
| `LVK_METAL_LVK_DIR` | `3party/src/lightweightvk` | Path to the lightweightvk source tree. |

By default the bundled lvk (`LVK_METAL_LVK_DIR == 3party/src/lightweightvk`) is configured and built for you. If you point `LVK_METAL_LVK_DIR` at an external checkout, `lvk-metal` assumes that project already defines the `LVKLibrary` target before it is added — the bundled lvk options are not applied in that case.

## Samples

Built by default (`LVK_METAL_WITH_SAMPLES=ON`) into `build/samples/<Name>/<Name>`. The screenshots below are the committed test references, captured headless at 1024×768 by `tests/screenshot_test.py`.

| Demo | Screenshot |
| --- | --- |
| **HelloTriangle** — minimal triangle | <img src="tests/references/HelloTriangle.png" width="320"> |
| **TransferOps** — blit-encoder buffer copy/fill/update + image clear/copy, all feeding one textured quad | <img src="tests/references/TransferOps.png" width="320"> |
| **IndirectDraws** — GPU-driven multi-draw indirect, one row per method: `cmdDrawIndirect` (top, compute-filled args + per-draw primitive types), `cmdDrawIndexedIndirect` (middle), `cmdDrawMeshTasksIndirect` (bottom) | <img src="tests/references/IndirectDraws.png" width="320"> |
| **MeshShaders** — a triangle emitted by an object + mesh shader | <img src="tests/references/MeshShaders.png" width="320"> |
| **MeshShaderFireworks** — GPU-billboarded particle fireworks via a mesh shader, additive blending | <img src="tests/references/MeshShaderFireworks.png" width="320"> |
| **RenderToCubeMap** — render a scene into a cubemap, then sample it | <img src="tests/references/RenderToCubeMap.png" width="320"> |
| **RenderToCubeMapSinglePass** — the same in one layered pass | <img src="tests/references/RenderToCubeMapSinglePass.png" width="320"> |
| **TextureView** — one 2D-array texture viewed per layer via `createTextureView`; the last column is a swizzled view (R↔B) of the red layer | <img src="tests/references/TextureView.png" width="320"> |
| **QueryPool** — wraps a pass in GPU timestamps (`cmdWriteTimestamp` / `getQueryPoolResults`) and logs the measured GPU time | <img src="tests/references/QueryPool.png" width="320"> |
| **YUV** — samples `YUV_NV12` and `YUV_420p` textures (BT.709 decode in-shader); needs lvk's `deploy_content.py` (igl-samples), so it is not part of the automated screenshot suite | <img src="tests/references/YUV.png" width="320"> |
| **SwapchainHDR** — renders into an HDR10 (PQ, `RGB10A2`) swapchain; the screenshot capture unpacks the packed 10-bit drawable to 8-bit RGB | <img src="tests/references/SwapchainHDR.png" width="320"> |
| **OmniShadows** — omnidirectional point-light shadows: the scene is rendered into a cube shadow map in **one multiview pass** (vertex amplification), then lit with 9-tap PCF; an ImGui panel shows the 6 cube faces via `createTextureView` | <img src="tests/references/OmniShadows.png" width="320"> |
| **LocalRead** — single-pass deferred: a parallax-occlusion-mapped cube (bindless Iron_Bars base-color/normal/height) fills a G-buffer in **memoryless** attachments, then is lit in the same pass by reading them back with **framebuffer fetch** (`[[color(n)]]` / Slang `SubpassInput`) — the Metal analog of Vulkan's `cmdNextSubpass` + input attachments | <img src="tests/references/LocalRead.png" width="320"> |
| **LocalReadTile** — the same deferred scene, but the lighting reads the G-buffer from the tile **imageblock** in a Metal-only **tile shader** (`cmdBindTilePipeline` + `cmdDispatchTile`) instead of a full-screen pass; renders pixel-identically to `LocalRead` | <img src="tests/references/LocalReadTile.png" width="320"> |
| **ImGuiDemo** — Dear ImGui integration | <img src="tests/references/ImGuiDemo.png" width="320"> |
| **SolarSystem** — textured planets, asteroid belt and HDR skybox | <img src="tests/references/SolarSystem.png" width="320"> |
| **Bistro** — large exterior scene: shadow map, IBL skybox, bindless materials, MSAA and a compute post-process | <img src="tests/references/Bistro.png" width="320"> |
| **RTX_Hello** — inline RayQuery in a compute kernel: a barycentric-shaded icosahedron over an animated checkerboard "miss" background | <img src="tests/references/RTX_Hello.png" width="320"> |
| **RTX_Textures** — two BLAS instances with per-instance hit behavior selected by `instance_id` (triplanar-textured vs barycentric), reading the vertex/index buffers by `primitive_id` | <img src="tests/references/RTX_Textures.png" width="320"> |
| **RTX_Bistro** — the full Bistro traced in one compute kernel: primary + shadow rays, octahedral normals, bindless materials | <img src="tests/references/RTX_Bistro.png" width="320"> |
| **RTX_AO** — Bistro lit entirely by ray tracing in a compute kernel: a primary ray per pixel, then ray-traced ambient occlusion cached in a spatial hash (Gautron 2020) plus ray-traced shadows, with an ImGui control panel | <img src="tests/references/RTX_AO.png" width="320"> |

## Compatibility with LVK

`lvk-metal` tracks LVK's bindless-first API surface. Where Metal 4 differs from Vulkan, a feature is either mapped, deferred, or intentionally left out. ✅ **Implemented** = works today; ⬜ **Not implemented** = expressible in Metal 4 but not wired yet; ⛔ **Unsupported** = Metal 4 has no equivalent (or it is excluded by design).

| Feature | Status | Comment |
| --- | --- | --- |
| Bindless textures / samplers / buffers | ✅ Implemented | Metal 4 argument tables + a residency set; indices match LVK's bindless model |
| Dynamic rendering (`cmdBeginRendering` / `cmdEndRendering`) | ✅ Implemented | Native to Metal render command encoders; LVK render-pass/framebuffer descriptors map directly |
| Swapchain + present | ✅ Implemented | `CAMetalLayer`, multi-buffered |
| Buffers — create / upload / download / map / GPU address | ✅ Implemented | Unified memory; `flushMappedMemory` is a coherent no-op |
| Textures — 2D / 3D / Cube / 2D-array | ✅ Implemented | |
| MSAA + resolve | ✅ Implemented | Color and depth/stencil resolve |
| Depth & stencil state | ✅ Implemented | Dynamic depth + pipeline stencil combined into a cached `MTLDepthStencilState`; `cmdSetStencilRef` |
| Samplers incl. shadow/comparison | ✅ Implemented | |
| Render pipelines — blend, cull, winding, polygon mode, alpha-to-coverage | ✅ Implemented | |
| Compute pipelines + dispatch (+ indirect dispatch) | ✅ Implemented | |
| Push constants | ✅ Implemented | Constants ring bound through the argument table |
| Deferred resource destruction | ✅ Implemented | Mirrors LVK's deferred-task queue (freed once the GPU is done) |
| GPU capture + debug labels | ✅ Implemented | |
| MSL shaders | ✅ Implemented | Inline source or precompiled `metallib` |
| Slang → MSL | ✅ Implemented | Compiled at runtime (sample-side `SlangRuntime`) |
| Specialization constants | ✅ Implemented | Mapped to Metal function constants (`[[function_constant(N)]]`); types resolved via library reflection, `constantId` == the MSL index |
| Layout transitions / barriers (`cmdTransitionTo*`) | ✅ Implemented | No-ops — Metal auto-tracks hazards (residency set + encoder barriers) |
| Mesh / task shaders | ✅ Implemented | Mapped to Metal object/mesh shaders + `drawMeshThreadgroups`; threadgroup size via a `clang::annotate("lvk_numthreads(...)")` attribute (MSL) or Slang reflection. See the `MeshShaders` and `MeshShaderFireworks` samples |
| Indirect draws (`cmdDrawIndirect`, `cmdDrawIndexedIndirect`, `cmdDrawMeshTasksIndirect`) | ✅ Implemented | True GPU multi-draw with **no CPU loop**: pass the compute-filled args buffer (`BufferUsageBits_Indirect`) as a `cmdBeginRendering` dependency, and the wrapper runs an internal MSL kernel that encodes a `MTLIndirectCommandBuffer` from it; the draw issues the range with one `executeCommandsInBuffer` (`drawCount==1` skips the ICB and uses a native indirect draw). Per-draw primitive type, the index buffer (indexed), and mesh threadgroup sizes are supplied via `IMetalContext::setIndirectBufferMetadata`. See the `IndirectDraws` sample. The GPU-count variants (`cmdDrawIndexedIndirectCount` / `cmdDrawMeshTasksIndirectCount`) are ⛔ unsupported — a GPU-side count buffer can't be resolved at the `cmdBeginRendering` encode point; the validation layer warns and they no-op |
| Buffer copy / fill / update | ✅ Implemented | `cmdCopyBuffer` / `cmdFillBuffer` / `cmdUpdateBuffer` via the MTL4 compute (blit) encoder; `cmdUpdateBuffer` stages through a per-frame ring (Metal 4 has no inline `vkCmdUpdateBuffer`). `cmdFillBuffer` fills the low byte of `data` (Metal fills a byte). See the `TransferOps` sample |
| Image copy / clear / mipmap generation | ✅ Implemented | `cmdCopyImage` (`copyFromTexture`), `cmdGenerateMipmap` (`generateMipmaps`), and `generateMipmaps` in `createTexture`; `cmdClearColorImage` via a clear-only render pass (needs an attachment-capable texture). Mipmaps exercised by `SolarSystem`/`Bistro`, copy/clear by `TransferOps` |
| Ray tracing — acceleration structures, TLAS/BLAS, inline RayQuery | ✅ Implemented | BLAS/TLAS via `createAccelerationStructure` (MTL4 primitive/instance descriptors, built on a compute encoder); the TLAS is bindless (`ArgumentKind::AccelStructs`, indexed like a texture). Metal has no ray-tracing *pipeline* (no SBT / `DispatchRays` / raygen-miss-hit groups), so RT is expressed as **inline `metal::raytracing::intersector`** inside a compute or fragment shader; `cmdTraceRays` aliases `cmdDispatch`. See the `RTX_*` samples |
| Texture views | ✅ Implemented | `createTextureView` → `MTL::Texture::newTextureView` (type / mip-level range / slice range / component swizzle); the view takes its own bindless slot sharing the parent's storage. Textures are created with `MTLTextureUsagePixelFormatView`. See the `TextureView` sample |
| Query pools / timestamps | ✅ Implemented | GPU timestamps via an MTL4 `CounterHeap` (`CounterHeapTypeTimestamp`); `cmdWriteTimestamp` → `writeTimestampIntoHeap` (or the encoder's `writeTimestamp` inside a pass), `getQueryPoolResults` resolves the heap CPU-side, `getTimestampPeriodToMs` = `1000 / queryTimestampFrequency()`. See the `QueryPool` sample |
| YUV textures | ✅ Implemented | `Format_YUV_NV12` / `Format_YUV_420p` become a two-plane Metal texture (Y = `R8Unorm`, chroma = `RG8Unorm` half-res, 420p's Cb/Cr interleaved on upload); the chroma plane lives in a parallel bindless heap indexed by the same handle, and `textureBindlessYUV` does BT.709 limited-range YCbCr→RGB in-shader. See the `YUV` sample |
| Async-compute queue | ⬜ Not implemented | Single queue today; Metal allows several |
| HDR / EDR swapchain | ✅ Implemented | `ContextConfig::swapchainRequestedColorSpace` maps to the `CAMetalLayer` pixel format + `CGColorSpace` + `wantsExtendedDynamicRangeContent`: HDR10→`RGB10A2Unorm`/PQ, extended-linear→`RGBA16Float`/extended-linear-sRGB, BT.709; `getSwapchainColorSpace` reports it. See the `SwapchainHDR` sample |
| Multiview (`viewMask`, `layerCount`) | ✅ Implemented | Layered single-pass rendering via Metal **vertex amplification**: a non-zero `RenderPass::viewMask` drives `setVertexAmplificationCount` + view mappings + `renderTargetArrayLength`, and the vertex shader selects the view with `[[amplification_id]]` (MSL) / `SV_ViewID` (Slang). Per-pipeline amplification capacity comes from the Metal-only `ShaderModuleMetadata::viewCount`. Used for the cube shadow map in the `OmniShadows` sample. Stereo/VR output is out of scope |
| Render-pass subpasses (`cmdNextSubpass`, input attachments) | ✅ Implemented | Metal has no Vulkan-style subpasses; local reads happen inside one pass. Input attachments map to **framebuffer fetch** — a fragment reads its own attachments via `[[color(n)]]` (MSL) / `SubpassInput.SubpassLoad()` (Slang, `input_attachment_index` → color location), so `cmdNextSubpass` is a no-op. Memoryless G-buffer attachments keep it all in tile memory. See the `LocalRead` sample |
| Tile shading (imageblock compute) | ✅ Implemented | Metal-only extension for cross-pixel/tile work (e.g. tile-based deferred lighting, per-tile light culling): `IMetalContext::createTileRenderPipeline` + `IMetalCommandBuffer::cmdBindTilePipeline` / `cmdDispatchTile` run a `[[kernel]]` over each tile's imageblock, ordered after prior draws to the same tile (no barrier). See the `LocalReadTile` sample |
| Tessellation pipeline | ⛔ Unsupported | By design (won't be added) — use mesh shaders instead |
| SPIR-V shader ingestion | ⛔ Unsupported |  By design (won't be added) — author in MSL or Slang |
| Binding vertex buffers / vertex input | ⛔ Unsupported | By design (won't be added) — bindless vertex pulling via GPU address |
| Geometry shaders | ⛔ Unsupported | Metal has no geometry stage — use mesh shaders |
| Minimum sample shading | ⛔ Unsupported | No Metal API; per-sample execution is shader-driven (`[[sample_id]]`) |

## License

[MIT](LICENSE.md) © 2026 Roman Kuznetsov.

Bundled/derived dependencies keep their own licenses (lightweightvk, metal-cpp, GLFW, ldrutils etc).
