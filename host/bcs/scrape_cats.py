"""Scrape cat images for BCS classes, classifying BEFORE saving.

Each fetched image is run through an ImageNet MobileNetV2; if none of the top-10
predictions is a domestic-cat class, the image is DISCARDED (never written). So
over/ and under/ only ever receive actual cats - no post-cull of non-cats needed
(you still cull for body *condition*/pose).

Setup (once):
    .venv/Scripts/python.exe -m pip install icrawler

Run:
    .venv/Scripts/python.exe scrape_cats.py --per 200
    .venv/Scripts/python.exe scrape_cats.py --only under --per 300
"""

import argparse
import io
import os

import numpy as np
from PIL import Image
from tensorflow.keras.applications.mobilenet_v2 import (
    MobileNetV2, preprocess_input, decode_predictions)

from icrawler import ImageDownloader
from icrawler.builtin import BingImageCrawler
try:
    from icrawler.builtin import GoogleImageCrawler
    _HAS_GOOGLE = True
except Exception:
    _HAS_GOOGLE = False

HERE = os.path.dirname(os.path.abspath(__file__))
DST = os.path.join(HERE, "bcs_dataset")

TERMS = {
    "over":  ["fat cats", "fat cat"],
    "under": ["thin cats", "thin cat"],
}

CAT_LABELS = {"tabby", "tiger_cat", "Persian_cat", "Siamese_cat", "Egyptian_cat"}
_model = MobileNetV2(weights="imagenet")

# counters for reporting
_seen = {"kept": 0, "dropped": 0}


def _is_cat(img_bytes):
    try:
        img = Image.open(io.BytesIO(img_bytes)).convert("RGB").resize((224, 224))
    except Exception:
        return False
    x = preprocess_input(np.expand_dims(np.asarray(img, dtype="float32"), 0))
    top = decode_predictions(_model.predict(x, verbose=0), top=10)[0]
    return any(label in CAT_LABELS for (_, label, _) in top)


class CatOnlyDownloader(ImageDownloader):
    """Only keep the file if the classifier says it's a cat."""

    def keep_file(self, task, response, **kwargs):
        if _is_cat(response.content):
            _seen["kept"] += 1
            return True
        _seen["dropped"] += 1
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per", type=int, default=200,
                    help="candidates to fetch per term (cats kept will be fewer)")
    ap.add_argument("--only", choices=["under", "over"])
    args = ap.parse_args()

    crawlers = [("bing", BingImageCrawler)]
    if _HAS_GOOGLE:
        crawlers.append(("google", GoogleImageCrawler))

    classes = [args.only] if args.only else ["over", "under"]
    for cls in classes:
        out = os.path.join(DST, cls)
        os.makedirs(out, exist_ok=True)
        for term in TERMS[cls]:
            for name, Crawler in crawlers:
                print(f"\n=== [{cls}] ({name}) '{term}'  (cat-only) ===")
                try:
                    c = Crawler(downloader_cls=CatOnlyDownloader,
                                downloader_threads=1,   # single thread: TF-safe
                                storage={"root_dir": out})
                    c.crawl(keyword=term, max_num=args.per,
                            file_idx_offset="auto")
                except Exception as e:
                    print(f"  {name} crawler failed: {e}")

    print(f"\nDONE. classifier kept {_seen['kept']} cats, "
          f"dropped {_seen['dropped']} non-cats.")
    for cls in classes:
        out = os.path.join(DST, cls)
        n = len([f for f in os.listdir(out)
                 if f.lower().endswith((".jpg", ".jpeg", ".png"))])
        print(f"  {cls}/: {n} cat images total")


if __name__ == "__main__":
    main()
