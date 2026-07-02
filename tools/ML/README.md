# tools/ML — machine-learning assets for the `MachineLearning` sample

Offline scripts that produce the neural network the `MachineLearning` sample runs on
the GPU via `MTL4MachineLearningPipeline`. At runtime lvk-metal only loads the
`.mtlpackage`; the weights are trained and converted here.

The generated assets (`samples/MachineLearning/assets/nerf.mtlpackage` + `nerf.bin`)
are committed, so **you only need this to regenerate the model** — building and running
the sample does not require any of it.

## Scripts

- **`nerf.py`** — trains a tiny view-independent NeRF on `Duck.glb` (from the
  glTF-Sample-Models already deployed under lvk's content) and exports it:
  - renders ground-truth views by ray-casting the mesh with embree (texture × Lambert shading),
  - trains a small MLP on the GPU (Metal / MPS),
  - exports `samples/MachineLearning/assets/nerf.mtlpackage` + `nerf.bin` (camera & volume-render params),
  - also writes a checkpoint `build/nerf.pt` and a preview `build/nerf_preview.png`.
- **`make_test_mlp.py`** — toolchain smoke test: builds a trivial MLP, runs it through the
  same `torch → coremltools → metal-package-builder` pipeline, and emits `build/test_mlp.mtlpackage`
  + `build/test_mlp.bin` (input/expected vectors) so the C++ ML path can be checked numerically.

## Prerequisites

- macOS 26 + Xcode 26 (provides `metal-package-builder`, used to build the `.mtlpackage`).
- Python **3.12** (torch / coremltools have no 3.14 wheels, so this is a *separate* env from
  the repo's `tools/.venv`).

## Setup (one-time)

```sh
uv venv --python 3.12 tools/ML/.venv
uv pip install --python tools/ML/.venv/bin/python numpy pillow torch coremltools trimesh embreex
```

## Run

```sh
# train the NeRF and export nerf.mtlpackage + nerf.bin (a few minutes on Apple Silicon)
tools/ML/.venv/bin/python tools/ML/nerf.py

# optional: verify the torch -> coremltools -> metal-package-builder toolchain
tools/ML/.venv/bin/python tools/ML/make_test_mlp.py
```

## Pipeline

```
PyTorch (train)  ->  coremltools (.mlpackage, mlprogram)  ->  metal-package-builder -ml  ->  .mtlpackage
```

The exported network is a plain Linear+ReLU MLP: positional encoding and the output
activations (sigmoid rgb / softplus density) run on the host — in `nerf.py` and, bit-for-bit,
in the sample's compute shaders — because `metal-package-builder` cannot lower `sin`/`cos`/`concat`.
Tweak network size / quality via the constants at the top of `nerf.py`
(`TRAIN_RES`, `NUM_VIEWS`, `L_EMBED`, `WIDTH`, `DEPTH`, iteration count).
