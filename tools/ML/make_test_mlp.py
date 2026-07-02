#!/usr/bin/env python3
"""Toolchain smoke test: build a trivial MLP, run it through
torch -> coremltools (.mlpackage) -> metal-package-builder (.mtlpackage),
and emit a few (input, expected-output) vectors so the C++ side can verify
that lvk::metal::IMetalContext::createMachineLearningPipeline runs it correctly.

Run with the tools/ML/.venv interpreter (Python 3.12, torch + coremltools)."""

import json
import os
import subprocess
import sys

import numpy as np
import torch

MPB = "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/metal-package-builder"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "build")


class MLP(torch.nn.Module):
    def __init__(self):
        super().__init__()
        self.net = torch.nn.Sequential(
            torch.nn.Linear(3, 8),
            torch.nn.ReLU(),
            torch.nn.Linear(8, 4),
        )

    def forward(self, x):
        return self.net(x)


def main():
    import coremltools as ct

    os.makedirs(OUT, exist_ok=True)
    torch.manual_seed(0)
    model = MLP().eval()

    example = torch.rand(5, 3, dtype=torch.float32)
    traced = torch.jit.trace(model, example)

    mlmodel = ct.convert(
        traced,
        inputs=[ct.TensorType(name="input", shape=(ct.RangeDim(1, 1_000_000), 3), dtype=np.float32)],
        outputs=[ct.TensorType(name="output", dtype=np.float32)],
        minimum_deployment_target=ct.target.macOS26,
        convert_to="mlprogram",
    )
    mlpackage = os.path.join(OUT, "test_mlp.mlpackage")
    mtlpackage = os.path.join(OUT, "test_mlp.mtlpackage")
    mlmodel.save(mlpackage)

    subprocess.run([MPB, "-ml", mlpackage, "-o", mtlpackage], check=True)

    with torch.no_grad():
        expected = model(example).numpy().astype(np.float32)
    inp = example.numpy().astype(np.float32)

    # Flat binary the C++ verifier reads without a JSON dependency:
    # [u32 numRows][u32 inCols][u32 outCols][f32 input...][f32 expected...]
    header = np.array([inp.shape[0], inp.shape[1], expected.shape[1]], dtype=np.uint32)
    with open(os.path.join(OUT, "test_mlp.bin"), "wb") as f:
        f.write(header.tobytes())
        f.write(np.ascontiguousarray(inp).tobytes())
        f.write(np.ascontiguousarray(expected).tobytes())

    print("wrote", mtlpackage)
    print("wrote", os.path.join(OUT, "test_mlp.bin"))


if __name__ == "__main__":
    sys.exit(main())
