"""Train a VelaPaw embedding model with an ArcFace margin head.

Only the backbone->embedding sub-model is saved (the head is training-only).

Example:
    python train_embedding.py --variant fast --data data/train --epochs 30
"""

import argparse
import os
import tensorflow as tf

from models import build_embedding_model, build_training_model

NORM_MEAN = 127.5   # images scaled to [-1, 1]; recorded in meta.json by export
NORM_STD = 127.5


def make_dataset(data_dir, input_size, batch, channels):
    color = "rgb" if channels == 3 else "grayscale"
    ds = tf.keras.utils.image_dataset_from_directory(
        data_dir, labels="inferred", label_mode="int",
        color_mode=color, image_size=(input_size, input_size),
        batch_size=batch, shuffle=True)
    num_classes = len(ds.class_names)

    norm = lambda x: (tf.cast(x, tf.float32) - NORM_MEAN) / NORM_STD
    # ArcFace model takes (image, label) and predicts logits; target is label.
    ds = ds.map(lambda x, y: ((norm(x), y), y),
                num_parallel_calls=tf.data.AUTOTUNE)
    return ds.prefetch(tf.data.AUTOTUNE), num_classes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", choices=["fast", "accurate"], default="fast")
    ap.add_argument("--data", required=True, help="train dir (one folder/identity)")
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--input-size", type=int, default=96)
    ap.add_argument("--embed-dim", type=int, default=128)
    ap.add_argument("--channels", type=int, default=3, choices=[1, 3])
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--alpha", type=float, default=1.0)
    ap.add_argument("--full", action="store_true",
                    help="hard-swish + SE (default minimalistic: relu, no SE)")
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--out", default="export")
    args = ap.parse_args()

    ds, num_classes = make_dataset(args.data, args.input_size, args.batch,
                                   args.channels)
    print(f"[train] {num_classes} identities, variant={args.variant}, "
          f"minimalistic={not args.full}")

    embedding = build_embedding_model(
        variant=args.variant, input_size=args.input_size,
        channels=args.channels, embed_dim=args.embed_dim, alpha=args.alpha,
        minimalistic=not args.full)
    train_model = build_training_model(embedding, num_classes)

    train_model.compile(
        optimizer=tf.keras.optimizers.Adam(args.lr),
        loss=tf.keras.losses.SparseCategoricalCrossentropy(from_logits=True),
        metrics=["accuracy"])
    train_model.fit(ds, epochs=args.epochs)

    out_dir = os.path.join(args.out, args.variant)
    os.makedirs(out_dir, exist_ok=True)
    emb_path = os.path.join(out_dir, "embedding.keras")
    embedding.save(emb_path)
    print(f"[train] saved embedding model -> {emb_path}")
    print("[train] next: python quantize_export.py "
          f"--variant {args.variant} --repr data/repr")


if __name__ == "__main__":
    main()
