"""Add a validated subset of the Kaggle Cats vs Dogs set into host/data/ as two
extra classes (Cat, Dog), alongside the Oxford breeds. Skips corrupt images
(the Kaggle set has a few). Run after fetch_dataset.py (breeds).

    python prep_catsdogs.py --src "C:/Users/VICTUS/Downloads/archive/PetImages"
"""

import argparse
import glob
import os
import shutil
from PIL import Image

DEST = "data"


def valid(path):
    try:
        with Image.open(path) as im:
            im.verify()                      # detect truncated/corrupt
        return os.path.getsize(path) > 1024
    except Exception:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="C:/Users/VICTUS/Downloads/archive/PetImages")
    ap.add_argument("--train", type=int, default=180)
    ap.add_argument("--val", type=int, default=40)
    ap.add_argument("--repr", type=int, default=20)
    args = ap.parse_args()

    for cls in ("Cat", "Dog"):
        src = os.path.join(args.src, cls)
        files = sorted(glob.glob(os.path.join(src, "*.jpg")),
                       key=lambda p: int(os.path.splitext(os.path.basename(p))[0])
                       if os.path.splitext(os.path.basename(p))[0].isdigit() else 0)
        need = args.train + args.val
        copied = 0
        for f in files:
            if copied >= need:
                break
            if not valid(f):
                continue
            split = "val" if copied < args.val else "train"
            dd = os.path.join(DEST, split, cls)
            os.makedirs(dd, exist_ok=True)
            shutil.copy(f, os.path.join(dd, f"{cls}_{copied}.jpg"))
            if split == "train" and copied % 8 == 0:
                rdir = os.path.join(DEST, "repr")
                os.makedirs(rdir, exist_ok=True)
                shutil.copy(f, os.path.join(rdir, f"{cls}_{copied}.jpg"))
            copied += 1
        print(f"[catsdogs] {cls}: {copied} copied (train {args.train}/val {args.val})")


if __name__ == "__main__":
    main()
