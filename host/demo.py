"""VelaPaw board-less demo: recognize pets with the exported INT8 model.

Mirrors the on-device logic exactly (enroll = averaged L2-normalized embedding;
recognize = cosine vs enrolled, threshold -> 'unknown'), but runs the same
.tflite on the PC. Two modes:

  images  : enroll from --enroll-dir, classify --test-dir, save a results grid
  webcam  : live recognition from the webcam (needs opencv-python)

    python demo.py --variant accurate --shots 5
    python demo.py --variant accurate --webcam        # live (pip install opencv-python)
"""

import argparse
import glob
import json
import os
import numpy as np
import tensorflow as tf

NORM_MEAN, NORM_STD = 127.5, 127.5
MAX_PETS = 0   # 0 = all; set via --max-pets (the feeder scenario is ~2-3)


class Recognizer:
    """Loads an INT8 tflite embedding model; enroll + cosine match (= device)."""

    def __init__(self, variant, out_dir, input_size, channels):
        self.input_size, self.channels = input_size, channels
        tfl = os.path.join(out_dir, variant, f"{variant}.tflite")
        self.interp = tf.lite.Interpreter(model_path=tfl)
        self.interp.allocate_tensors()
        self.inp = self.interp.get_input_details()[0]
        self.out = self.interp.get_output_details()[0]
        meta_p = os.path.join(out_dir, variant, "meta.json")
        meta = json.load(open(meta_p)) if os.path.exists(meta_p) else {}
        self.threshold = meta.get("match_threshold") or 0.5
        self.names, self.centroids = [], None

    def _prep(self, img_float):  # img_float HxWxC in pixel space
        x = (img_float - NORM_MEAN) / NORM_STD
        s, z = self.inp["quantization"]
        return np.round(x / s + z).astype(self.inp["dtype"])[None, ...]

    def embed(self, img_float):
        self.interp.set_tensor(self.inp["index"], self._prep(img_float))
        self.interp.invoke()
        s, z = self.out["quantization"]
        y = (self.interp.get_tensor(self.out["index"])[0].astype(np.float32) - z) * s
        return y / (np.linalg.norm(y) + 1e-9)

    def _load(self, path):
        color = "grayscale" if self.channels == 1 else "rgb"
        img = tf.keras.utils.load_img(path, color_mode=color,
                                      target_size=(self.input_size, self.input_size))
        a = np.asarray(img, np.float32)
        return a[..., None] if self.channels == 1 else a

    def enroll_vectors(self, name, embs):
        c = np.mean(embs, axis=0)
        c = c / (np.linalg.norm(c) + 1e-9)
        self.names.append(name)
        self.centroids = (c[None] if self.centroids is None
                          else np.vstack([self.centroids, c]))

    def enroll(self, name, paths):
        self.enroll_vectors(name, [self.embed(self._load(p)) for p in paths])

    def embed_bgr(self, frame_bgr):
        """Embed an OpenCV BGR frame (resize + BGR->RGB)."""
        import cv2
        rgb = cv2.cvtColor(cv2.resize(frame_bgr, (self.input_size, self.input_size)),
                           cv2.COLOR_BGR2RGB).astype(np.float32)
        if self.channels == 1:
            rgb = np.mean(rgb, axis=2, keepdims=True)
        return self.embed(rgb)

    def recognize(self, img_float):
        return self._match(self.embed(img_float))

    def recognize_bgr(self, frame_bgr):
        return self._match(self.embed_bgr(frame_bgr))

    def _match(self, e):
        sims = self.centroids @ e
        i = int(np.argmax(sims))
        score = float(sims[i])
        return (self.names[i] if score >= self.threshold else "unknown"), score


def run_images(rec, enroll_dir, test_dir, shots, per_test, report):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    classes = sorted(d for d in os.listdir(enroll_dir)
                     if os.path.isdir(os.path.join(enroll_dir, d)))
    if MAX_PETS:
        classes = classes[:MAX_PETS]
    for c in classes:
        files = sorted(glob.glob(os.path.join(enroll_dir, c, "*")))[:shots]
        if files:
            rec.enroll(c, files)
    print(f"[demo] enrolled {len(rec.names)} pets: {', '.join(rec.names)}")

    tests = []
    for c in classes:
        for p in sorted(glob.glob(os.path.join(test_dir, c, "*")))[:per_test]:
            tests.append((c, p))

    correct = 0
    shown = tests[:min(12, len(tests))]
    cols, rows = 4, (len(shown) + 3) // 4
    fig, axes = plt.subplots(rows, cols, figsize=(cols * 2.4, rows * 2.6))
    axes = np.array(axes).reshape(-1)
    for ax in axes:
        ax.axis("off")
    for k, (truth, p) in enumerate(tests):
        pred, score = rec.recognize(rec._load(p))
        correct += (pred == truth)
        if k < len(shown):
            img = tf.keras.utils.load_img(p, target_size=(96, 96))
            ax = axes[k]
            ax.imshow(img)
            ok = pred == truth
            ax.set_title(f"{pred} {score:.2f}\n({'OK' if ok else 'X true:'+truth})",
                         color=("green" if ok else "red"), fontsize=8)
    acc = correct / max(len(tests), 1)
    fig.suptitle(f"VelaPaw recognition demo — {rec.threshold:.2f} thresh — "
                 f"top-1 acc {acc:.0%} ({correct}/{len(tests)})", fontsize=11)
    fig.tight_layout()
    fig.savefig(report, dpi=110)
    print(f"[demo] top-1 accuracy: {acc:.1%} ({correct}/{len(tests)})")
    print(f"[demo] results image -> {report}")


def run_webcam(rec, enroll_dir, shots, enroll_live):
    try:
        import cv2
    except ImportError:
        raise SystemExit("webcam mode needs: pip install opencv-python")
    cap = cv2.VideoCapture(0)
    if not cap.isOpened():
        raise SystemExit("cannot open webcam (camera index 0)")

    if enroll_live:
        # Live enroll: point the camera at each pet, press SPACE to grab shots.
        for name in enroll_live:
            embs = []
            print(f"[enroll] {name}: SPACE=capture ({shots} needed), N=next, q=quit")
            while len(embs) < shots:
                ok, frame = cap.read()
                if not ok:
                    break
                disp = frame.copy()
                cv2.putText(disp, f"Enroll {name}: {len(embs)}/{shots}  "
                            "[SPACE]=capture [N]=next", (10, 30),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 255), 2)
                cv2.imshow("VelaPaw enroll", disp)
                k = cv2.waitKey(1) & 0xFF
                if k == ord(" "):
                    embs.append(rec.embed_bgr(frame))
                    print(f"  captured {len(embs)}/{shots}")
                elif k == ord("n"):
                    break
                elif k == ord("q"):
                    cap.release(); cv2.destroyAllWindows(); return
            if embs:
                rec.enroll_vectors(name, embs)
                print(f"[enroll] {name} enrolled ({len(embs)} shots)")
        try:
            cv2.destroyWindow("VelaPaw enroll")
        except Exception:
            pass
    else:
        # Enroll from folders of photos (one folder per pet).
        classes = sorted(d for d in os.listdir(enroll_dir)
                         if os.path.isdir(os.path.join(enroll_dir, d)))
        for c in classes:
            files = sorted(glob.glob(os.path.join(enroll_dir, c, "*")))[:shots]
            if files:
                rec.enroll(c, files)

    if not rec.names:
        raise SystemExit("no pets enrolled")
    print(f"[demo] recognizing {rec.names} (threshold {rec.threshold:.2f}); q=quit")

    while True:
        ok, frame = cap.read()
        if not ok:
            break
        pred, score = rec.recognize_bgr(frame)
        color = (0, 255, 0) if pred != "unknown" else (0, 0, 255)
        cv2.putText(frame, f"{pred}  {score:.2f}", (10, 40),
                    cv2.FONT_HERSHEY_SIMPLEX, 1.1, color, 2)
        cv2.imshow("VelaPaw demo (q=quit)", frame)
        if cv2.waitKey(1) & 0xFF == ord("q"):
            break
    cap.release()
    cv2.destroyAllWindows()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", choices=["fast", "accurate"], default="accurate")
    ap.add_argument("--enroll-dir", default="data/train")
    ap.add_argument("--test-dir", default="data/val")
    ap.add_argument("--shots", type=int, default=5, help="enroll images per pet")
    ap.add_argument("--per-test", type=int, default=4, help="test images per pet")
    ap.add_argument("--input-size", type=int, default=96)
    ap.add_argument("--channels", type=int, default=3, choices=[1, 3])
    ap.add_argument("--out", default="export")
    ap.add_argument("--report", default="demo_results.png")
    ap.add_argument("--webcam", action="store_true")
    ap.add_argument("--enroll-live", default="",
                    help="comma-separated pet names to enroll live from webcam, "
                         "e.g. --enroll-live \"Rex,Milo\" (implies --webcam)")
    ap.add_argument("--max-pets", type=int, default=0,
                    help="limit enrolled pets (feeder scenario ~2-3; 0=all)")
    args = ap.parse_args()
    enroll_live = [s.strip() for s in args.enroll_live.split(",") if s.strip()]

    global MAX_PETS
    MAX_PETS = args.max_pets

    rec = Recognizer(args.variant, args.out, args.input_size, args.channels)
    print(f"[demo] model={args.variant}  threshold={rec.threshold:.3f}")
    if args.webcam or enroll_live:
        run_webcam(rec, args.enroll_dir, args.shots, enroll_live)
    else:
        run_images(rec, args.enroll_dir, args.test_dir, args.shots,
                   args.per_test, args.report)


if __name__ == "__main__":
    main()
