"""Bulk-download candidate cat images for the BCS under/over classes.

Uses icrawler's Bing crawler (no API key). Results are NOISY by nature - after
running, REVIEW and delete the bad ones (face-only, wrong condition, watermarked,
not a cat) following LABELING_GUIDE.md. Target ~200 good images per class.

Licensing: these are web images - fine to train on locally for the contest, but
do NOT commit/redistribute them. Keep bcs_dataset/ out of git.

Setup (once):
    .venv/Scripts/python.exe -m pip install icrawler

Run:
    .venv/Scripts/python.exe scrape_bcs.py             # both classes, 80/term
    .venv/Scripts/python.exe scrape_bcs.py --per 120   # more per term
    .venv/Scripts/python.exe scrape_bcs.py --only under
"""

import argparse
import os

HERE = os.path.dirname(os.path.abspath(__file__))
DST = os.path.join(HERE, "bcs_dataset")

# Cat-ANCHORED phrasing so search returns cats, not generic thin/obese things.
# (Generic "underweight"/"obese" pulled people/sports/objects.) The filter_cats.py
# pass then drops any remaining non-cats.
TERMS = {
    "over":  ["fat cats", "fat cat"],
    "under": ["thin cats", "thin cat"],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per", type=int, default=80, help="images per search term")
    ap.add_argument("--only", choices=["under", "over"], help="just one class")
    args = ap.parse_args()

    from icrawler.builtin import BingImageCrawler
    CRAWLERS = [("bing", BingImageCrawler)]
    try:
        from icrawler.builtin import GoogleImageCrawler
        CRAWLERS.append(("google", GoogleImageCrawler))
    except Exception:
        pass

    classes = [args.only] if args.only else ["over", "under"]
    for cls in classes:
        out = os.path.join(DST, cls)
        os.makedirs(out, exist_ok=True)
        for term in TERMS[cls]:
            for name, Crawler in CRAWLERS:
                print(f"\n=== [{cls}] ({name}) '{term}'  ->  {out} ===")
                try:
                    crawler = Crawler(downloader_threads=4,
                                      storage={"root_dir": out})
                    # file_idx_offset='auto' continues numbering (no overwrite)
                    crawler.crawl(keyword=term, max_num=args.per,
                                  file_idx_offset="auto")
                except Exception as e:
                    print(f"  {name} crawler failed: {e}")

    print("\nDONE. Next: CULL each folder - delete face-only / wrong-condition / "
          "watermarked / non-cat images. Aim for ~200 good ones per class.")
    for cls in classes:
        out = os.path.join(DST, cls)
        n = len([f for f in os.listdir(out)
                 if f.lower().endswith((".jpg", ".jpeg", ".png"))])
        print(f"  {cls}/: {n} candidate images")


if __name__ == "__main__":
    main()
