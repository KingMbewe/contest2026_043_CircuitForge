"""Scrape REAL cat photos for BCS classes from Reddit, classifying before save.

Generic web image search returned ~no real cats (memes/quote-graphics). Reddit
cat communities are ~100% real cat photos:
  over  -> r/chonkers, r/thicccats   (genuinely overweight cats)
  under -> Reddit search for underweight/emaciated/rescue cats (no dedicated sub,
           so condition still needs a manual cull; the classifier guarantees cat)

Uses Reddit's public .json listings (no API key) with a descriptive User-Agent.
Every image is run through the cat classifier; only cats are written.

Run:
    .venv/Scripts/python.exe reddit_scrape.py --per 200
    .venv/Scripts/python.exe reddit_scrape.py --only over --per 250
"""

import argparse
import io
import json
import os
import time
import urllib.request
from urllib.parse import quote

import numpy as np
from PIL import Image
from tensorflow.keras.applications.mobilenet_v2 import (
    MobileNetV2, preprocess_input, decode_predictions)

HERE = os.path.dirname(os.path.abspath(__file__))
DST = os.path.join(HERE, "bcs_dataset")
UA = "velapaw-bcs-research/0.1 (contest dataset; contact kingsleymbewe998@gmail.com)"

CAT_LABELS = {"tabby", "tiger_cat", "Persian_cat", "Siamese_cat", "Egyptian_cat"}
_model = MobileNetV2(weights="imagenet")

# (kind, name): kind 'sub' = subreddit top listing, 'search' = sitewide search
SOURCES = {
    "over":  [("sub", "chonkers"), ("sub", "thicccats"), ("sub", "chonky"),
              ("search", "obese cat vet")],
    "under": [("search", "underweight cat"), ("search", "emaciated cat rescue"),
              ("search", "skinny rescue cat"), ("search", "malnourished cat")],
}


def fetch_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=25) as r:
        return json.load(r)


def listing_url(kind, name, after):
    if kind == "sub":
        u = f"https://www.reddit.com/r/{name}/top.json?t=all&limit=100"
    else:
        u = (f"https://www.reddit.com/search.json?q={quote(name)}"
             f"&sort=top&t=all&limit=100&type=link")
    if after:
        u += f"&after={after}"
    return u


def image_urls(d):
    """Pull direct image URLs from a Reddit post's data dict."""
    out = []
    url = d.get("url_overridden_by_dest") or d.get("url") or ""
    base = url.lower().split("?")[0]
    if "v.redd.it" not in url and base.endswith((".jpg", ".jpeg", ".png")):
        out.append(url)
    if d.get("is_gallery") and d.get("media_metadata"):
        for m in d["media_metadata"].values():
            u = (m.get("s") or {}).get("u")
            if u:
                out.append(u.replace("&amp;", "&"))
    if not out:
        imgs = d.get("preview", {}).get("images", [])
        if imgs:
            u = imgs[0].get("source", {}).get("url")
            if u:
                out.append(u.replace("&amp;", "&"))
    return out


def is_cat(b):
    try:
        img = Image.open(io.BytesIO(b)).convert("RGB").resize((224, 224))
    except Exception:
        return False
    x = preprocess_input(np.expand_dims(np.asarray(img, "float32"), 0))
    top = decode_predictions(_model.predict(x, verbose=0), top=10)[0]
    return any(label in CAT_LABELS for (_, label, _) in top)


def download(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=25) as r:
        return r.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per", type=int, default=200, help="target cats per class")
    ap.add_argument("--only", choices=["under", "over"])
    args = ap.parse_args()

    classes = [args.only] if args.only else ["over", "under"]
    for cls in classes:
        out = os.path.join(DST, cls)
        os.makedirs(out, exist_ok=True)
        idx = len([f for f in os.listdir(out) if f.lower().endswith(
            (".jpg", ".jpeg", ".png"))])
        kept = 0
        seen = set()
        print(f"\n### class '{cls}' target {args.per} ###")
        for kind, name in SOURCES[cls]:
            after = None
            for _page in range(6):
                try:
                    data = fetch_json(listing_url(kind, name, after))
                except Exception as e:
                    print(f"  [{kind}:{name}] listing failed: {e}")
                    break
                children = data.get("data", {}).get("children", [])
                if not children:
                    break
                for ch in children:
                    for u in image_urls(ch.get("data", {})):
                        if u in seen:
                            continue
                        seen.add(u)
                        try:
                            b = download(u)
                        except Exception:
                            continue
                        if is_cat(b):
                            idx += 1
                            kept += 1
                            with open(os.path.join(out, f"r_{cls}_{idx:05d}.jpg"),
                                      "wb") as fh:
                                fh.write(b)
                        time.sleep(0.4)   # be polite to Reddit
                        if kept >= args.per:
                            break
                    if kept >= args.per:
                        break
                print(f"  [{kind}:{name}] kept so far: {kept}")
                after = data.get("data", {}).get("after")
                if not after or kept >= args.per:
                    break
                time.sleep(1.0)
            if kept >= args.per:
                break
        total = len([f for f in os.listdir(out) if f.lower().endswith(
            (".jpg", ".jpeg", ".png"))])
        print(f"### '{cls}' done: kept {kept} this run, {total} cat images total")


if __name__ == "__main__":
    main()
