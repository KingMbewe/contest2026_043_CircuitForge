"""Confusion matrix / per-class recall for bcs.tflite (bias check)."""
import os
import numpy as np
import tensorflow as tf
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "bcs_dataset")
CLASSES = ["under", "ideal", "over"]   # matches training class_names order

interp = tf.lite.Interpreter(os.path.join(HERE, "bcs.tflite"))
interp.allocate_tensors()
inp = interp.get_input_details()[0]
out = interp.get_output_details()[0]

conf = np.zeros((3, 3), int)   # conf[true][pred]
for ti, c in enumerate(CLASSES):
    d = os.path.join(DATA, c)
    for f in os.listdir(d):
        p = os.path.join(d, f)
        if not os.path.isfile(p) or not f.lower().endswith((".jpg", ".jpeg", ".png")):
            continue
        try:
            img = Image.open(p).convert("RGB").resize((128, 128))
        except Exception:
            continue
        x = np.expand_dims(np.asarray(img, dtype=inp["dtype"]), 0)
        interp.set_tensor(inp["index"], x)
        interp.invoke()
        conf[ti][int(np.argmax(interp.get_tensor(out["index"])[0]))] += 1

print("input dtype:", inp["dtype"])
print("\nconfusion (rows=TRUE, cols=PRED):    ", CLASSES)
for i, c in enumerate(CLASSES):
    print(f"  {c:6} {conf[i]}   recall={conf[i][i]/max(1,conf[i].sum()):.2f}  n={conf[i].sum()}")
print("\npredicted totals per class:", dict(zip(CLASSES, conf.sum(0).tolist())))
print("overall acc:", np.trace(conf) / max(1, conf.sum()))
