#!/usr/bin/env python3
"""Convert an RVC .pth voice model into a DyVocalizer-compatible ONNX generator.

Usage:
    python pth_to_onnx.py <model.pth> [--out <models_dir>] [--rvc-dir <RVC checkout>]

Output: <models_dir>/voice_<name>.onnx   (default models_dir = %APPDATA%/DyVocalizer/models)

Inputs of the exported graph (what NeuralVC.h feeds):
    phone         float [1,T,768]
    phone_lengths int64 [1]
    pitch         int64 [1,T]     (coarse pitch bins)
    pitchf        float [1,T]     (Hz)
    ds            int64 [1]       (speaker id)
output:
    audio         float [1,1,M]   @ model sample rate
The random noise input ('rnd') of the original RVC export is generated
inside the graph, so the plugin does not need to supply it.

Requirements: pip install torch onnx onnxruntime numpy
RVC code: git clone --depth 1 https://github.com/RVC-Project/Retrieval-based-Voice-Conversion-WebUI
          (auto-cloned to %APPDATA%/DyVocalizer/rvc if git is installed; or pass --rvc-dir)
Only v2 models (768-dim content features) match the bundled content.onnx;
v1 models (256-dim) are rejected with a clear message.
"""
import argparse
import os
import sys
from pathlib import Path


def default_models_dir() -> Path:
    appdata = os.environ.get("APPDATA")
    if appdata:
        return Path(appdata) / "DyVocalizer" / "models"
    return Path.home() / ".dyvocalizer" / "models"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("pth")
    ap.add_argument("--out", default=None)
    ap.add_argument("--rvc-dir", default=os.environ.get("RVC_DIR") or str(default_models_dir().parent / "rvc"))
    args = ap.parse_args()

    pth = Path(args.pth)
    out_dir = Path(args.out) if args.out else default_models_dir()
    out_dir.mkdir(parents=True, exist_ok=True)

    if not (Path(args.rvc_dir) / "infer").is_dir():
        import shutil, subprocess
        if shutil.which("git"):
            print("Fetching RVC code (one time)...")
            subprocess.run(["git", "clone", "--depth", "1",
                            "https://github.com/RVC-Project/Retrieval-based-Voice-Conversion-WebUI",
                            args.rvc_dir], check=False)
        if not (Path(args.rvc_dir) / "infer").is_dir():
            print("ERROR: RVC code folder not found. Install git or clone "
                  "RVC-Project/Retrieval-based-Voice-Conversion-WebUI into: " + args.rvc_dir)
            return 2
    sys.path.insert(0, str(Path(args.rvc_dir)))

    import torch
    from infer.lib.infer_pack.models_onnx import SynthesizerTrnMsNSFsidM

    cpt = torch.load(str(pth), map_location="cpu", weights_only=False)
    if "config" not in cpt or "weight" not in cpt:
        print("ERROR: this .pth is not an RVC voice model (no 'config'/'weight').")
        return 3
    version = cpt.get("version", "v1")
    if version != "v2":
        print("ERROR: model is RVC v1 (256-dim features). DyVocalizer's content encoder "
              "is 768-dim, so only v2 models are supported.")
        return 4
    if cpt.get("f0", 1) != 1:
        print("ERROR: model was trained without pitch (f0=0); DyVocalizer needs f0 models.")
        return 5

    cpt["config"][-3] = cpt["weight"]["emb_g.weight"].shape[0]
    net = SynthesizerTrnMsNSFsidM(*cpt["config"], is_half=False, version=version)
    net.load_state_dict(cpt["weight"], strict=False)
    net.eval()

    class Wrapper(torch.nn.Module):
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, phone, phone_lengths, pitch, pitchf, ds):
            rnd = torch.randn(1, 192, phone.shape[1])
            return self.m(phone, phone_lengths, pitch, pitchf, ds, rnd)

    T = 200
    dummy = (
        torch.rand(1, T, 768),
        torch.tensor([T]).long(),
        torch.randint(5, 255, (1, T)),
        torch.rand(1, T) * 300 + 80,
        torch.LongTensor([0]),
    )
    name = pth.stem.replace(" ", "_")
    out = out_dir / f"voice_{name}.onnx"
    torch.onnx.export(
        Wrapper(net), dummy, str(out),
        input_names=["phone", "phone_lengths", "pitch", "pitchf", "ds"],
        output_names=["audio"],
        dynamic_axes={"phone": [1], "pitch": [1], "pitchf": [1]},
        do_constant_folding=False, opset_version=16)
    print(f"OK: {out}  (sample rate {cpt['config'][-1]} Hz)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
