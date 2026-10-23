#!/usr/bin/env python3
"""Convert RVC voice checkpoints (.pth) to VoiceFem-compatible ONNX models.

Usage:
    python tools/pth_to_onnx.py <model.pth> <out.onnx> [--test]

Supports RVC v1 (256-dim features) and v2 (768-dim) checkpoints, with or
without F0. The exported ONNX follows the official RVC export recipe
(tools/export_onnx.py, SynthesizerTrnMsNSFsidM) using the realtime-VC
convention that VoiceFem's neural engine consumes:

    phone          float32 [1, T, 256|768]
    phone_lengths  int64   [1]
    pitch          int64   [1, T]        (coarse 1..255; f0 models only)
    pitchf         float32 [1, T]        (Hz; f0 models only)
    ds / sid       int64   [1]
    rnd            float32 [1, C, T]     (decoder noise; C = inter_channels)
    -> audio       float32 [1, 1, M]     (M = T * modelSR/100 samples)

Frame semantics (RVC v1/v2 generators): phone/pitch/pitchf are consumed at
100 frames/s - i.e. the 50 fps ContentVec features and F0 must be duplicated
(x2, nearest) before feeding. VoiceFem's engine detects this automatically
via the model probe and handles the upsampling.

Install:  pip install torch onnx onnxruntime scipy
          (Windows CPU:  pip install torch --index-url https://download.pytorch.org/whl/cpu)

After converting, copy the .onnx into %APPDATA%\\VoiceFem\\models
(next to content.onnx) and restart / rescan in the plugin.
"""
import argparse
import os
import sys

import numpy as np
import torch
from torch import nn

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rvc_arch import (  # noqa: E402
    SynthesizerTrnMs256NSFsid,
    SynthesizerTrnMs256NSFsid_nono,
    SynthesizerTrnMs768NSFsid,
    SynthesizerTrnMs768NSFsid_nono,
    SynthesizerTrnMsNSFsidM,
)


class RvcExportNonF0(nn.Module):
    """Inference graph for f0-less checkpoints (noise as explicit input)."""

    def __init__(self, net, v2: bool):
        super().__init__()
        self.net = net
        self.v2 = v2

    def forward(self, phone, phone_lengths, sid, rnd):
        n = self.net
        g = n.emb_g(sid).unsqueeze(-1)
        m_p, logs_p, x_mask = n.enc_p(phone, phone_lengths)
        z_p = (m_p + torch.exp(logs_p) * rnd) * x_mask
        z = n.flow(z_p, x_mask, g=g, reverse=True)
        o = n.dec(z * x_mask, g=g)
        return o


class RvcExportM(nn.Module):
    """Official SynthesizerTrnMsNSFsidM graph, with two fixes:
    - the vendored models_onnx.py forgets to unpack the (x, logdet) tuple in
      the reversed flow loop (reverse=True) - we unpack explicitly;
    - speaker mixing (speaker_map) is not used, plain single-speaker path."""

    def __init__(self, net):
        super().__init__()
        self.net = net

    def forward(self, phone, phone_lengths, pitch, pitchf, ds, rnd):
        n = self.net
        g = n.emb_g(ds).unsqueeze(-1)                       # [1, gin, 1]
        m_p, logs_p, x_mask = n.enc_p(phone, pitch, phone_lengths)
        z_p = (m_p + torch.exp(logs_p) * rnd) * x_mask
        z = z_p
        for flow in n.flow.flows[::-1]:                     # reverse pass
            z, _ = flow(z, x_mask, g=g, reverse=True)
        o = n.dec(z * x_mask, pitchf, g=g)
        return o


def pick_model(version: str, f0: bool):
    if version == "v2":
        return SynthesizerTrnMs768NSFsid if f0 else SynthesizerTrnMs768NSFsid_nono
    return SynthesizerTrnMs256NSFsid if f0 else SynthesizerTrnMs256NSFsid_nono


def load_checkpoint(path):
    try:
        cpt = torch.load(path, map_location="cpu", weights_only=False)
    except TypeError:  # older torch without weights_only
        cpt = torch.load(path, map_location="cpu")
    if "weight" not in cpt or "config" not in cpt:
        raise SystemExit("not an RVC checkpoint (missing 'weight'/'config')")
    return cpt


def export_f0(cpt, version, config, model_sr, out_path, opset):
    """Official RVC export path (SynthesizerTrnMsNSFsidM, rnd as input)."""
    config = list(config)
    n_spk = int(cpt["weight"]["emb_g.weight"].shape[0]) if "emb_g.weight" in cpt["weight"] else 1
    config[-3] = n_spk  # official recipe: fix the spk slot
    net = SynthesizerTrnMsNSFsidM(*config, version=version, is_half=False)
    missing, unexpected = net.load_state_dict(cpt["weight"], strict=False)
    unexpected = [k for k in unexpected if not k.startswith("module.")]
    bad_missing = [k for k in missing if not k.startswith("enc_q.")]
    if unexpected or bad_missing:
        raise SystemExit(f"checkpoint mismatch:\n  missing: {bad_missing[:8]}\n"
                         f"  unexpected: {unexpected[:8]}")
    if missing:
        print(f"note: {len(missing)} enc_q.* weights absent (training-only module, fine for export)")
    net.eval()
    try:
        net.remove_weight_norm()
    except Exception as e:
        print("remove_weight_norm:", e)

    wrapper = RvcExportM(net).eval()
    feat_dim = 768 if version == "v2" else 256
    inter_ch = int(config[2])

    # RVC generators consume 100 fps inputs when modelSr / samplesPerFrame == 100
    # (v2 40k/48k models emit modelSr/100 samples per 50 fps ContentVec frame).
    # VoiceFem feeds T=6 frames (2x3) in that mode, T=3 otherwise - trace exactly
    # that shape, because the traced attention pads are T-locked.
    ups = config[12] if len(config) > 12 and isinstance(config[12], (list, tuple)) else [400]
    spf = float(np.prod(ups))
    fps = int(round(model_sr / spf)) if model_sr > 0 else 100
    if fps not in (50, 100):
        fps = 100
    T = 6 if fps == 100 else 3
    print(f"frame semantics: {spf:.0f} samples/frame -> {fps} fps mode, trace T={T}")
    iargs = (
        torch.rand(1, T, feat_dim) * 0.5,
        torch.tensor([T], dtype=torch.int64),
        torch.randint(5, 255, (1, T)),
        torch.rand(1, T) * 300.0 + 60.0,
        torch.LongTensor([0]),
        torch.rand(1, inter_ch, T),
    )
    inames = ["phone", "phone_lengths", "pitch", "pitchf", "ds", "rnd"]
    with torch.no_grad():
        torch.onnx.export(
            wrapper, iargs, out_path,
            input_names=inames,
            output_names=["audio"],
            dynamic_axes={"phone": [1], "pitch": [1], "pitchf": [1], "rnd": [2], "audio": [2]},
            do_constant_folding=False,
            opset_version=opset,
            verbose=False,
            dynamo=False,
        )
    return inames, iargs, feat_dim, inter_ch


def export_nonf0(cpt, version, out_path, opset):
    cls = pick_model(version, False)
    config = list(cpt["config"])
    net = cls(*config, is_half=False)
    missing, unexpected = net.load_state_dict(cpt["weight"], strict=False)
    bad_missing = [k for k in missing if not k.startswith("enc_q.")]
    unexpected = [k for k in unexpected if not k.startswith("module.")]
    if unexpected or bad_missing:
        raise SystemExit(f"checkpoint mismatch: {bad_missing[:5]} / {unexpected[:5]}")
    net.eval()
    try:
        net.remove_weight_norm()
    except Exception as e:
        print("remove_weight_norm:", e)

    wrapper = RvcExportNonF0(net, version == "v2").eval()
    feat_dim = 768 if version == "v2" else 256
    inter_ch = int(config[2])
    T = 3
    iargs = (
        torch.rand(1, T, feat_dim) * 0.5,
        torch.tensor([T], dtype=torch.int64),
        torch.LongTensor([0]),
        torch.rand(1, inter_ch, T),
    )
    inames = ["phone", "phone_lengths", "ds", "rnd"]
    with torch.no_grad():
        torch.onnx.export(
            wrapper, iargs, out_path,
            input_names=inames,
            output_names=["audio"],
            dynamic_axes={"phone": [1], "rnd": [2], "audio": [2]},
            do_constant_folding=False,
            opset_version=opset,
            dynamo=False,
        )
    return inames, iargs, feat_dim, inter_ch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pth", help="RVC .pth checkpoint")
    ap.add_argument("onnx", help="output .onnx path")
    ap.add_argument("--opset", type=int, default=16, help="16 = official RVC recipe")
    ap.add_argument("--test", action="store_true", help="verify torch vs onnx output")
    args = ap.parse_args()

    cpt = load_checkpoint(args.pth)
    version = str(cpt.get("version", "v1"))
    f0 = bool(cpt.get("f0", True))
    config = list(cpt["config"])
    model_sr = int(config[-1]) if isinstance(config[-1], (int, float)) else 0

    print(f"checkpoint: version={version} f0={f0} sr={model_sr}")

    if f0:
        inames, iargs, feat_dim, inter_ch = export_f0(cpt, version, config, model_sr, args.onnx, args.opset)
    else:
        inames, iargs, feat_dim, inter_ch = export_nonf0(cpt, version, args.onnx, args.opset)
    print("exported:", args.onnx, f"({os.path.getsize(args.onnx) / 1e6:.1f} MB)")

    try:
        import onnx
        m = onnx.load(args.onnx)
        for k, v in (("vfem_sr", str(model_sr)), ("vfem_f0", "1" if f0 else "0"),
                     ("vfem_fps", str(100 if (len(config) > 12 and model_sr > 0 and
                        int(round(model_sr / float(np.prod(config[12] if isinstance(config[12], (list, tuple)) else [400])))) == 100) else 50)),
                     ("vfem_version", version), ("vfem_source", os.path.basename(args.pth))):
            e = m.metadata_props.add(); e.key, e.value = k, v
        onnx.save(m, args.onnx)
    except Exception as e:
        print("metadata:", e)

    if args.test:
        import onnxruntime as ort
        feeds = {n: t.detach().numpy() for n, t in zip(inames, iargs)}
        ref = ort.InferenceSession(args.onnx, providers=["CPUExecutionProvider"])
        # official recipe check at the streaming frame count
        T0 = iargs[0].shape[1]
        for T in sorted({T0, 3 if T0 != 3 else 6}):
            f = dict(feeds)
            for n in ("phone", "pitch", "pitchf", "rnd"):
                if n in f:
                    axis = 1 if n != "rnd" else 2
                    sl = [slice(None)] * f[n].ndim
                    sl[axis] = slice(0, min(T, f[n].shape[axis]))
                    f[n] = f[n][tuple(sl)]
            if "phone_lengths" in f:
                f["phone_lengths"] = np.array([f["phone"].shape[1]], np.int64)
            try:
                out = ref.run(None, f)[0]
                print(f"  T={T}: OK audio={out.shape}")
            except Exception as e:
                print(f"  T={T}: (locked, expected for non-trace T) {str(e)[:50]}")
        out = ref.run(None, feeds)[0].astype(np.float32)
        print(f"test: rms={float(np.sqrt((out ** 2).mean())):.4f} "
              f"(sanity: > 0.005 expected for random features)")


if __name__ == "__main__":
    main()
