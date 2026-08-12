"""Train a 3-class cat Body Condition Score model and export INT8 TFLite.

Input: bcs_dataset/{under,ideal,over}/  (JPEGs)
Output: bcs.tflite (INT8) + bcs_labels.txt

Small on-device footprint: MobileNetV2 alpha=0.35 @ 128x128 (matches the camera
path), transfer-learned from ImageNet, class-balanced for the mild imbalance.

Run:
    .venv/Scripts/python.exe train_bcs.py
"""

import os
import shutil

import numpy as np
import tensorflow as tf
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "bcs_dataset")
IMG = 128
BATCH = 16
SEED = 42


def integrity_pass():
    """Move unreadable/corrupt images to a _corrupt/ subfolder (reversible)."""
    bad = 0
    for cls in ("under", "ideal", "over"):
        d = os.path.join(DATA, cls)
        if not os.path.isdir(d):
            continue
        cor = os.path.join(d, "_corrupt")
        for f in list(os.listdir(d)):
            p = os.path.join(d, f)
            if not os.path.isfile(p) or not f.lower().endswith(
                    (".jpg", ".jpeg", ".png")):
                continue
            try:
                with Image.open(p) as im:
                    im.verify()
            except Exception:
                os.makedirs(cor, exist_ok=True)
                shutil.move(p, os.path.join(cor, f))
                bad += 1
    print(f"integrity: moved {bad} corrupt image(s) to _corrupt/")


def main():
    integrity_pass()

    train_ds = tf.keras.utils.image_dataset_from_directory(
        DATA, labels="inferred", label_mode="categorical",
        image_size=(IMG, IMG), batch_size=BATCH, validation_split=0.2,
        subset="training", seed=SEED, class_names=["under", "ideal", "over"])
    val_ds = tf.keras.utils.image_dataset_from_directory(
        DATA, labels="inferred", label_mode="categorical",
        image_size=(IMG, IMG), batch_size=BATCH, validation_split=0.2,
        subset="validation", seed=SEED, class_names=["under", "ideal", "over"])
    class_names = ["under", "ideal", "over"]

    # class weights for the mild imbalance
    counts = [len([f for f in os.listdir(os.path.join(DATA, c))
                   if f.lower().endswith((".jpg", ".jpeg", ".png"))])
              for c in class_names]
    total = sum(counts)
    class_weight = {i: total / (3.0 * counts[i]) for i in range(3)}
    print("counts", dict(zip(class_names, counts)), "weights", class_weight)

    AUTOTUNE = tf.data.AUTOTUNE
    train_ds = train_ds.prefetch(AUTOTUNE)
    val_ds = val_ds.prefetch(AUTOTUNE)

    aug = tf.keras.Sequential([
        tf.keras.layers.RandomFlip("horizontal"),
        tf.keras.layers.RandomRotation(0.08),
        tf.keras.layers.RandomZoom(0.15),
        tf.keras.layers.RandomContrast(0.15),
    ])
    prep = tf.keras.applications.mobilenet_v2.preprocess_input

    base = tf.keras.applications.MobileNetV2(
        input_shape=(IMG, IMG, 3), alpha=0.35, include_top=False,
        weights="imagenet")
    base.trainable = False

    inp = tf.keras.Input((IMG, IMG, 3))
    x = aug(inp)
    x = prep(x)
    x = base(x, training=False)
    x = tf.keras.layers.GlobalAveragePooling2D()(x)
    x = tf.keras.layers.Dropout(0.3)(x)
    out = tf.keras.layers.Dense(3, activation="softmax")(x)
    model = tf.keras.Model(inp, out)

    # phase 1: train the head
    model.compile(optimizer=tf.keras.optimizers.Adam(1e-3),
                  loss="categorical_crossentropy", metrics=["accuracy"])
    model.fit(train_ds, validation_data=val_ds, epochs=10,
              class_weight=class_weight, verbose=2)

    # phase 2: fine-tune the top of the backbone
    base.trainable = True
    for layer in base.layers[:-30]:
        layer.trainable = False
    model.compile(optimizer=tf.keras.optimizers.Adam(1e-4),
                  loss="categorical_crossentropy", metrics=["accuracy"])
    model.fit(train_ds, validation_data=val_ds, epochs=8,
              class_weight=class_weight, verbose=2)

    loss, acc = model.evaluate(val_ds, verbose=0)
    print(f"\n=== VAL ACCURACY: {acc:.3f} ===")

    # INT8 export with a representative dataset
    def rep():
        for imgs, _ in train_ds.take(30):
            for i in range(imgs.shape[0]):
                yield [tf.expand_dims(imgs[i], 0)]

    conv = tf.lite.TFLiteConverter.from_keras_model(model)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = rep
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.uint8
    conv.inference_output_type = tf.uint8
    tfl = conv.convert()

    out_path = os.path.join(HERE, "bcs.tflite")
    with open(out_path, "wb") as f:
        f.write(tfl)
    with open(os.path.join(HERE, "bcs_labels.txt"), "w") as f:
        f.write("\n".join(class_names) + "\n")
    print(f"exported {out_path} ({len(tfl)} bytes), labels: {class_names}")


if __name__ == "__main__":
    main()
