"""Generate a tiny synthetic dataset to smoke-test the pipeline (NOT for real
training). Creates a few 'identities' with class-distinct patterns so the
model/export plumbing can be validated cheaply. Safe to delete."""

import os
import numpy as np
from PIL import Image

ROOT = "data_smoke"
CLASSES = ["pet_a", "pet_b", "pet_c"]
PER_CLASS = 10
SIZE = 96


def make_img(seed, base):
    rng = np.random.default_rng(seed)
    # class-distinct base color + noise so classes are separable-ish
    img = np.full((SIZE, SIZE, 3), base, np.uint8)
    img = np.clip(img.astype(int) + rng.integers(-30, 30, (SIZE, SIZE, 3)),
                  0, 255).astype(np.uint8)
    return Image.fromarray(img)


def main():
    for ci, c in enumerate(CLASSES):
        d = os.path.join(ROOT, "train", c)
        os.makedirs(d, exist_ok=True)
        base = np.array([60 + 70 * ci, 120, 200 - 60 * ci])
        for i in range(PER_CLASS):
            make_img(ci * 100 + i, base).save(os.path.join(d, f"{i:02d}.png"))

    rdir = os.path.join(ROOT, "repr")
    os.makedirs(rdir, exist_ok=True)
    for i in range(30):
        base = np.array([60 + 70 * (i % 3), 120, 200 - 60 * (i % 3)])
        make_img(1000 + i, base).save(os.path.join(rdir, f"{i:02d}.png"))

    print(f"smoke data: {len(CLASSES)} classes x {PER_CLASS} + 30 repr -> {ROOT}/")


if __name__ == "__main__":
    main()
