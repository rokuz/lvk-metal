#!/usr/bin/env python3
"""Train a tiny view-independent NeRF on a real 3D model shipped with the project
(the glTF-Sample-Models Duck) and export the trained network as an .mtlpackage that
lvk::metal::createMachineLearningPipeline can run.

Ground-truth views are rendered headlessly by ray-casting the mesh with embree and
shading each hit with its texture color * Lambert term (black background). The NeRF
maps world-space sample positions [N,3] -> [N,4] = (rgb, sigma); positional encoding
is baked into the graph, so the C++ sample only does ray generation + volume
compositing around a chunked ML dispatch.

Pipeline: trimesh/embree (GT) -> PyTorch (train on MPS) -> coremltools (.mlpackage)
-> metal-package-builder (.mtlpackage) + a flat nerf.bin with render params.

Run with tools/ML/.venv (Python 3.12: torch, coremltools, trimesh, embreex, pillow)."""

import os
import struct
import subprocess

import numpy as np
import torch
import torch.nn as nn
import trimesh
from trimesh.ray.ray_pyembree import RayMeshIntersector
from trimesh.triangles import points_to_barycentric

MPB = "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/metal-package-builder"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
ASSETS = os.path.join(ROOT, "samples", "MachineLearning", "assets")
MODEL = os.path.join(
    ROOT, "3party/src/lightweightvk/third-party/content/src/glTF-Sample-Models/2.0/Duck/glTF-Binary/Duck.glb"
)

DEVICE = torch.device("mps" if torch.backends.mps.is_available() else "cpu")

OBJECT_RADIUS = 0.9
CAM_RADIUS = 2.6
NEAR, FAR = 1.4, 3.8
FOV_DEG = 45.0
TRAIN_RES = 128
NUM_VIEWS = 90
NUM_SAMPLES = 96
LIGHT_DIR = np.array([0.5, 0.8, 0.6], np.float32)
LIGHT_DIR /= np.linalg.norm(LIGHT_DIR)

RENDER_RES = 160
RENDER_SAMPLES = 96
ELEVATION_DEG = 18.0

L_EMBED = 10
WIDTH = 192
DEPTH = 8


def load_model():
    mesh = trimesh.load(MODEL, force="mesh", process=False)
    mesh.vertices -= mesh.bounds.mean(axis=0)
    mesh.vertices *= OBJECT_RADIUS / (0.5 * np.linalg.norm(mesh.extents))
    tex = np.asarray(mesh.visual.material.baseColorTexture.convert("RGB"), np.float32) / 255.0
    uv = np.asarray(mesh.visual.uv, np.float32)
    return mesh, RayMeshIntersector(mesh), tex, uv


def sample_texture(tex, uvs):
    h, w = tex.shape[:2]
    u = np.clip(uvs[:, 0], 0.0, 1.0) * (w - 1)
    v = (1.0 - np.clip(uvs[:, 1], 0.0, 1.0)) * (h - 1)
    return tex[np.round(v).astype(int), np.round(u).astype(int)]


def pose_spherical(azimuth_deg, elevation_deg, radius):
    def rot_x(a):
        c, s = np.cos(a), np.sin(a)
        return np.array([[1, 0, 0, 0], [0, c, -s, 0], [0, s, c, 0], [0, 0, 0, 1]], np.float32)

    def rot_y(a):
        c, s = np.cos(a), np.sin(a)
        return np.array([[c, 0, s, 0], [0, 1, 0, 0], [-s, 0, c, 0], [0, 0, 0, 1]], np.float32)

    trans = np.array([[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, radius], [0, 0, 0, 1]], np.float32)
    flip = np.array([[-1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0], [0, 0, 0, 1]], np.float32)
    c2w = rot_y(np.deg2rad(azimuth_deg)) @ rot_x(np.deg2rad(-elevation_deg)) @ trans
    return (flip @ c2w).astype(np.float32)


def get_rays(res, focal, c2w):
    i, j = np.meshgrid(np.arange(res, dtype=np.float32), np.arange(res, dtype=np.float32), indexing="xy")
    dirs = np.stack([(i - res * 0.5) / focal, -(j - res * 0.5) / focal, -np.ones_like(i)], -1)
    rays_d = dirs.reshape(-1, 3) @ c2w[:3, :3].T
    rays_o = np.broadcast_to(c2w[:3, 3], rays_d.shape)
    return rays_o.astype(np.float32), rays_d.astype(np.float32)


def render_gt(mesh, rmi, tex, uv, res, focal, c2w):
    rays_o, rays_d = get_rays(res, focal, c2w)
    colors = np.zeros((res * res, 3), np.float32)
    locs, idx_ray, idx_tri = rmi.intersects_location(rays_o, rays_d, multiple_hits=False)
    if len(idx_ray):
        tris = mesh.triangles[idx_tri]
        bary = points_to_barycentric(tris, locs)
        face_uv = uv[mesh.faces[idx_tri]]
        hit_uv = np.einsum("ij,ijk->ik", bary, face_uv)
        base = sample_texture(tex, hit_uv)
        normals = mesh.face_normals[idx_tri]
        lambert = np.abs(normals @ LIGHT_DIR)
        shade = (0.25 + 0.75 * lambert).astype(np.float32)
        colors[idx_ray] = base * shade[:, None]
    return rays_o, rays_d, colors


def build_dataset(mesh, rmi, tex, uv):
    focal = 0.5 * TRAIN_RES / np.tan(0.5 * np.deg2rad(FOV_DEG))
    rng = np.random.default_rng(0)
    O, D, C = [], [], []
    for _ in range(NUM_VIEWS):
        c2w = pose_spherical(rng.uniform(0, 360), rng.uniform(-20, 60), CAM_RADIUS)
        ro, rd, col = render_gt(mesh, rmi, tex, uv, TRAIN_RES, focal, c2w)
        O.append(ro)
        D.append(rd)
        C.append(col)
    return (
        np.concatenate(O).astype(np.float32),
        np.concatenate(D).astype(np.float32),
        np.concatenate(C).astype(np.float32),
        float(focal),
    )


ENC_DIM = 3 * (1 + 2 * L_EMBED)


def encode(x):
    """Positional encoding: [x, sin(2^k x), cos(2^k x) for k in 0..L-1]. Kept OUT of the
    exported network so the .mtlpackage is a plain Linear+ReLU MLP (metal-package-builder
    can't lower sin/cos/concat). The C++ sample reproduces this exactly."""
    out = [x]
    for k in range(L_EMBED):
        f = float(2**k)
        out += [torch.sin(x * f), torch.cos(x * f)]
    return torch.cat(out, dim=-1)


class NeRF(nn.Module):
    """Encoded features [N, ENC_DIM] -> raw [N,4]. Pure Linear+ReLU (exported as-is);
    output activations (sigmoid rgb / relu sigma) are applied by the caller."""

    def __init__(self, width=WIDTH, depth=DEPTH):
        super().__init__()
        layers = [nn.Linear(ENC_DIM, width), nn.ReLU()]
        for _ in range(depth - 1):
            layers += [nn.Linear(width, width), nn.ReLU()]
        layers += [nn.Linear(width, 4)]
        self.net = nn.Sequential(*layers)

    def forward(self, features):
        return self.net(features)


def render_rays(model, rays_o, rays_d, num_samples, perturb):
    t = torch.linspace(NEAR, FAR, num_samples, device=rays_o.device)
    z = t.expand(rays_o.shape[0], num_samples).clone()
    if perturb:
        mid = 0.5 * (z[:, 1:] + z[:, :-1])
        upper = torch.cat([mid, z[:, -1:]], -1)
        lower = torch.cat([z[:, :1], mid], -1)
        z = lower + (upper - lower) * torch.rand_like(z)
    pts = rays_o[:, None, :] + rays_d[:, None, :] * z[..., None]
    raw = model(encode(pts.reshape(-1, 3))).reshape(rays_o.shape[0], num_samples, 4)
    rgb = torch.sigmoid(raw[..., :3])
    sigma = torch.nn.functional.softplus(raw[..., 3])
    dists = torch.cat([z[:, 1:] - z[:, :-1], torch.full_like(z[:, :1], 1e10)], -1)
    alpha = 1.0 - torch.exp(-sigma * dists)
    trans = torch.cumprod(torch.cat([torch.ones_like(alpha[:, :1]), 1.0 - alpha + 1e-10], -1), -1)[:, :-1]
    weights = alpha * trans
    return torch.sum(weights[..., None] * rgb, dim=1)


def train(ro, rd, col):
    ro_t = torch.from_numpy(ro).to(DEVICE)
    rd_t = torch.from_numpy(rd).to(DEVICE)
    col_t = torch.from_numpy(col).to(DEVICE)
    hit = torch.from_numpy((col.sum(axis=1) > 0).astype(np.int64)).to(DEVICE)
    hit_idx = torch.nonzero(hit, as_tuple=False).squeeze(1)
    n = ro_t.shape[0]
    model = NeRF().to(DEVICE)
    opt = torch.optim.Adam(model.parameters(), lr=5e-4)
    sched = torch.optim.lr_scheduler.ExponentialLR(opt, gamma=0.9999)
    iters, batch = 16000, 4096
    for it in range(iters):
        # Object-biased sampling: half the batch from rays that hit the model, half uniform.
        half = batch // 2
        idx = torch.cat([hit_idx[torch.randint(0, hit_idx.shape[0], (half,), device=DEVICE)],
                         torch.randint(0, n, (batch - half,), device=DEVICE)])
        pred = render_rays(model, ro_t[idx], rd_t[idx], NUM_SAMPLES, perturb=True)
        loss = torch.mean((pred - col_t[idx]) ** 2)
        opt.zero_grad()
        loss.backward()
        opt.step()
        sched.step()
        if (it + 1) % 500 == 0:
            print(f"  iter {it + 1}/{iters}  loss {loss.item():.5f}  psnr {-10 * np.log10(loss.item()):.2f}")
    return model.cpu().eval()


@torch.no_grad()
def render_image(model, c2w, res, focal):
    ro, rd = get_rays(res, focal, c2w)
    ro_t = torch.from_numpy(ro).to(DEVICE)
    rd_t = torch.from_numpy(rd).to(DEVICE)
    out = []
    for i in range(0, ro_t.shape[0], 8192):
        out.append(render_rays(model.to(DEVICE), ro_t[i : i + 8192], rd_t[i : i + 8192], RENDER_SAMPLES, perturb=False))
    img = torch.cat(out).clamp(0, 1).reshape(res, res, 3).cpu().numpy()
    return (img * 255).astype(np.uint8)


def preview(model):
    from PIL import Image

    focal = 0.5 * RENDER_RES / np.tan(0.5 * np.deg2rad(FOV_DEG))
    img = render_image(model, pose_spherical(35.0, 18.0, CAM_RADIUS), RENDER_RES, focal)
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    Image.fromarray(img).save(os.path.join(HERE, "build", "nerf_preview.png"))
    print("wrote", os.path.join(HERE, "build", "nerf_preview.png"), "nonblack", int((img.sum(2) > 0).mean() * 100), "%")


def export(model):
    import coremltools as ct

    model = model.cpu().eval()
    os.makedirs(ASSETS, exist_ok=True)
    traced = torch.jit.trace(model, torch.rand(8, ENC_DIM, dtype=torch.float32))
    mlmodel = ct.convert(
        traced,
        inputs=[ct.TensorType(name="input", shape=(ct.RangeDim(1, 8_000_000), ENC_DIM), dtype=np.float32)],
        outputs=[ct.TensorType(name="output", dtype=np.float32)],
        minimum_deployment_target=ct.target.macOS26,
        convert_to="mlprogram",
    )
    mlpackage = os.path.join(ASSETS, "nerf.mlpackage")
    mtlpackage = os.path.join(ASSETS, "nerf.mtlpackage")
    mlmodel.save(mlpackage)
    subprocess.run([MPB, "-ml", mlpackage, "-o", mtlpackage], check=True)

    # nerf.bin: u32 input_index,output_index,l_embed,enc_dim; f32 fov_deg,near,far,radius,elevation_deg.
    # The sample builds its own camera poses (pose_spherical) so it can orbit the model.
    with open(os.path.join(ASSETS, "nerf.bin"), "wb") as f:
        f.write(struct.pack("<4I", 0, 1, L_EMBED, ENC_DIM))
        f.write(struct.pack("<5f", FOV_DEG, NEAR, FAR, CAM_RADIUS, ELEVATION_DEG))
    print("wrote", mtlpackage)
    print("wrote", os.path.join(ASSETS, "nerf.bin"))


def main():
    print(f"loading model + rendering {NUM_VIEWS} GT views ...")
    mesh, rmi, tex, uv = load_model()
    ro, rd, col = build_dataset(mesh, rmi, tex, uv)[:3]
    print(f"training tiny NeRF on {DEVICE} ({ro.shape[0]} rays) ...")
    model = train(ro, rd, col)
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    torch.save(model.state_dict(), os.path.join(HERE, "build", "nerf.pt"))
    print("preview ...")
    preview(model)
    print("exporting ...")
    export(model)


if __name__ == "__main__":
    main()
