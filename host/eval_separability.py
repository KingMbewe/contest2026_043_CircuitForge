"""Evaluate embedding separability and suggest a match THRESHOLD.

Embeds a labeled val set with the exported INT8 model, then compares the cosine
similarity distribution of same-identity pairs (intra) vs different-identity
pairs (inter). A good embedding has intra >> inter. Picks the threshold at the
equal-error rate and writes it into export/<variant>/meta.json.

Example:
    python eval_separability.py --variant accurate --data data/val
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


def load_split(data_dir, input_size, channels):
    color = "grayscale" if channels == 1 else "rgb"
    items = []  # (label, embedding-input)
    classes = sorted(d for d in os.listdir(data_dir)
                     if os.path.isdir(os.path.join(data_dir, d)))
    for ci, cname in enumerate(classes):
        for f in glob.glob(os.path.join(data_dir, cname, "*")):
            try:
                img = tf.keras.utils.load_img(
                    f, color_mode=color, target_size=(input_size, input_size))
            except Exception:
                continue
            x = (np.asarray(img, np.float32) - NORM_MEAN) / NORM_STD
            if channels == 1:
                x = x[..., None]
            items.append((ci, x))
    return classes, items


def embed_all(tflite_path, items):
    interp = tf.lite.Interpreter(model_path=tflite_path)
    interp.allocate_tensors()
    inp = interp.get_input_details()[0]
    out = interp.get_output_details()[0]
    in_scale, in_zp = inp["quantization"]
    out_scale, out_zp = out["quantization"]

    embs, labels = [], []
    for label, x in items:
        q = np.round(x / in_scale + in_zp).astype(inp["dtype"])
        interp.set_tensor(inp["index"], q[None, ...])
        interp.invoke()
        y = interp.get_tensor(out["index"])[0].astype(np.float32)
        y = (y - out_zp) * out_scale
        y = y / (np.linalg.norm(y) + 1e-9)   # L2-normalize (device does too)
        embs.append(y)
        labels.append(label)
    return np.array(embs), np.array(labels)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", choices=["fast", "accurate"], default="fast")
    ap.add_argument("--data", required=True, help="val dir (one folder/identity)")
    ap.add_argument("--input-size", type=int, default=96)
    ap.add_argument("--channels", type=int, default=3, choices=[1, 3])
    ap.add_argument("--out", default="export")
    args = ap.parse_args()

    out_dir = os.path.join(args.out, args.variant)
    tfl = os.path.join(out_dir, f"{args.variant}.tflite")
    classes, items = load_split(args.data, args.input_size, args.channels)
    embs, labels = embed_all(tfl, items)
    print(f"[eval] {len(items)} images, {len(classes)} identities")

    # All pairwise cosine similarities, split into intra/inter.
    sims = embs @ embs.T
    intra, inter = [], []
    n = len(labels)
    for i in range(n):
        for j in range(i + 1, n):
            (intra if labels[i] == labels[j] else inter).append(sims[i, j])
    intra, inter = np.array(intra), np.array(inter)
    print(f"[eval] intra  cosine: mean={intra.mean():.3f} std={intra.std():.3f}")
    print(f"[eval] inter  cosine: mean={inter.mean():.3f} std={inter.std():.3f}")

    # Equal-error-rate threshold sweep.
    best_t, best_gap = 0.6, 1e9
    for t in np.linspace(-0.2, 1.0, 121):
        far = (inter >= t).mean()   # accept different-identity (false accept)
        frr = (intra < t).mean()    # reject same-identity (false reject)
        if abs(far - frr) < best_gap:
            best_gap, best_t = abs(far - frr), float(t)
    print(f"[eval] suggested THRESHOLD ~= {best_t:.3f} (EER point)")

    meta_path = os.path.join(out_dir, "meta.json")
    if os.path.exists(meta_path):
        meta = json.load(open(meta_path))
        meta["match_threshold"] = round(best_t, 3)
        meta["separability"] = {
            "intra_mean": float(intra.mean()), "inter_mean": float(inter.mean())}
        json.dump(meta, open(meta_path, "w"), indent=2)
        print(f"[eval] wrote match_threshold into {meta_path}")


if __name__ == "__main__":
    main()
