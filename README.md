# lvk-metal

A thin, bindless-first **Metal 4** graphics wrapper built on
[`metal-cpp`](https://developer.apple.com/metal/cpp/) and shaped after
[LightweightVK](https://github.com/corporateshark/lightweightvk) (lvk).

`lvk-metal` is an **extensible lvk backend**: it reuses lvk's public value types
so code written against lvk ports over with minimal changes, while exposing the
Metal-only capabilities through its own `IMetalContext` and `IMetalCommandBuffer` interfaces.

Namespace: `lvk::metal`. Public header: `lvk/LVK-Metal.h`.

## Highlights

- **Bindless by default** — global resource/sampler heaps of `MTL::ResourceID`
  plus a GPU-address buffer heap, addressed in shaders via `MTL4::ArgumentTable`
  at fixed slots; the bindless index is simply `handle.index()`.
- **lvk-style command model** — `VulkanImmediateCommands`-equivalent ring of
  reusable `MTL4::CommandBuffer`s gated by a single `MTL::SharedEvent` timeline.
- **Native MSL shaders** — text via `newLibrary(source)` or precompiled
  `metallib` via `newLibrary(dispatch_data)`.

## Requirements

- macOS with a **Metal 4**-capable GPU (Apple7 family or newer).
- Xcode Command Line Tools (clang with C++20).
- [CMake](https://cmake.org/) ≥ 3.21 and [Ninja](https://ninja-build.org/).
- Python 3 — used by the dependency bootstrapper and by lvk's own dependency
  deployment.
- lvk as a dependency (bootstrapped automatically, or supplied via a path).

## Building

```sh
# Dependencies are fetched automatically on first configure (bootstrap.py),
# and lvk deploys its own deps too.
cmake -S . -B build -G Ninja
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
| `LVK_METAL_LVK_DIR` | `3party/src/lightweightvk` | Path to the lightweightvk source tree. |

By default the bundled lvk (`LVK_METAL_LVK_DIR == 3party/src/lightweightvk`) is
configured and built for you. If you point `LVK_METAL_LVK_DIR` at an external
checkout, `lvk-metal` assumes that project already defines the `LVKLibrary`
target before it is added — the bundled lvk options are not applied in that case.

## License

[MIT](LICENSE.md) © 2026 Roman Kuznetsov.

Bundled/derived dependencies keep their own licenses (lightweightvk, metal-cpp,
GLFW, ldrutils).
