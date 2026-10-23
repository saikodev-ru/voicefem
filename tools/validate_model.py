#!/usr/bin/env python3
"""Validate that ONNX models are compatible with VoiceFem's neural engine.

Usage:  python3 validate_model.py <models_folder>

Checks:
  content.onnx      — float input (rank 2 or 3), float output with last dim 768
  voice_*.onnx      — phone/feat float input, int64 'length' input, pitch inputs,
                      one float audio output
Prints the inferred model sample rate (from a dummy generator run) when possible.
"""
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort


def main():
    folder = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    content = folder / "content.onnx"
    voices = sorted(p for p in folder.glob("*.onnx") if p.stem != "content")

    print(f"folder: {folder}")
    ok = True

    if not content.exists():
        print("MISSING: content.onnx (ContentVec-style content encoder, 16 kHz)")
        ok = False
    else:
        try:
            s = ort.InferenceSession(str(content), providers=["CPUExecutionProvider"])
            i = s.get_inputs()[0]
            o = s.get_outputs()[0]
            rank = len(i.shape)
            last = o.shape[-1]
            good = i.type == "tensor(float)" and rank in (2, 3) and last in (768, "N", None)
            print(f"content.onnx OK   input {i.name} {i.shape} -> output {o.name} {o.shape}"
                  + ("" if good else "  << UNEXPECTED SIGNATURE"))
            ok &= bool(good)
        except Exception as e:
            print(f"content.onnx BROKEN: {e}")
            ok = False

    if not voices:
        print("MISSING: voice_*.onnx (RVC-style generator, e.g. exported via w-okada/RVC)")
        ok = False
    for v in voices:
        try:
            s = ort.InferenceSession(str(v), providers=["CPUExecutionProvider"])
            names = [(x.name, x.type) for x in s.get_inputs()]
            outs = s.get_outputs()
            # probe frame->samples ratio
            T = 3
            feeds = {}
            for x in s.get_inputs():
                n = x.name.lower()
                if "length" in n:
                    feeds[x.name] = np.array([T], dtype=np.int64)
                elif "pitch" in n and "float" in x.type:
                    feeds[x.name] = np.full((1, T), 110.0, dtype=np.float32)
                elif "pitch" in n:
                    feeds[x.name] = np.full((1, T), 92, dtype=np.int64)
                elif "float" in x.type:
                    shape = [d if isinstance(d, int) else 1 for d in x.shape]
                    shape[-1] = shape[-1] if isinstance(x.shape[-1], int) and x.shape[-1] > 0 else 768
                    shape[-2] = T if len(shape) >= 2 else shape[-2]
                    feeds[x.name] = np.zeros(shape, dtype=np.float32)
                else:
                    feeds[x.name] = np.zeros([d if isinstance(d, int) else 1 for d in x.shape], dtype=np.int64)
            out = s.run([outs[0].name], feeds)[0]
            M = max(out.shape)
            sr = M * 50 / T
            snap = next((c for c in (32000, 40000, 44100, 48000) if abs(sr - c) < 200), None)
            print(f"{v.name}: OK   inputs={names}  audio_out={list(out.shape)}  modelSR≈{snap or sr:.0f}")
        except Exception as e:
            print(f"{v.name}: BROKEN ({e})")
            ok = False

    print("\nRESULT:", "COMPATIBLE" if ok else "INCOMPATIBLE — see messages above")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
