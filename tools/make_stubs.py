#!/usr/bin/env python3.13
"""Generate stub ONNX models for NeuralVC plumbing tests.

content.onnx : wav [1,N] @16k -> features [1, N/320, 768]   (Conv1D, deterministic)
voice_stub.onnx / voice2_stub.onnx : RVC-generator interface
    phone [1,T,768] float, phone_lengths [1] int64, pitch [1,T] int64,
    pitchf [1,T] float, ds [1] int64  ->  audio [1,1,T*960] float (48k convention)

The stubs verify streaming, shapes, latency balance and voice switching —
not neural quality (real RVC models drop into the same interface).
"""
import numpy as np
import onnx
from onnx import helper, TensorProto


def make_content(path: str):
    N = helper.make_tensor_value_info("wav", TensorProto.FLOAT, [1, 1, "n16"])
    T = helper.make_tensor_value_info("features", TensorProto.FLOAT, [1, "t", 768])
    rng = np.random.default_rng(7)
    W = rng.uniform(-0.05, 0.05, size=(768, 1, 320)).astype(np.float32)
    w_init = onnx.numpy_helper.from_array(W, name="W")
    node = helper.make_node("Conv", ["wav", "W"], ["conv"], kernel_shape=[320], strides=[320])
    node2 = helper.make_node("Transpose", ["conv"], ["features"], perm=[0, 2, 1])
    graph = helper.make_graph([node, node2], "content_stub", [N], [T], initializer=[w_init])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 8
    onnx.save(model, path)
    print("wrote", path)


def make_gen(path: str, seed: int):
    r = np.random.default_rng(seed)
    phone = helper.make_tensor_value_info("phone", TensorProto.FLOAT, [1, "T", 768])
    plen = helper.make_tensor_value_info("phone_lengths", TensorProto.INT64, [1])
    pitch = helper.make_tensor_value_info("pitch", TensorProto.INT64, [1, "T"])
    pitchf = helper.make_tensor_value_info("pitchf", TensorProto.FLOAT, [1, "T"])
    ds = helper.make_tensor_value_info("ds", TensorProto.INT64, [1])
    audio = helper.make_tensor_value_info("audio", TensorProto.FLOAT, [1, 1, "M"])

    Wm = r.uniform(-0.02, 0.02, size=(768, 960)).astype(np.float32)
    w_init = onnx.numpy_helper.from_array(Wm, name="Wm")
    shape = onnx.numpy_helper.from_array(np.array([1, 1, -1], dtype=np.int64), name="shape")
    n1 = helper.make_node("MatMul", ["phone", "Wm"], ["mm"])
    n2 = helper.make_node("Reshape", ["mm", "shape"], ["audio"])
    graph = helper.make_graph(
        [n1, n2], "gen_stub", [phone, plen, pitch, pitchf, ds], [audio],
        initializer=[w_init, shape])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 8
    onnx.save(model, path)
    print("wrote", path)


if __name__ == "__main__":
    make_content("content.onnx")
    make_gen("voice_alpha.onnx", seed=1)
    make_gen("voice_beta.onnx", seed=2)
