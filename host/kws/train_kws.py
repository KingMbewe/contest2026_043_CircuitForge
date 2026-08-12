#!/usr/bin/env python3
"""Train the VelaPaw keyword-spotting model on Google Speech Commands.

This replaces the upstream Colab notebook
(`micro_speech/train/train_micro_speech_model.ipynb`) with a plain, staged,
resumable script.  Same pipeline, same hyper-parameters, no `!` magics and no
`git clone` of TensorFlow master -- `speech_commands/` is vendored next to this
file, pinned to tag v2.15.0 to match the TF 2.15.1 in `host/.venv`.

WHY THE PUBLIC DATASET
    Every word VelaPaw's scheduling dialog needs is already in Speech Commands
    v0.02: the digits zero-nine carry the hour, yes/no carry the confirm, and
    the pet is identified by the camera rather than spoken.  The words that are
    NOT in the dataset ("schedule", "breakfast", a wake word) are the ones a
    button and three numbered meal slots replace at no cost.  So no custom
    recording session is required to ship the feature.

LABEL ORDER IS LOAD-BEARING
    `input_data.prepare_words_list()` returns

        [_silence_, _unknown_] + wanted_words

    so silence=0 and unknown=1 always, and the wanted words follow IN THE ORDER
    GIVEN.  Keeping "yes,no" first therefore preserves the device enum

        VELAPAW_KWS_SILENCE=0  UNKNOWN=1  YES=2  NO=3

    unchanged and merely appends the digits at 4..13.  Do not reorder WORDS
    without editing `app/velapaw/voice/kws.h` to match -- a silent reordering
    would make the board confidently report the wrong word.

    Every one of the ~23 dataset words we do NOT ask for is folded into
    `_unknown_` automatically (input_data.py:325), which is free negative data.

FEATURE PIPELINE MUST MATCH THE BOARD
    PREPROCESS='micro' selects TF's `audio_microfrontend` op, which is the same
    algorithm the on-device `audio_preprocessor_int8` model implements.  The
    resulting feature map is 49 frames x 40 mel bins = 1960 int8, matching
    VELAPAW_KWS_FEATURE_COUNT x VELAPAW_KWS_FEATURE_SIZE in kws.h.  Changing
    WINDOW_SIZE_MS / WINDOW_STRIDE / FEATURE_BIN_COUNT here without changing
    kws.h (and regenerating the preprocessor model) silently desynchronises
    training from inference.

USAGE
    host/.venv/Scripts/python.exe host/kws/train_kws.py all

    Stages run independently and are resumable:
        prep      download + verify the dataset (~2.4 GB on first run)
        train     the long one; writes checkpoints every 1000 steps
        freeze    checkpoint -> SavedModel
        convert   SavedModel -> float .tflite and int8 .tflite
        evaluate  accuracy of both, on the held-out test split
        all       everything above, in order

    Training is CPU-only on Windows: TF dropped native Windows GPU support
    after 2.10 and `host/.venv` has the `tensorflow-intel` wheel
    (is_built_with_cuda() == False).  tiny_conv is small and the real bottleneck
    is per-batch MFCC of the WAVs, which is CPU-bound even on a GPU run, so this
    is an overnight job rather than an intractable one.  See README.md for the
    WSL2 and Colab routes if you want the RTX 4060 involved.
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SC_DIR = os.path.join(HERE, "speech_commands")

# ---------------------------------------------------------------------------
# Vocabulary
# ---------------------------------------------------------------------------
# yes/no first so the existing device enum values survive (see module docstring).
# The digits carry the hour in the scheduling dialog.
#
# The upstream notebook's comment claims the options are limited to the ten
# classic benchmark words.  That is convention, not a constraint: input_data.py
# builds the label map from whatever folders it finds under the dataset root
# (input_data.py:270-295), and Speech Commands v0.02 ships all ten digits.  The
# `prep` stage asserts this rather than trusting it.
WORDS = [
    "yes", "no",
    "zero", "one", "two", "three", "four",
    "five", "six", "seven", "eight", "nine",
]
WANTED_WORDS = ",".join(WORDS)

# Published per-word clip counts for Speech Commands v0.02, used by `prep` to
# detect a TRUNCATED extraction -- the failure mode that actually happened here:
# a killed download left `four/` holding 269 of its 3728 clips, which every
# "does the folder exist / is it non-empty" check in the world passes.  The
# model would have trained overnight on 7% of a class and the symptom would
# have surfaced on the board, not in the logs.
EXPECTED_CLIPS = {
    "yes": 4044, "no": 3941,
    "zero": 4052, "one": 3890, "two": 3880, "three": 3727, "four": 3728,
    "five": 4052, "six": 3860, "seven": 3998, "eight": 3787, "nine": 3934,
}
EXPECTED_TOTAL = 105829       # all 35 words, excluding _background_noise_

# ---------------------------------------------------------------------------
# Hyper-parameters -- upstream defaults except where noted
# ---------------------------------------------------------------------------
TRAINING_STEPS = "15000,3000"
LEARNING_RATE = "0.001,0.0001"
TOTAL_STEPS = str(sum(int(s) for s in TRAINING_STEPS.split(",")))

PREPROCESS = "micro"
MODEL_ARCHITECTURE = "tiny_conv"
WINDOW_STRIDE = 20            # ms -- VELAPAW_KWS_STRIDE_MS
WINDOW_SIZE_MS = 30.0         # ms -- VELAPAW_KWS_DURATION_MS
FEATURE_BIN_COUNT = 40        # VELAPAW_KWS_FEATURE_SIZE
SAMPLE_RATE = 16000           # VELAPAW_KWS_SAMPLE_RATE
CLIP_DURATION_MS = 1000

BACKGROUND_FREQUENCY = 0.8    # fraction of clips that get noise mixed in
BACKGROUND_VOLUME_RANGE = 0.1
TIME_SHIFT_MS = 100.0
VALIDATION_PERCENTAGE = 10
TESTING_PERCENTAGE = 10

# Balance silence/unknown against the real words so no class dominates.
N_LABELS = len(WORDS) + 2
EQUAL_PCT = int(100.0 / N_LABELS)
SILENT_PERCENTAGE = EQUAL_PCT
UNKNOWN_PERCENTAGE = EQUAL_PCT

DATA_URL = ("https://storage.googleapis.com/download.tensorflow.org/data/"
            "speech_commands_v0.02.tar.gz")

# ---------------------------------------------------------------------------
# Paths (all under host/kws/, none tracked except the final model)
# ---------------------------------------------------------------------------
DATASET_DIR = os.path.join(HERE, "dataset")
LOGS_DIR = os.path.join(HERE, "logs")
TRAIN_DIR = os.path.join(HERE, "checkpoints")
MODELS_DIR = os.path.join(HERE, "models")
SAVED_MODEL = os.path.join(MODELS_DIR, "saved_model")
FLOAT_TFLITE = os.path.join(MODELS_DIR, "kws_float.tflite")
QUANT_TFLITE = os.path.join(MODELS_DIR, "kws_int8.tflite")


def _import_speech_commands():
    """Import the vendored input_data/models, and PROVE they are the right ones.

    `host/models/` (where the vision pipeline writes its exports) is a bare
    directory, which Python 3 happily treats as a namespace package.  So a
    plain `import models` from host/ resolves to that directory instead of
    speech_commands/models.py, and fails with a bewildering
    "module 'models' has no attribute 'prepare_model_settings'".  Inserting
    SC_DIR at position 0 beats cwd, but the assert is what stops a subtler
    version of this from costing an overnight run.
    """
    if SC_DIR not in sys.path:
        sys.path.insert(0, SC_DIR)
    import input_data
    import models
    for m in (input_data, models):
        if os.path.dirname(os.path.abspath(m.__file__)) != SC_DIR:
            sys.exit("FATAL: %r resolved to %s, not the vendored copy in %s.\n"
                     "Something on sys.path is shadowing it."
                     % (m.__name__, m.__file__, SC_DIR))
    return input_data, models


def _run(argv):
    """Run a child python with speech_commands/ importable, streaming output."""
    env = dict(os.environ)
    env["PYTHONPATH"] = SC_DIR + os.pathsep + env.get("PYTHONPATH", "")
    env.setdefault("PYTHONIOENCODING", "utf-8")
    print("\n$ " + " ".join(argv) + "\n", flush=True)
    rc = subprocess.call(argv, env=env)
    if rc != 0:
        sys.exit("FAILED (exit %d): %s" % (rc, argv[0]))


def _banner(title):
    print("\n" + "=" * 72 + "\n== " + title + "\n" + "=" * 72, flush=True)


# ---------------------------------------------------------------------------
# Stages
# ---------------------------------------------------------------------------
def stage_prep():
    """Download the dataset and prove every word we asked for is really there.

    Failing here costs seconds.  Discovering a missing label 6 hours into
    training costs the night, because input_data would have quietly folded the
    missing word into _unknown_ and trained a model that can never emit it.
    """
    _banner("PREP -- dataset download + vocabulary check")
    input_data, models = _import_speech_commands()

    os.makedirs(DATASET_DIR, exist_ok=True)
    settings = models.prepare_model_settings(
        len(input_data.prepare_words_list(WORDS)),
        SAMPLE_RATE, CLIP_DURATION_MS, WINDOW_SIZE_MS,
        WINDOW_STRIDE, FEATURE_BIN_COUNT, PREPROCESS)

    print("downloading / verifying Speech Commands v0.02 "
          "(~2.4 GB on first run, cached afterwards)...", flush=True)
    input_data.AudioProcessor(
        DATA_URL, DATASET_DIR, SILENT_PERCENTAGE, UNKNOWN_PERCENTAGE,
        WORDS, VALIDATION_PERCENTAGE, TESTING_PERCENTAGE, settings, LOGS_DIR)

    root = os.path.join(DATASET_DIR, "speech_commands_v0.02")
    if not os.path.isdir(root):
        root = DATASET_DIR

    bad = []
    for w in WORDS:
        d = os.path.join(root, w)
        n = len(os.listdir(d)) if os.path.isdir(d) else 0
        want = EXPECTED_CLIPS[w]
        ok = n >= want * 0.95
        print("  %-8s %6d clips  (expect %d) %s"
              % (w, n, want, "" if ok else "  <-- SHORT"))
        if not ok:
            bad.append((w, n, want))

    total = sum(len(os.listdir(os.path.join(root, d)))
                for d in os.listdir(root)
                if os.path.isdir(os.path.join(root, d))
                and d != "_background_noise_")
    print("\n  total clips across all 35 words: %d (expect %d)"
          % (total, EXPECTED_TOTAL))

    if bad or total < EXPECTED_TOTAL * 0.95:
        sys.exit(
            "\nFATAL: the dataset is incomplete -- %s\n\n"
            "RE-RUNNING THIS STAGE WILL NOT FIX IT.  input_data.py nests the\n"
            "extractall() inside `if not gfile.Exists(tarball)` (line 219-240),\n"
            "so once the .tar.gz is on disk the extract is skipped forever and\n"
            "a half-unpacked tree looks 'cached'.  Repair it by hand -- note the\n"
            "archive members are prefixed './', so a bare name matches nothing:\n\n"
            "    cd %s\n"
            "    tar -xzf speech_commands_v0.02.tar.gz ./<word> [./<word> ...]\n\n"
            "then re-run this stage to verify."
            % (", ".join("%s has %d of %d" % b for b in bad) or
               "total clip count is short", root))

    print("\nfeature map: %d frames x %d bins = %d values"
          % (settings["spectrogram_length"], settings["fingerprint_width"],
             settings["fingerprint_size"]))
    print("labels (%d): %s" % (N_LABELS,
                               ", ".join(input_data.prepare_words_list(WORDS))))
    print("\nOK -- vocabulary verified.")


def stage_train():
    _banner("TRAIN -- %s steps, this is the long one" % TOTAL_STEPS)
    print("Checkpoints land in %s every 1000 steps; re-running this stage "
          "restarts from scratch, so use --start_checkpoint by hand if you "
          "need to resume a partial run." % TRAIN_DIR)
    _run([sys.executable, os.path.join(SC_DIR, "train.py"),
          "--data_dir=" + DATASET_DIR,
          "--data_url=" + DATA_URL,
          "--wanted_words=" + WANTED_WORDS,
          "--silence_percentage=%d" % SILENT_PERCENTAGE,
          "--unknown_percentage=%d" % UNKNOWN_PERCENTAGE,
          "--preprocess=" + PREPROCESS,
          "--window_stride_ms=%d" % WINDOW_STRIDE,
          "--window_size_ms=%d" % WINDOW_SIZE_MS,
          "--feature_bin_count=%d" % FEATURE_BIN_COUNT,
          "--model_architecture=" + MODEL_ARCHITECTURE,
          "--how_many_training_steps=" + TRAINING_STEPS,
          "--learning_rate=" + LEARNING_RATE,
          "--train_dir=" + TRAIN_DIR,
          "--summaries_dir=" + LOGS_DIR,
          # DEBUG, not WARN and not INFO.  The ~15 TF1 deprecation notices come
          # out at WARN, so WARN is all noise and no signal.  INFO is not enough
          # either: train.py:249 logs the per-step accuracy through
          # tf.logging.DEBUG (only the every-1000-step validation line is INFO),
          # so at INFO an 18k-step overnight run shows nothing for ~15 minutes
          # at a stretch and looks indistinguishable from a hang.  Ask me how I
          # know.
          "--verbosity=DEBUG",
          "--eval_step_interval=1000",
          "--save_step_interval=1000"])


def stage_freeze():
    _banner("FREEZE -- checkpoint -> SavedModel")
    ckpt = os.path.join(TRAIN_DIR, "%s.ckpt-%s" % (MODEL_ARCHITECTURE, TOTAL_STEPS))
    if not os.path.exists(ckpt + ".index"):
        sys.exit("FATAL: no checkpoint at %s\nDid the train stage finish?" % ckpt)
    import shutil
    shutil.rmtree(SAVED_MODEL, ignore_errors=True)
    os.makedirs(MODELS_DIR, exist_ok=True)
    _run([sys.executable, os.path.join(SC_DIR, "freeze.py"),
          "--wanted_words=" + WANTED_WORDS,
          "--window_stride_ms=%d" % WINDOW_STRIDE,
          "--window_size_ms=%d" % WINDOW_SIZE_MS,
          "--feature_bin_count=%d" % FEATURE_BIN_COUNT,
          "--preprocess=" + PREPROCESS,
          "--model_architecture=" + MODEL_ARCHITECTURE,
          "--start_checkpoint=" + ckpt,
          "--save_format=saved_model",
          "--output_file=" + SAVED_MODEL])


def _audio_processor():
    input_data, models = _import_speech_commands()
    settings = models.prepare_model_settings(
        len(input_data.prepare_words_list(WORDS)),
        SAMPLE_RATE, CLIP_DURATION_MS, WINDOW_SIZE_MS,
        WINDOW_STRIDE, FEATURE_BIN_COUNT, PREPROCESS)
    ap = input_data.AudioProcessor(
        DATA_URL, DATASET_DIR, SILENT_PERCENTAGE, UNKNOWN_PERCENTAGE,
        WORDS, VALIDATION_PERCENTAGE, TESTING_PERCENTAGE, settings, LOGS_DIR)
    return ap, settings


def stage_convert():
    _banner("CONVERT -- SavedModel -> float + int8 .tflite")
    import numpy as np
    import tensorflow as tf
    ap, settings = _audio_processor()
    fp_size = settings["fingerprint_size"]      # 1960, derived not hardcoded

    with tf.compat.v1.Session() as sess:
        conv = tf.lite.TFLiteConverter.from_saved_model(SAVED_MODEL)
        n = open(FLOAT_TFLITE, "wb").write(conv.convert())
        print("float model: %d bytes" % n)

        conv = tf.lite.TFLiteConverter.from_saved_model(SAVED_MODEL)
        conv.optimizations = [tf.lite.Optimize.DEFAULT]
        conv.inference_input_type = tf.int8
        conv.inference_output_type = tf.int8

        def rep_dataset():
            for i in range(100):
                data, _ = ap.get_data(1, i, settings, BACKGROUND_FREQUENCY,
                                      BACKGROUND_VOLUME_RANGE, TIME_SHIFT_MS,
                                      "testing", sess)
                yield [np.array(data.flatten(),
                                dtype=np.float32).reshape(1, fp_size)]

        conv.representative_dataset = rep_dataset
        n = open(QUANT_TFLITE, "wb").write(conv.convert())
        print("int8 model:  %d bytes  -> %s" % (n, QUANT_TFLITE))
        print("\n(for reference the shipping 4-label model is 18,800 B; the "
              "growth is the final_fc head, which is 4000 x n_labels)")


def stage_evaluate():
    """Accuracy on the held-out test split -- the go/no-go gate.

    The int8 number is the one that matters; it is the model that runs on the
    board.  Expect the digits to sit below yes/no: 'three'/'free' and
    'nine'/'five' are genuinely close, and the dialog's confirm step exists to
    absorb exactly that.
    """
    _banner("EVALUATE -- accuracy on the test split")
    import numpy as np
    import tensorflow as tf
    ap, settings = _audio_processor()
    input_data, _ = _import_speech_commands()
    labels = input_data.prepare_words_list(WORDS)

    np.random.seed(0)   # reproducible test set
    with tf.compat.v1.Session() as sess:
        test_data, test_labels = ap.get_data(
            -1, 0, settings, BACKGROUND_FREQUENCY, BACKGROUND_VOLUME_RANGE,
            TIME_SHIFT_MS, "testing", sess)
    test_data = np.expand_dims(test_data, axis=1).astype(np.float32)

    for path, kind in ((FLOAT_TFLITE, "Float"), (QUANT_TFLITE, "Quantized")):
        if not os.path.exists(path):
            continue
        interp = tf.lite.Interpreter(
            path,
            experimental_op_resolver_type=(
                tf.lite.experimental.OpResolverType.BUILTIN_REF))
        interp.allocate_tensors()
        inp = interp.get_input_details()[0]
        out = interp.get_output_details()[0]

        data = test_data
        if kind == "Quantized":
            scale, zp = inp["quantization"]
            data = (test_data / scale + zp).astype(inp["dtype"])

        correct = 0
        per_label_ok = [0] * len(labels)
        per_label_n = [0] * len(labels)
        for i in range(len(data)):
            interp.set_tensor(inp["index"], data[i])
            interp.invoke()
            pred = interp.get_tensor(out["index"])[0].argmax()
            truth = int(test_labels[i])
            per_label_n[truth] += 1
            if pred == truth:
                correct += 1
                per_label_ok[truth] += 1

        acc = 100.0 * correct / len(data)
        print("\n%s model accuracy: %.2f%%  (n=%d)" % (kind, acc, len(data)))
        if kind == "Quantized":
            print("  per-label:")
            for j, name in enumerate(labels):
                if per_label_n[j]:
                    print("    %-10s %5.1f%%  (n=%d)"
                          % (name, 100.0 * per_label_ok[j] / per_label_n[j],
                             per_label_n[j]))
            print("\n  GATE: >=90%% overall -> proceed to embedding.")
            print("        85-90%%          -> usable, expect more 'say again'.")
            print("        <85%%            -> stop, do not spend device time.")


STAGES = {
    "prep": stage_prep,
    "train": stage_train,
    "freeze": stage_freeze,
    "convert": stage_convert,
    "evaluate": stage_evaluate,
}
ORDER = ["prep", "train", "freeze", "convert", "evaluate"]


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("stage", nargs="+", choices=ORDER + ["all"])
    args = p.parse_args()
    stages = ORDER if "all" in args.stage else args.stage
    for s in stages:
        STAGES[s]()
    print("\ndone: %s" % ", ".join(stages))


if __name__ == "__main__":
    main()
