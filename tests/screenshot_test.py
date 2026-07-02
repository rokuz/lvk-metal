#!/usr/bin/env python3
"""Screenshot tests for lvk-metal samples.
"""

import argparse
import os
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REF_DIR = os.path.join(HERE, "references")

SAMPLES = [
    ("HelloTriangle", "samples/HelloTriangle/HelloTriangle", 1),
    ("TransferOps", "samples/TransferOps/TransferOps", 1),
    ("IndirectDraws", "samples/IndirectDraws/IndirectDraws", 1),
    ("MeshShaders", "samples/MeshShaders/MeshShaders", 1),
    ("MeshShaderFireworks", "samples/MeshShaderFireworks/MeshShaderFireworks", 650),
    ("RenderToCubeMap", "samples/RenderToCubeMap/RenderToCubeMap", 30),
    ("RenderToCubeMapSinglePass", "samples/RenderToCubeMapSinglePass/RenderToCubeMapSinglePass", 30),
    ("TextureView", "samples/TextureView/TextureView", 1),
    ("QueryPool", "samples/QueryPool/QueryPool", 1),
    ("SwapchainHDR", "samples/SwapchainHDR/SwapchainHDR", 1),
    ("OmniShadows", "samples/OmniShadows/OmniShadows", 60),
    ("LocalRead", "samples/LocalRead/LocalRead", 60),
    ("LocalReadTile", "samples/LocalReadTile/LocalReadTile", 60),
    ("MachineLearning", "samples/MachineLearning/MachineLearning", 47),
    ("SolarSystem", "samples/SolarSystem/SolarSystem", 120),
    ("ImGuiDemo", "samples/ImGuiDemo/ImGuiDemo", 1),
    ("Bistro", "samples/Bistro/Bistro", 30),
    ("RTX_Hello", "samples/RTX_Hello/RTX_Hello", 60),
    ("RTX_Textures", "samples/RTX_Textures/RTX_Textures", 60),
    ("RTX_Bistro", "samples/RTX_Bistro/RTX_Bistro", 1),
    ("RTX_AO", "samples/RTX_AO/RTX_AO", 64),
]

# Crash in Metal validation layer, check in new version of Xcode/Metal tooling.
SHADER_VALIDATION_SKIP = {"IndirectDraws"}

MODES = [
    ("msl", {}),
    ("slang", {"LVK_METAL_USE_SLANG": "1"}),
]

WIDTH = 1024
HEIGHT = 768

PNG_SIG = b"\x89PNG\r\n\x1a\n"


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != PNG_SIG:
        raise ValueError(f"{path}: not a PNG file")
    pos = 8
    width = height = bitdepth = colortype = None
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bitdepth, colortype = struct.unpack(">IIBB", chunk[:10])
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
    if bitdepth != 8 or colortype not in (2, 6):
        raise ValueError(f"{path}: unsupported PNG (bitdepth={bitdepth}, colortype={colortype})")
    channels = 4 if colortype == 6 else 3
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(height * stride)
    prev = bytearray(stride)
    pos = 0
    for row in range(height):
        ftype = raw[pos]
        pos += 1
        cur = bytearray(raw[pos:pos + stride])
        pos += stride
        if ftype == 1:
            for i in range(channels, stride):
                cur[i] = (cur[i] + cur[i - channels]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                cur[i] = (cur[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                a = cur[i - channels] if i >= channels else 0
                cur[i] = (cur[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                a = cur[i - channels] if i >= channels else 0
                c = prev[i - channels] if i >= channels else 0
                cur[i] = (cur[i] + _paeth(a, prev[i], c)) & 0xFF
        out[row * stride:(row + 1) * stride] = cur
        prev = cur
    return width, height, channels, out


def write_png(path, width, height, channels, pixels):
    colortype = 6 if channels == 4 else 2
    stride = width * channels
    raw = bytearray()
    for row in range(height):
        raw.append(0)
        raw += pixels[row * stride:(row + 1) * stride]

    def chunk(ctype, payload):
        return (struct.pack(">I", len(payload)) + ctype + payload +
                struct.pack(">I", zlib.crc32(ctype + payload) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, colortype, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(PNG_SIG)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def compare(ref, act, pixel_tol):
    """Return (fraction_over_tol or None, detail, delta_pixels). delta amplifies
    the absolute per-channel difference so even small diffs are visible."""
    rw, rh, rc, rp = ref
    aw, ah, ac, ap = act
    if (rw, rh) != (aw, ah):
        return None, f"size mismatch: reference {rw}x{rh}, actual {aw}x{ah}", None
    n = rw * rh
    delta = bytearray(n * 4)
    diff_pixels = 0
    max_diff = 0
    for p in range(n):
        ri, ai, di = p * rc, p * ac, p * 4
        pixel_max = 0
        for c in range(3):
            d = abs(rp[ri + c] - ap[ai + c])
            if d > pixel_max:
                pixel_max = d
            delta[di + c] = d * 8 if d * 8 < 255 else 255
        delta[di + 3] = 255
        if pixel_max > max_diff:
            max_diff = pixel_max
        if pixel_max > pixel_tol:
            diff_pixels += 1
    return diff_pixels / n, f"max channel diff {max_diff}, {diff_pixels}/{n} pixels over tol", (rw, rh, delta)


def slang_enabled(build_dir):
    cache = os.path.join(build_dir, "CMakeCache.txt")
    try:
        with open(cache) as f:
            for line in f:
                if line.startswith("LVK_METAL_WITH_SAMPLES_SLANG:") and line.rstrip().endswith("ON"):
                    return True
    except OSError:
        pass
    return False


def run_sample(build_dir, rel_path, frame, out_png, validation, shader_validation, env_extra):
    binary = os.path.join(build_dir, rel_path)
    if not os.path.exists(binary):
        raise FileNotFoundError(f"binary not found: {binary} (build it first)")
    env = dict(os.environ)
    env.update(env_extra)
    if validation:
        env["MTL_DEBUG_LAYER"] = "1"
        env["MTL_DEBUG_LAYER_ERROR_MODE"] = "assert"
        if shader_validation:
            env["MTL_SHADER_VALIDATION"] = "1"
    cmd = [binary, "--headless", "--screenshot-file", out_png, "--screenshot-frame", str(frame),
           "--width", str(WIDTH), "--height", str(HEIGHT)]
    subprocess.run(cmd, env=env, check=True, timeout=120, capture_output=True)
    if not os.path.exists(out_png):
        raise RuntimeError(f"sample did not produce {out_png}")


def main():
    ap = argparse.ArgumentParser(description="lvk-metal screenshot tests")
    ap.add_argument("--build-dir", default=os.path.join(os.path.dirname(HERE), "build"))
    ap.add_argument("--update", action="store_true", help="overwrite all reference images")
    ap.add_argument("--pixel-tol", type=int, default=4, help="per-channel diff tolerated per pixel")
    ap.add_argument("--max-diff-fraction", type=float, default=0.001, help="fraction of pixels allowed over tol")
    ap.add_argument("--no-validation", action="store_true", help="disable Metal API Validation")
    ap.add_argument("--only", default="", help="run only the named sample")
    args = ap.parse_args()

    out_dir = os.path.join(args.build_dir, "_out")
    os.makedirs(REF_DIR, exist_ok=True)
    os.makedirs(out_dir, exist_ok=True)

    have_slang = slang_enabled(args.build_dir)
    if not have_slang:
        print("[NOTE] build not configured with -DLVK_METAL_WITH_SAMPLES_SLANG=ON; skipping Slang mode")

    failures = 0
    for name, rel_path, frame in SAMPLES:
        if args.only and args.only != name:
            continue
        ref_png = os.path.join(REF_DIR, name + ".png")

        for mode, env_extra in MODES:
            if mode == "slang" and not have_slang:
                continue
            label = f"{name} ({mode})"
            out_png = os.path.join(out_dir, f"{name}.{mode}.png")
            try:
                run_sample(args.build_dir, rel_path, frame, out_png, not args.no_validation,
                           name not in SHADER_VALIDATION_SKIP, env_extra)
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired, FileNotFoundError, RuntimeError) as e:
                print(f"[FAIL] {label}: {e}")
                failures += 1
                continue

            # The reference is seeded from the MSL run; Slang must match it.
            if mode == "msl" and (args.update or not os.path.exists(ref_png)):
                with open(out_png, "rb") as src, open(ref_png, "wb") as dst:
                    dst.write(src.read())
                print(f"[SEED] {label}: wrote reference {os.path.relpath(ref_png)}")
                continue

            ref = read_png(ref_png)
            act = read_png(out_png)
            frac, detail, delta = compare(ref, act, args.pixel_tol)
            if frac is None or frac > args.max_diff_fraction:
                delta_png = os.path.join(out_dir, f"{name}.{mode}.delta.png")
                if delta is not None:
                    write_png(delta_png, delta[0], delta[1], 4, delta[2])
                    detail += f"; delta: {os.path.relpath(delta_png)}"
                print(f"[FAIL] {label}: {detail}")
                failures += 1
            else:
                print(f"[PASS] {label}: {detail} (fraction {frac:.5f})")

    if failures:
        print(f"\n{failures} sample(s) failed")
        return 1
    print("\nall samples passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
