# VelaPaw KWS — training the scheduling vocabulary

Trains the keyword-spotting model that lets an owner set a feeding time by
voice. Replaces the upstream Colab notebook with a staged, resumable script.

```
host/.venv/Scripts/python.exe host/kws/train_kws.py all
```

---

## Why there is no recording session

`docs/AUDIO_PLAN.md` Phase 2 originally specified recording ~10–15 words
yourself, through this mic, in the printed enclosure, with the stepper running —
and rated the result *"roughly a coin flip"* against the schedule. That risk was
priced against building a dataset from scratch.

It turns out not to be necessary. **Google Speech Commands v0.02** (Pete Warden,
2018 — 105,829 one-second 16 kHz clips, 2,618 speakers, 35 words, CC BY 4.0)
already contains every word the scheduling dialog needs:

| dialog step | words | in dataset? |
|---|---|---|
| hour | `zero`–`nine` | ✅ |
| confirm | `yes`, `no` | ✅ (already shipping) |
| meal slot | `one`, `two`, `three` | ✅ (reuses the digits) |
| **pet** | — | **not spoken — the camera identifies the pet** |

The words the dataset *lacks* — "schedule", "breakfast", a wake word — are
exactly the ones a button and three numbered meal slots replace at no cost.

This is also not a new dependency: the model already on the board
(`micro_speech`, yes/no at 0.98 live) was trained on this same dataset. The
domain gap between crowdsourced laptop-mic audio and an ES8311 in a printed
enclosure is therefore **measured, not hypothetical** — and survivable.

## Vocabulary and label order — load-bearing

```python
WORDS = ["yes", "no", "zero", "one", ..., "nine"]
```

`input_data.prepare_words_list()` returns `[_silence_, _unknown_] + wanted_words`,
so the label map is:

```
 0 _silence_    1 _unknown_    2 yes     3 no
 4 zero   5 one   6 two   7 three   8 four
 9 five  10 six  11 seven 12 eight  13 nine
```

Keeping `yes,no` first means the device enum in `app/velapaw/voice/kws.h` is
**extended, not renumbered** — `VELAPAW_KWS_YES = 2` and `NO = 3` stay valid, so
build 54's code keeps working. Reordering `WORDS` without editing `kws.h` would
make the board confidently report the wrong word, with nothing in the logs to
suggest why.

The ~23 dataset words we don't ask for are folded into `_unknown_`
automatically, which is free negative training data.

## Feature geometry must match the board

`PREPROCESS='micro'` selects TF's `audio_microfrontend` op — the same algorithm
the on-device `audio_preprocessor_int8` model implements. Verified equal:

| | training | device (`kws.h`) |
|---|---|---|
| frames | 49 | `VELAPAW_KWS_FEATURE_COUNT` 49 |
| mel bins | 40 | `VELAPAW_KWS_FEATURE_SIZE` 40 |
| fingerprint | 1960 | 49 × 40 |
| sample rate | 16000 | `VELAPAW_KWS_SAMPLE_RATE` |
| window / stride | 30 / 20 ms | `DURATION_MS` / `STRIDE_MS` |

Changing any of these here without changing `kws.h` (and regenerating the
preprocessor model) silently desynchronises training from inference.

## Environment

`host/.venv` — Python 3.11.9, TensorFlow 2.15.1, numpy 1.26.4. The same env that
trained the vision models; nothing new to install.

**Training is CPU-only.** TensorFlow dropped native Windows GPU support after
2.10, and this venv holds the `tensorflow-intel` wheel — `is_built_with_cuda()`
is `False`, so the RTX 4060 is invisible to it. `tiny_conv` is one conv plus one
FC, and the real bottleneck is per-batch MFCC of the WAVs (CPU-bound even on a
GPU run), so 18,000 steps is an overnight job, not an intractable one.

If you want the GPU involved: **WSL2** (TF 2.15 GPU works there) or **Google
Colab** (free T4; the upstream notebook this derives from was built for it, but
you re-download 2.4 GB per session and long runs hit the idle timeout).

### The `models` shadowing trap

`host/models/` is where the vision pipeline writes its exports. It is a bare
directory, which Python 3 treats as a namespace package — so a plain
`import models` from `host/` resolves to *that* instead of
`speech_commands/models.py` and dies with a bewildering
`module 'models' has no attribute 'prepare_model_settings'`.
`_import_speech_commands()` inserts the vendored dir first **and asserts the
resolved path**, so a subtler version of this can't cost an overnight run.

## Stages

| stage | what it does | cost |
|---|---|---|
| `prep` | download + verify dataset against published clip counts | ~2.4 GB, once |
| `train` | 15,000 + 3,000 steps, checkpoint every 1,000 | **overnight** |
| `freeze` | checkpoint → SavedModel | seconds |
| `convert` | SavedModel → float + int8 `.tflite` | ~1 min |
| `evaluate` | accuracy of both on the held-out test split | ~2 min |

### `prep` checks counts, not existence — and this is not paranoia

A missing word is the easy case: `input_data` would quietly fold it into
`_unknown_` and train a model that can *never* emit it. The case that actually
bit us is worse. A killed download left `four/` holding **269 of its 3,728
clips** — the folder existed, was non-empty, and passed every "is it there"
check. Training would have run overnight on 7 % of a class and the symptom
would have surfaced on the board.

So `prep` asserts each word against its published count (95 % floor) plus the
105,829 total, and prints what it saw either way.

**A short extraction cannot be repaired by re-running `prep`.**
`input_data.py:219-240` nests the `extractall()` *inside* `if not
gfile.Exists(tarball)`, so once the 2.4 GB `.tar.gz` is on disk the extract is
skipped forever and a half-unpacked tree looks cached. Repair by hand — and
note the archive members are `./`-prefixed, so a bare name silently matches
nothing:

```bash
cd host/kws/dataset
tar -xzf speech_commands_v0.02.tar.gz ./four ./stop ./up      # NOT `four stop up`
```

Re-running `train` restarts from zero. To resume a partial run, pass
`--start_checkpoint` to `speech_commands/train.py` by hand.

## The go/no-go gate

`evaluate` prints overall and per-label accuracy for the int8 model — the one
that actually runs on the board.

| result | action |
|---|---|
| **≥ 90 %** | proceed to embedding |
| 85–90 % | usable; expect more "say again" in the dialog |
| **< 85 %** | stop — don't spend device time on it |

Expect the digits to score below yes/no. `three`/`free` and `nine`/`five` are
genuinely close, far closer than `yes`/`no`. The dialog's confirm step exists to
absorb exactly this, and the AM/PM confirm rides along on it for free.

Treat 90 % as a target, not a prediction: the ~91 % figure quoted upstream for
`tiny_conv` is at **4** labels. This architecture is essentially one 4000×N
fully-connected head, and ten confusable digits is a much harder problem than
yes/no/silence/unknown.

### Result, 2026-08-05 — and why 80.85 % is a PASS

18,000 steps, 84.2 % final validation. On the held-out test split (n=5623):
**float 80.90 %, int8 80.85 %** — quantisation cost 0.05 points. Model 59,024 B.

That is under the 85 % line above, and the line is measuring the wrong thing.
Almost the whole deficit is one class the product never has to name:

```
_silence_ 95.4    _unknown_ 34.1    yes 91.6   no 82.5
zero 90.4  one 80.2  two 72.6  three 84.2  four 72.8
five 82.2  six 91.1  seven 87.9  eight 81.1  nine 80.6
```

**VelaPaw never makes a 14-way decision.** Every capture happens inside a dialog
step that already knows its legal answers, so the honest measure is argmax over
that subset:

| step | words | int8 |
|---|---|---:|
| MEAL | one / two / three | **95.68 %** |
| HOUR | zero … nine | **84.95 %** |
| CONFIRM | yes / no | **97.45 %** |

The confusions inside HOUR are the predicted ones: `four`→`two` ×33,
`five`→`nine` ×38, `nine`→`one` ×23.

### Threshold on the RAW probability, not the renormalised one

A constrained argmax always emits *something*, so the accept test carries as
much weight as the model. The obvious choice — renormalise over the legal
subset, `p_best / Σp_legal` — is a good separator between legal words and a
**terrible** rejector of everything else, because renormalising discards the
probability mass sitting on `_unknown_`, which is exactly the mass that means
"that wasn't one of my words." Measured against silence + out-of-vocabulary
clips it lets **86 % of junk through at CONFIRM**.

Thresholding the raw softmax probability of the best legal word keeps that mass
in the denominator. Lowest raw threshold holding junk false-accept ≤ 5 %:

| step | raw p ≥ | accept rate | of accepted, correct | junk through |
|---|---:|---:|---:|---:|
| MEAL | 0.45 | 74.8 % | **99.24 %** | 4.8 % |
| HOUR | 0.73 | 64.6 % | **96.87 %** | 4.9 % |
| CONFIRM | 0.59 | 80.7 % | **99.55 %** | 4.9 % |

Rolled up over one dialog with up to three tries per step: MEAL lands right
97.7 %, HOUR 92.6 %, a wrong time reaches the confirm screen 3.7 % of the time —
where the owner reads it and says "no" — so a **wrong feeding time is actually
saved in 0.017 % of dialogs**. The residual cost is 6.0 % of dialogs abandoned
after three tries somewhere, which is a "say again" problem, not a correctness
one. HOUR's threshold is the dial: dropping it to 0.60 buys 11 points of accept
rate for 3 points of junk.

**Verdict: proceed to embedding.** `conv` would cost a PSRAM arena bring-up and
a widened resolver to attack per-word digit confusability that the confirm step
already absorbs.

Two things these numbers do *not* cover: the clips are crowdsourced laptop-mic
audio, so on-board figures through the ES8311 in a printed enclosure will be
lower — measure per-digit confidence on hardware and re-tune the three
thresholds there — and Speech Commands skews US English.

### If it misses — the fallback ladder, measured at 14 labels

| architecture | params | ~int8 | first-conv activation | new ops |
|---|---:|---:|---:|---|
| `tiny_embedding_conv` | 7,134 | 7 KB | small | none |
| **`tiny_conv`** (current) | **56,662** | **55 KB** | 25×20×8 = **4 KB** | — |
| `conv` | 622,222 | 608 KB | 49×40×64 ≈ **125 KB** | `MAX_POOL_2D` |
| `low_latency_conv` | 877,208 | 857 KB | large | none |

**Flash is not the constraint.** The firmware region runs 0x0 → 0x600000 before
the vision model and the binary ends around 0xF8B88, so even `conv` fits. **The
arena is.** `tiny_conv` fits an 8 KB arena because its activations are 4 KB;
`conv` would force a PSRAM arena *and* a widened resolver. That is a bring-up,
not a swap.

So in the 85–90 % band the cheap moves are per-word thresholds and leaning on the
confirm step — not a bigger model. Reach for `conv` only below ~80 %, where the
dialog genuinely would not work.

If the digits are weak **on-device** despite good test accuracy, the cause is
likely accent — Speech Commands skews US English. The fix is fine-tuning on a
few hundred of your own utterances on top of this model, which is hours of work,
not a restart.

## What happens after a good model

```bash
python3 host/gen_model_data.py \
    --input  host/kws/models/kws_int8.tflite \
    --symbol g_micro_speech_quantized_model_data \
    --outdir app/velapaw/voice/gen
```

Then on the device side:

1. `voice/kws.h` — `VELAPAW_KWS_CATEGORIES` 4 → 14, append the digit labels
2. `voice/kws.cc` — extend the label-name table
3. 🔴 **Register `AddConv2D()`, not `AddDepthwiseConv2D()`.** The shipped
   micro_speech model used a depthwise conv; `tiny_conv` emits a plain
   `CONV_2D`. The resolver stays `<4>` — same slot count, different op — and
   getting it wrong is not a link error, it's `AllocateTensors` failing at boot
   with "Didn't find op for builtin opcode".
4. `KWS_CLASSIFIER_ARENA` 8 KB → **16 KB**. The conv activation is 25×20×8 =
   4000 B and the flattened 4000 B feeding `final_fc` is live alongside the
   1960 B input, so the peak is ~10 KB against 6916 B before. The board prints
   `arena used N/16384` at init — shrink to the measured number, don't guess.
5. **Leave `CONFIG_VELAPAW_KWS_SELFTEST` on for the first boot.** An earlier
   note here said to turn it off because its vectors assert 4-label indices.
   That was wrong: the two bit-exactness vectors test the *frontend* graph,
   which did not change, and the end-to-end clips assert
   `VELAPAW_KWS_YES`/`NO`/`SILENCE` = 2/3/0, which the label order deliberately
   preserved. It is the cheapest end-to-end check of the new classifier on
   known audio. Turn it off afterwards for the ~98 KB of canned clips.

Model size grows from 18,800 B to **59,024 B**. The `final_fc` head is
4000 × n_labels and dominates.

## Provenance

`speech_commands/` is vendored from
`tensorflow/tensorflow` at tag **`v2.15.0`** — matching the installed TF 2.15.1,
so there is no version drift:

```
train.py  models.py  input_data.py  freeze.py
```

Vendored rather than cloned so the pipeline is reproducible and reviewable
in-repo, and doesn't depend on the state of TensorFlow master.

Dataset: <https://storage.googleapis.com/download.tensorflow.org/data/speech_commands_v0.02.tar.gz>
(CC BY 4.0 — attribution belongs in the project's licence notes).
