"""INT8 post-training quantization + export for a VelaPaw embedding model.

Produces, in export/<variant>/:
  <variant>.tflite              INT8 model (int8 in/out)
  <variant>_model_data.{cc,h}   C array for embedding in the device app
  meta.json                     input shape / normalization / embed dim / ops /
                                output quant params / suggested threshold slot

Example:
    python quantize_export.py --variant fast --repr data/repr
"""

import argparse
import glob
import json
import os
import numpy as np
import tensorflow as tf
import models  # noqa: F401  registers custom Keras layers (TSFM/HardSwish/L2Normalize)

NORM_MEAN = 127.5
NORM_STD = 127.5


def representative_dataset(repr_dir, input_size, channels):
    files = sorted(glob.glob(os.path.join(repr_dir, "*")))
    if not files:
        raise SystemExit(f"no calibration images in {repr_dir}")
    color = "grayscale" if channels == 1 else "rgb"

    def gen():
        for f in files[:300]:
            try:
                img = tf.keras.utils.load_img(
                    f, color_mode=color, target_size=(input_size, input_size))
            except Exception:
                continue
            x = (np.asarray(img, np.float32) - NORM_MEAN) / NORM_STD
            if channels == 1:
                x = x[..., None]
            yield [x[None, ...]]
    return gen


def write_c_array(tflite_bytes, name, cc_path, h_path):
    arr = ", ".join(f"0x{b:02x}" for b in tflite_bytes)
    with open(cc_path, "w") as f:
        f.write(f'#include "{os.path.basename(h_path)}"\n\n')
        f.write(f"alignas(16) const unsigned char g_{name}_model_data[] = "
                f"{{{arr}}};\n")
        f.write(f"const unsigned int g_{name}_model_data_size = "
                f"{len(tflite_bytes)};\n")
    with open(h_path, "w") as f:
        f.write("#include <cstdint>\n\n")
        f.write(f"extern const unsigned char g_{name}_model_data[];\n")
        f.write(f"extern const unsigned int g_{name}_model_data_size;\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", choices=["fast", "accurate"], default="fast")
    ap.add_argument("--repr", required=True, help="calibration image dir")
    ap.add_argument("--model", help="embedding .keras (default export/<v>/embedding.keras)")
    ap.add_argument("--input-size", type=int, default=96)
    ap.add_argument("--channels", type=int, default=3, choices=[1, 3])
    ap.add_argument("--embed-dim", type=int, default=128)
    ap.add_argument("--out", default="export")
    args = ap.parse_args()

    out_dir = os.path.join(args.out, args.variant)
    os.makedirs(out_dir, exist_ok=True)
    model_path = args.model or os.path.join(out_dir, "embedding.keras")
    model = tf.keras.models.load_model(model_path, compile=False)

    conv = tf.lite.TFLiteConverter.from_keras_model(model)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = representative_dataset(
        args.repr, args.input_size, args.channels)
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    tflite_bytes = conv.convert()

    tfl_path = os.path.join(out_dir, f"{args.variant}.tflite")
    with open(tfl_path, "wb") as f:
        f.write(tflite_bytes)

    write_c_array(tflite_bytes, args.variant,
                  os.path.join(out_dir, f"{args.variant}_model_data.cc"),
                  os.path.join(out_dir, f"{args.variant}_model_data.h"))

    # Inspect the model: op list (verify TFLM support) + output quant params.
    interp = tf.lite.Interpreter(model_content=tflite_bytes)
    interp.allocate_tensors()
    ops = sorted({d["op_name"] for d in interp._get_ops_details()})
    out_det = interp.get_output_details()[0]
    oscale, ozp = out_det["quantization"]

    meta = {
        "name": args.variant,
        "variant": args.variant,
        "input_size": args.input_size,
        "channels": args.channels,
        "embed_dim": args.embed_dim,
        "normalization": {"mean": NORM_MEAN, "std": NORM_STD},
        "input_dtype": "int8",
        "output_dtype": "int8",
        "output_scale": float(oscale),
        "output_zero_point": int(ozp),
        "ops": ops,
        "match_threshold": None,   # fill from eval_separability.py
    }
    with open(os.path.join(out_dir, "meta.json"), "w") as f:
        json.dump(meta, f, indent=2)

    print(f"[export] {tfl_path}  ({len(tflite_bytes)} bytes)")
    print(f"[export] ops (verify all are in TFLite-Micro): {ops}")
    print(f"[export] meta.json written; next: eval_separability.py")


if __name__ == "__main__":
    main()
