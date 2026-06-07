"""
Export a trained PyTorch policy to the dependency-free formats the C deployment
consumes:

  * policy.bin          -- the flat little-endian weights file src/policy.c loads.
  * tests/data/policy_ref.bin     -- a copy used as the parity-test model.
  * tests/data/policy_ref_io.bin  -- random observations + their reference logits,
                                     so tests/test_policy.c can assert the C
                                     forward pass matches PyTorch within 1e-4.

The byte layout MUST match src/policy.c exactly (see the contract there). The one
load-bearing detail: PyTorch nn.Linear.weight is shaped (out, in) and row-major,
which is exactly what the C dense() loop expects (row o == W + o*in), so weights
are written with NO transpose. A transpose here would silently invert every
matrix-vector product -- caught by the parity test, but avoided by construction.

No fallbacks: the module is walked and export RAISES if it contains any layer
type the hand-written C forward pass cannot reproduce (anything but Linear/Tanh).
"""
import argparse
import struct
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn

import ppo as PPO
import env as ENV

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
MAGIC = b"PPO1"

ALLOWED = (PPO.MultiDiscreteActorCritic, nn.Sequential, nn.ModuleList,
           nn.Linear, nn.Tanh)


def assert_exportable(model):
    for m in model.modules():
        if not isinstance(m, ALLOWED):
            raise TypeError(
                f"layer {type(m).__name__} has no hand-written C analog; the C "
                f"forward pass supports only Linear + Tanh. Refusing to export.")


def write_linear(buf, linear):
    w = linear.weight.detach().cpu().numpy().astype("<f4")
    b = linear.bias.detach().cpu().numpy().astype("<f4")
    assert w.ndim == 2 and b.shape[0] == w.shape[0]
    buf += w.tobytes()
    buf += b.tobytes()


def export_policy_bin(model, frame_skip, obs_version, path):
    assert_exportable(model)
    nvec = model.nvec
    hidden = model.trunk[0].out_features
    obs_dim = model.trunk[0].in_features

    head = bytearray()
    head += MAGIC
    head += struct.pack("<i", 1)
    head += struct.pack("<i", obs_dim)
    head += struct.pack("<i", 2)
    head += struct.pack("<i", hidden)
    head += struct.pack("<i", len(nvec))
    for n in nvec:
        head += struct.pack("<i", n)
    head += struct.pack("<i", frame_skip)
    head += struct.pack("<I", obs_version)

    body = bytearray()
    write_linear(body, model.trunk[0])
    write_linear(body, model.trunk[2])
    for h in range(len(nvec)):
        write_linear(body, model.heads[h])

    Path(path).write_bytes(bytes(head) + bytes(body))
    print(f"[export] wrote {path} ({len(head)+len(body)} bytes, frame_skip={frame_skip})")


def export_parity_io(model, obs_dim, n_samples, path, seed=12345):
    rng = np.random.default_rng(seed)
    obs = rng.uniform(-1.0, 1.0, size=(n_samples, obs_dim)).astype(np.float32)
    obs[0] = 0.0
    with torch.no_grad():
        h = model.trunk(torch.tensor(obs))
        logits = torch.cat([head(h) for head in model.heads], dim=-1).numpy().astype(np.float32)
    total = logits.shape[1]
    out = bytearray()
    out += struct.pack("<iii", n_samples, obs_dim, total)
    out += obs.tobytes()
    out += logits.tobytes()
    Path(path).write_bytes(bytes(out))
    print(f"[export] wrote {path} ({n_samples} samples, {total} logits each)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", default=str(HERE / "checkpoints" / "navigation.pt"))
    ap.add_argument("--out", default=str(HERE / "checkpoints" / "policy.bin"))
    ap.add_argument("--ref-dir", default=str(ROOT / "tests" / "data"))
    ap.add_argument("--samples", type=int, default=256)
    args = ap.parse_args()

    ck = torch.load(args.ckpt, map_location="cpu", weights_only=False)
    model = PPO.MultiDiscreteActorCritic(ck["obs_dim"], ck["nvec"], ck["hidden"])
    model.load_state_dict(ck["state_dict"])
    model.eval()

    if ck["obs_version"] != int(ENV.lib.OBS_VERSION):
        raise SystemExit(
            f"checkpoint obs_version {ck['obs_version']} != current "
            f"OBS_VERSION {int(ENV.lib.OBS_VERSION)} -- stale checkpoint.")

    export_policy_bin(model, ck["frame_skip"], ck["obs_version"], args.out)

    ref_dir = Path(args.ref_dir)
    ref_dir.mkdir(parents=True, exist_ok=True)
    export_policy_bin(model, ck["frame_skip"], ck["obs_version"], ref_dir / "policy_ref.bin")
    export_parity_io(model, ck["obs_dim"], args.samples, ref_dir / "policy_ref_io.bin")


if __name__ == "__main__":
    main()
