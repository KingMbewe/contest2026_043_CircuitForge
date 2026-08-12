"""Keep only images a classifier recognizes as a (domestic) cat.

Bing image search for "obese cat" / "thin cat" returns lots of non-cats (dogs,
people, charts). This runs an ImageNet MobileNetV2 over each candidate and, if
none of the top-5 predictions is a domestic-cat class, MOVES it to a _rejected/
subfolder (reversible - review _rejected/ if you think a real cat was dropped).

Run after scrape_bcs.py:
    .venv/Scripts/python.exe filter_cats.py            # filters over/ and under/
    .venv/Scripts/python.exe filter_cats.py --only under
    .venv/Scripts/python.exe filter_cats.py --dirs ideal over under
"""

import argparse
import os
import shutil

import numpy as np
from tensorflow.keras.applications.mobilenet_v2 import (
    MobileNetV2, preprocess_input, decode_predictions)
from tensorflow.keras.preprocessing import image as kimage

HERE = os.path.dirname(os.path.abspath(__file__))
DST = os.path.join(HERE, "bcs_dataset")

# ImageNet domestic-cat synsets (wild cats like lynx/cougar/tiger excluded)
CAT_LABELS = {"tabby", "tiger_cat", "Persian_cat", "Siamese_cat", "Egyptian_cat"}
EXTS = (".jpg", ".jpeg", ".png", ".bmp", ".webp")


def is_cat(model, path):
    try:
        img = kimage.load_img(path, target_size=(224, 224))
    except Exception:
        return False   # unreadable/corrupt -> reject
    x = preprocess_input(np.expand_dims(kimage.img_to_array(img), 0))
    preds = decode_predictions(model.predict(x, verbose=0), top=5)[0]
    return any(label in CAT_LABELS for (_, label, _) in preds)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", choices=["under", "over", "ideal"])
    ap.add_argument("--dirs", nargs="*", help="explicit class dirs to filter")
    args = ap.parse_args()

    dirs = args.dirs or ([args.only] if args.only else ["over", "under"])
    model = MobileNetV2(weights="imagenet")

    for cls in dirs:
        d = os.path.join(DST, cls)
        if not os.path.isdir(d):
            continue
        rej = os.path.join(d, "_rejected")
        os.makedirs(rej, exist_ok=True)
        kept = moved = 0
        for f in sorted(os.listdir(d)):
            p = os.path.join(d, f)
            if not os.path.isfile(p) or not f.lower().endswith(EXTS):
                continue
            if is_cat(model, p):
                kept += 1
            else:
                shutil.move(p, os.path.join(rej, f))
                moved += 1
        print(f"{cls}/: kept {kept} cats, moved {moved} non-cats -> _rejected/")


if __name__ == "__main__":
    main()
