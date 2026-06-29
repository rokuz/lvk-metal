# lvk-metal

[![CI](https://github.com/rokuz/lvk-metal/actions/workflows/ci.yml/badge.svg)](https://github.com/rokuz/lvk-metal/actions/workflows/ci.yml)

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
| **RenderToCubeMap** — render a scene into a cubemap, then sample it | <img src="tests/references/RenderToCubeMap.png" width="320"> |
| **RenderToCubeMapSinglePass** — the same in one layered pass | <img src="tests/references/RenderToCubeMapSinglePass.png" width="320"> |
| **ImGuiDemo** — Dear ImGui integration | <img src="tests/references/ImGuiDemo.png" width="320"> |
| **SolarSystem** — textured planets, asteroid belt and HDR skybox | <img src="tests/references/SolarSystem.png" width="320"> |
| **Bistro** — large exterior scene: shadow map, IBL skybox, bindless materials, MSAA and a compute post-process | <img src="tests/references/Bistro.png" width="320"> |

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
| Tessellation (`smTesc` / `smTese`, patch control points) | ⬜ Not implemented | Metal uses a different model (compute-generated factors + post-tessellation vertex function) |
| Mesh / task shaders | ⬜ Not implemented | Metal has object/mesh shaders; not wired yet |
| Ray tracing — pipelines, acceleration structures, `cmdTraceRays`, TLAS | ⬜ Not implemented | Fully implemented in LVK (the `RTX_*` samples); Metal supports RT (`MTLAccelerationStructure`, intersectors), not wired here yet |
| Indirect draws (`cmdDrawIndirect*`) | ⬜ Not implemented | Metal supports indirect draws / indirect command buffers |
| Buffer copy / fill / update | ⬜ Not implemented | Metal blit encoder |
| Image copy / clear / mipmap generation | ⬜ Not implemented | Metal blit encoder |
| Texture views | ⬜ Not implemented | `newTextureView` |
| Query pools / timestamps | ⬜ Not implemented | Metal counter sampling |
| YUV textures | ⬜ Not implemented | Metal biplanar YUV formats |
| Async-compute queue | ⬜ Not implemented | Single queue today; Metal allows several |
| HDR / EDR swapchain | ⬜ Not implemented | Only sRGB gamma is wired; EDR not exposed |
| Present-mode selection | ⬜ Not implemented | vsync (FIFO) only, via `displaySyncEnabled` |
| Render-pass subpasses (`cmdNextSubpass`, input attachments) | ⬜ Not implemented | Metal favors single-pass tile memory / programmable blending |
| SPIR-V shader ingestion | ⛔ Unsupported | Deliberate — author in MSL or Slang |
| Binding vertex buffers / vertex input | ⛔ Unsupported | By design (won't be added) — bindless vertex pulling via GPU address |
| Multiview (`viewMask`, `layerCount`) | ⛔ Unsupported | By design (won't be added); Metal could express it via vertex amplification |
| Geometry shaders | ⛔ Unsupported | Metal has no geometry stage — use mesh shaders / vertex amplification |
| Minimum sample shading | ⛔ Unsupported | No Metal API; per-sample execution is shader-driven (`[[sample_id]]`) |

## License

[MIT](LICENSE.md) © 2026 Roman Kuznetsov.

Bundled/derived dependencies keep their own licenses (lightweightvk, metal-cpp, GLFW, ldrutils etc).
