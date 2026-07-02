#!/usr/bin/env python3
"""Convert a CuNNy pretrained upscaler (a small residual CNN, LGPL-3.0, from the CuNNy
dependency at 3party/src/CuNNy) into an .mtlpackage that lvk::metal's
MTL4MachineLearningPipeline can run. It backs the reusable samples/common/upscaler
(lvk::metal::Upscaler), used by RTX_AO and SolarSystem as a 1/2-res render + upscale mode.
Default is the "fast" variant, an rgb=False LUMA model (the net upscales Rec.601 luma, chroma
is bilinear — cheapest, ideal for near-grayscale content like RTX_AO). Set CUNNY_MODEL to a
heavier rgb=True pickle (3x12-NVL / 4x16-NVL / 8x32-NVL) for full-RGB sharpening at higher GPU
cost. upscaler.cpp / Upscaler::init must be told which (its `luma` arg) to match the model.

CuNNy is a 2x super-resolution net: cin(3->32) -> 8x[conv(32->32)+clamp01] -> cout(32->12)
-> pixel_shuffle(2) -> residual; the final image is that residual + bilinear(2x) of the input.
The residual/bilinear add is done by upscaler.cpp's compute shader (like the NeRF activations),
so the exported network is only conv + clamp + pixel_shuffle.

Run with tools/ML/.venv (Python 3.12: torch, coremltools, numpy, pillow)."""

import os
import pickle
import subprocess

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

MPB = "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/metal-package-builder"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
# Pick an rgb=True CuNNy variant (heavier = sharper, lighter = faster). 8x32 is the heaviest;
# 3x12 is the lightest that keeps the plain RGB architecture below (~14x cheaper than 8x32).
CUNNY_MODEL = os.environ.get("CUNNY_MODEL", "fast-NVL")
CUNNY_PICKLE = os.path.join(ROOT, "3party/src/CuNNy/pretrained", CUNNY_MODEL + ".pickle")
ASSETS = os.path.join(ROOT, "samples", "common", "assets")


# CuNNy rgb=False models are luma-only (Rec.601): the net upscales luma, chroma is bilinear.
LUMA = torch.tensor([0.299, 0.587, 0.114])
RY = torch.tensor([[0.299, 0.587, 0.114], [-0.169, -0.331, 0.5], [0.5, -0.419, -0.081]])
YR = torch.tensor([[1.0, -0.00093, 1.401687], [1.0, -0.3437, -0.71417], [1.0, 1.77216, 0.00099]])


class CuNNy(nn.Module):
    def __init__(self, layers, rgb):
        super().__init__()
        self.rgb = rgb
        ich = layers[0]
        self.cin = nn.Conv2d(3 if rgb else 1, ich, 3, padding="same")
        self.conv = nn.ModuleList()
        for och in layers[1:]:
            self.conv.append(nn.Conv2d(ich, och, 3, padding="same", bias=False))
            ich = och
        self.cout = nn.Conv2d(ich, 12 if rgb else 4, 3, padding="same")

    def forward(self, x):
        x = torch.clamp(self.cin(x), 0.0, 1.0)
        for conv in self.conv:
            x = torch.clamp(conv(x), 0.0, 1.0)
        x = self.cout(x)
        return F.pixel_shuffle(x, 2)


def load_cunny():
    with open(CUNNY_PICKLE, "rb") as f:
        m = pickle.load(f)
    assert not m["quant-8"], "expected a non-dp4a model"
    model = CuNNy(m["layers"], m["rgb"])
    sd = {}
    for k, v in m.items():
        if not (k.endswith("weight") or k.endswith("bias")):
            continue
        key = k[len("_orig_mod.") :] if k.startswith("_orig_mod.") else k
        sd[key] = torch.as_tensor(np.asarray(v), dtype=torch.float32)
    model.load_state_dict(sd)
    return model.eval()


def upscale(model, low):
    base = F.interpolate(low, scale_factor=2, mode="bilinear", align_corners=False)
    if model.rgb:
        return torch.clamp(base + model(low), 0.0, 1.0)
    lum = (low * LUMA.view(1, 3, 1, 1)).sum(1, keepdim=True)
    res = model(lum)
    yuv = torch.einsum("nk,mkyx->mnyx", RY, base)
    newY = torch.clamp(yuv[:, 0:1] + res, 0.0, 1.0)
    rgb = torch.einsum("nk,mkyx->mnyx", YR, torch.cat([newY, yuv[:, 1:3]], dim=1))
    return torch.clamp(rgb, 0.0, 1.0)


def export(model):
    import coremltools as ct

    os.makedirs(ASSETS, exist_ok=True)
    cin = 3 if model.rgb else 1
    traced = torch.jit.trace(model, torch.rand(1, cin, 32, 32))
    dyn = ct.RangeDim(16, 4096)
    mlmodel = ct.convert(
        traced,
        inputs=[ct.TensorType(name="input", shape=(1, cin, dyn, dyn), dtype=np.float32)],
        outputs=[ct.TensorType(name="output", dtype=np.float32)],
        minimum_deployment_target=ct.target.macOS26,
        convert_to="mlprogram",
    )
    mlpackage = os.path.join(ASSETS, "cunny.mlpackage")
    mtlpackage = os.path.join(ASSETS, "cunny.mtlpackage")
    mlmodel.save(mlpackage)
    subprocess.run([MPB, "-ml", mlpackage, "-o", mtlpackage], check=True)
    print("wrote", mtlpackage)


def preview(model):
    from PIL import Image

    src = os.path.join(ROOT, "3party/src/lightweightvk/third-party/content/src/glTF-Sample-Models/2.0/Duck/glTF/DuckCM.png")
    img = np.asarray(Image.open(src).convert("RGB"), np.float32) / 255.0
    hi = torch.from_numpy(img).permute(2, 0, 1).unsqueeze(0)
    low = F.interpolate(hi, scale_factor=0.5, mode="area")
    with torch.no_grad():
        bil = F.interpolate(low, scale_factor=2, mode="bilinear", align_corners=False).clamp(0, 1)
        cun = upscale(model, low)
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)

    def to_img(t):
        return Image.fromarray((t.squeeze(0).permute(1, 2, 0).numpy() * 255).astype(np.uint8))

    w, h = to_img(bil).size
    strip = Image.new("RGB", (w * 2, h))
    strip.paste(to_img(bil), (0, 0))
    strip.paste(to_img(cun), (w, 0))
    strip.save(os.path.join(HERE, "build", "upscale_preview.png"))
    sharp = float((cun - bil).abs().mean())
    print(f"wrote build/upscale_preview.png (left bilinear | right CuNNy); mean |CuNNy-bilinear| = {sharp:.4f}")


def main():
    model = load_cunny()
    print(f"loaded CuNNy {CUNNY_MODEL}:", sum(p.numel() for p in model.parameters()), "params")
    preview(model)
    export(model)


if __name__ == "__main__":
    main()
