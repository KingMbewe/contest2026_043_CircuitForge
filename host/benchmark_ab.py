"""A/B benchmark: fast (MobileNetV3-Small) vs accurate (+ TSFM-Enhanced).

Quantifies the operator's TSFM tradeoff with real numbers from the exported
INT8 models: model size, op count, host inference latency (mean/p50/p95), and
embedding separability (intra/inter cosine, EER threshold) on the val set.

Writes a comparison table to docs/BENCHMARK_AB.md.

    python benchmark_ab.py --val data/val

NOTE: latency here is HOST CPU (relative tradeoff only); absolute on-device
numbers must be measured on the ESP32-S3 (see docs/TFLM_SPIKE_REPORT.md). The
device app surfaces real arena_used + latency via the infer-backend interface.
"""

import argparse
import glob
import os
import time
import numpy as np
import tensorflow as tf
import models  # noqa: F401  registers custom layers (not needed for tflite, harmless)

NORM_MEAN, NORM_STD = 127.5, 127.5


def load_val(data_dir, input_size, channels):
    color = "grayscale" if channels == 1 else "rgb"
    items = []
    classes = sorted(d for d in os.listdir(data_dir)
                     if os.path.isdir(os.path.join(data_dir, d)))
    for ci, c in enumerate(classes):
        for f in glob.glob(os.path.join(data_dir, c, "*")):
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


def measure(tflite_path, items, runs):
    interp = tf.lite.Interpreter(model_path=tflite_path)
    interp.allocate_tensors()
    inp = interp.get_input_details()[0]
    out = interp.get_output_details()[0]
    in_scale, in_zp = inp["quantization"]
    out_scale, out_zp = out["quantization"]
    ops = sorted({d["op_name"] for d in interp._get_ops_details()
                  if d["op_name"] != "DELEGATE"})

    def to_q(x):
        return np.round(x / in_scale + in_zp).astype(inp["dtype"])[None, ...]

    # Latency (warmup + timed runs on the first sample).
    sample = to_q(items[0][1])
    for _ in range(3):
        interp.set_tensor(inp["index"], sample); interp.invoke()
    ts = []
    for _ in range(runs):
        t0 = time.perf_counter()
        interp.set_tensor(inp["index"], sample); interp.invoke()
        ts.append((time.perf_counter() - t0) * 1000.0)
    ts = np.array(ts)

    # Separability over the val set.
    embs, labels = [], []
    for label, x in items:
        interp.set_tensor(inp["index"], to_q(x)); interp.invoke()
        y = (interp.get_tensor(out["index"])[0].astype(np.float32) - out_zp) * out_scale
        y = y / (np.linalg.norm(y) + 1e-9)
        embs.append(y); labels.append(label)
    embs, labels = np.array(embs), np.array(labels)
    sims = embs @ embs.T
    intra, inter = [], []
    n = len(labels)
    for i in range(n):
        for j in range(i + 1, n):
            (intra if labels[i] == labels[j] else inter).append(sims[i, j])
    intra, inter = np.array(intra), np.array(inter)
    best_t, best_gap = 0.5, 1e9
    for t in np.linspace(-0.2, 1.0, 121):
        far = (inter >= t).mean(); frr = (intra < t).mean()
        if abs(far - frr) < best_gap:
            best_gap, best_t = abs(far - frr), float(t)
    eer = ((inter >= best_t).mean() + (intra < best_t).mean()) / 2

    return {
        "size_kb": os.path.getsize(tflite_path) / 1024.0,
        "ops": ops, "n_ops": len(ops),
        "lat_mean": ts.mean(), "lat_p50": np.percentile(ts, 50),
        "lat_p95": np.percentile(ts, 95),
        "intra": intra.mean(), "inter": inter.mean(),
        "margin": intra.mean() - inter.mean(),
        "threshold": best_t, "eer": eer,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variants", nargs="+", default=["fast", "accurate"])
    ap.add_argument("--val", default="data/val")
    ap.add_argument("--input-size", type=int, default=96)
    ap.add_argument("--channels", type=int, default=3, choices=[1, 3])
    ap.add_argument("--runs", type=int, default=50)
    ap.add_argument("--out", default="export")
    ap.add_argument("--report", default="../docs/BENCHMARK_AB.md")
    args = ap.parse_args()

    classes, items = load_val(args.val, args.input_size, args.channels)
    print(f"[bench] val: {len(items)} imgs, {len(classes)} identities, "
          f"{args.runs} latency runs/model\n")

    rows = {}
    for v in args.variants:
        tfl = os.path.join(args.out, v, f"{v}.tflite")
        if not os.path.exists(tfl):
            print(f"[bench] SKIP {v}: {tfl} not found (train+export it first)")
            continue
        rows[v] = measure(tfl, items, args.runs)

    if not rows:
        return

    # Console + markdown table.
    hdr = ["metric"] + list(rows)
    def line(name, fmt):
        return [name] + [fmt(rows[v]) for v in rows]
    table = [
        line("tflite size (KB)", lambda r: f"{r['size_kb']:.0f}"),
        line("ops (count)",      lambda r: f"{r['n_ops']}"),
        line("latency mean (ms)", lambda r: f"{r['lat_mean']:.1f}"),
        line("latency p95 (ms)",  lambda r: f"{r['lat_p95']:.1f}"),
        line("intra cosine",      lambda r: f"{r['intra']:.3f}"),
        line("inter cosine",      lambda r: f"{r['inter']:.3f}"),
        line("separation margin", lambda r: f"{r['margin']:.3f}"),
        line("EER threshold",     lambda r: f"{r['threshold']:.3f}"),
        line("EER",               lambda r: f"{r['eer']:.3f}"),
    ]
    widths = [max(len(str(c)) for c in col) for col in zip(hdr, *table)]
    def render(cells):
        return " | ".join(str(c).ljust(w) for c, w in zip(cells, widths))
    print(render(hdr)); print("-+-".join("-" * w for w in widths))
    for row in table:
        print(render(row))

    md = ["# Benchmark A/B — fast vs accurate (TSFM-Enhanced)", "",
          f"Host CPU, INT8 models, val={len(items)} imgs / {len(classes)} ids, "
          f"{args.runs} latency runs. Latency is **relative** (host, not ESP32-S3).",
          "", "| " + " | ".join(hdr) + " |",
          "|" + "|".join("---" for _ in hdr) + "|"]
    for row in table:
        md.append("| " + " | ".join(str(c) for c in row) + " |")
    md += ["", "Ops (TFLite-Micro must support all):"]
    for v in rows:
        md.append(f"- **{v}**: {', '.join(rows[v]['ops'])}")
    os.makedirs(os.path.dirname(args.report), exist_ok=True)
    open(args.report, "w").write("\n".join(md) + "\n")
    print(f"\n[bench] wrote {args.report}")


if __name__ == "__main__":
    main()
