"""Fetch a public pet dataset (Oxford-IIIT Pet, 37 breeds ~7.4k images) and
arrange it into the layout train_embedding.py expects:

    data/train/<breed>/*.jpg
    data/val/<breed>/*.jpg
    data/repr/*.jpg            (PTQ calibration sample)

Breeds stand in as "identities" to pre-train a discriminative pet embedding;
enroll your own pets on-device later. Stdlib only.

    python fetch_dataset.py            # download + arrange into data/
    python fetch_dataset.py --limit 15 # only first 15 breeds (faster CPU train)
"""

import argparse
import os
import re
import shutil
import tarfile
import urllib.request

URL = "https://thor.robots.ox.ac.uk/~vgg/data/pets/images.tar.gz"
ROOT = "data"
VAL_EVERY = 5          # ~20% to val
REPR_MAX = 200


def download(dst):
    if os.path.exists(dst):
        print(f"[fetch] already have {dst}")
        return
    print(f"[fetch] downloading {URL} (~800 MB)...")
    def hook(b, bs, total):
        if total > 0 and b % 500 == 0:
            print(f"\r  {b*bs/1e6:.0f}/{total/1e6:.0f} MB", end="")
    urllib.request.urlretrieve(URL, dst, hook)
    print("\n[fetch] done")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=0, help="max breeds (0=all)")
    args = ap.parse_args()

    os.makedirs(ROOT, exist_ok=True)
    tgz = os.path.join(ROOT, "images.tar.gz")
    download(tgz)

    print("[fetch] extracting...")
    raw = os.path.join(ROOT, "_raw")
    with tarfile.open(tgz) as t:
        t.extractall(raw)
    img_dir = os.path.join(raw, "images")

    # Group by breed (filename prefix before the trailing _<number>.jpg).
    breeds = {}
    for f in sorted(os.listdir(img_dir)):
        if not f.lower().endswith(".jpg"):
            continue
        m = re.match(r"^(.*)_\d+\.jpg$", f)
        if not m:
            continue
        breeds.setdefault(m.group(1).lower(), []).append(f)

    names = sorted(breeds)
    if args.limit:
        names = names[:args.limit]
    print(f"[fetch] {len(names)} breeds")

    repr_dir = os.path.join(ROOT, "repr")
    os.makedirs(repr_dir, exist_ok=True)
    repr_n = 0
    for bi, b in enumerate(names):
        files = breeds[b]
        for i, f in enumerate(files):
            split = "val" if i % VAL_EVERY == 0 else "train"
            dd = os.path.join(ROOT, split, b)
            os.makedirs(dd, exist_ok=True)
            try:
                shutil.copy(os.path.join(img_dir, f), os.path.join(dd, f))
            except Exception:
                continue
            if split == "train" and repr_n < REPR_MAX and i % 7 == 0:
                shutil.copy(os.path.join(img_dir, f),
                            os.path.join(repr_dir, f))
                repr_n += 1

    shutil.rmtree(raw, ignore_errors=True)
    print(f"[fetch] arranged into {ROOT}/train, {ROOT}/val, {ROOT}/repr "
          f"({repr_n} calib imgs)")
    print("[fetch] next: python train_embedding.py --variant accurate "
          "--data data/train --epochs 20")


if __name__ == "__main__":
    main()
