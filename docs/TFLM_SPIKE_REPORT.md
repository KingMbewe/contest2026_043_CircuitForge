# TFLite-Micro Latency/Integration Spike — Report

**Date:** 2026-06-27
**Goal:** De-risk on-device AI for VelaPaw — confirm TFLite-Micro integrates and runs on openVela, and measure the tensor-arena footprint, op coverage, and compute profile of a MobileNet-class INT8 CNN.
**Model:** `person_detect.tflite` (bundled in tflite-micro) — INT8 MobileNet-v1-0.25, 96×96×1 grayscale, ~294 KB. A close compute proxy for VelaPaw's "fast" embedding backbone (MobileNetV3-Small @ 96×96).
**Vehicle:** the in-tree `tflm_benchmark` tool (`CONFIG_TFLITEMICRO_BENCHMARK_TOOL`), run on the goldfish ARM emulator.

> **Critical caveat on latency:** the emulator is QEMU emulating **ARM cortex-a15** on an x86 host — NOT the ESP32-S3's **Xtensa LX7**. Absolute milliseconds here are **not** a hardware estimate. What *is* portable and trustworthy: (1) integration works, (2) arena footprint, (3) op coverage, (4) the relative compute breakdown across layer types.

---

## Results

### ✅ Integration — works
TFLite-Micro builds for openVela (ARM cross), links into the image, and runs a full INT8 inference to completion. Output `CRC32: 0x24B4C645` (deterministic, model executed correctly). `nuttx.bin` grew 1.88 MB → 2.70 MB with TFLM + deps linked in.

### ✅ Tensor arena — ~82 KB (the key portable number)
```
[[ Arena ]]      Bytes    % Arena
Total           84,420    100.0
NonPersistent   55,296     65.5
Persistent      29,124     34.5
```
- Configured arena 204,800 B; **actual used 84,420 B (~82 KB)**.
- On the ESP32-S3-EYE (8 MB PSRAM) this is **negligible** — arena sizing is a non-issue for VelaPaw. Even several models loaded at once fit easily.

### ✅ Operator coverage — all supported by the default resolver
`CONV_2D`, `DEPTHWISE_CONV_2D`, `AVERAGE_POOL_2D`, `RESHAPE`, `SOFTMAX` — all ran with the stock op resolver. (VelaPaw's MobileNetV3-Small adds SE blocks / hard-swish → will also need `MUL`, `ADD`, `LOGISTIC`/`HARD_SWISH`; verify those when the real model is exported.)

### Compute profile (one inference) — CONV dominates
```
Layer type          ticks      share
CONV_2D            643,819     ~84%
DEPTHWISE_CONV_2D  122,680     ~16%
AVERAGE_POOL_2D        439     ~0%
SOFTMAX              1,048     ~0%
RESHAPE                119     ~0%
TOTAL              768,105
AllocateTensors     24,860   (one-time)
```
**Takeaway:** ~84% of inference time is standard `CONV_2D`. Latency on real hardware is therefore almost entirely a function of how fast INT8 convolutions run.

### ⚠️ Latency (emulated only — do not quote as S3)
~768,105 profiler ticks ≈ **~0.77 s** of emulated cortex-a15 time for one inference. This reflects QEMU emulation, not silicon, and uses TFLM **reference** kernels (no LX7/NEON acceleration in this path). Treat only as an upper-bound sanity check that the pipeline runs.

---

## What this means for VelaPaw (and the plan's Risk #1)

The plan's biggest risk was "INT8 fine-grained ID may miss the <300 ms target on the S3 LX7." This spike sharpens that:

- **Arena/memory: solved** — ~82 KB, trivial on 8 MB PSRAM.
- **Op coverage: green** for a MobileNet core; recheck SE/hard-swish ops for MobileNetV3.
- **Latency: still the open risk, and it hinges on conv kernels.** Since CONV_2D is ~84% of the work, hitting <300 ms on the S3 depends almost entirely on **optimized INT8 convolution kernels (ESP-NN)**. In Phase 0 we found **no ESP-NN** in the tree (only `cmsis-nn` = ARM, `nnlib-hifi4` = HiFi DSP — neither targets the S3's LX7). With reference kernels, real-hardware latency will be far above target.

**Decision implied:** before committing to MobileNetV3+TSFM at 96×96, the next hardware step (Phase 5, or a quick ESP32-S3 bring-up) must establish whether ESP-NN can be added to this TFLM build. If not, the mitigations from the plan move up: smaller input (e.g. 64×64), shrink the backbone/embedding head, widen the presence-gate interval. The emulator cannot answer the absolute-latency question — only the arena, op, and profile questions, which it now has.

---

## Build integration notes (Windows)

Getting the in-tree benchmark tool to build on native Windows needed three fixes (TFLM library itself built cleanly; all issues were in the benchmark/codegen tooling). Documented for reproducibility:

1. **Benchmark CMakeLists was a standalone `project(tflm_benchmarking CXX)`** → re-ran compiler detection (defaulted to `nmake`, no host compiler) and aborted the subdir before the app registered. Neutralized `cmake_minimum_required`/`project()` so it inherits the parent cross-toolchain.
   File: `apps/mlearning/tflite-micro/tflite-micro/tensorflow/lite/micro/tools/benchmarking/CMakeLists.txt`
2. **Meta-data banner** (`collect_meta_data.sh`) ran under cmd.exe and then tried to `pip install tensorflow`. Bypassed entirely via `-DGENERIC_BENCHMARK_NO_META_DATA` and dropping `show_meta_data.cc` from sources (same file).
3. **`generate_cc_arrays.py` built the C identifier from the full Windows path** (`g_D:\openVela\...`) — used `abs_fname.split('/')` which doesn't split Windows backslashes. Fixed `get_array_name()` to use `os.path.basename` + `os.path.splitext`.
   File: `apps/mlearning/tflite-micro/tflite-micro/tensorflow/lite/micro/tools/generate_cc_arrays.py`

### Reproduce
```bash
source /d/openVela/vela-env.sh
cd /d/openVela/nuttx
# enable (deps auto-resolve via olddefconfig):
kconfig-tweak --file build/.config -e SYSTEM_FLATBUFFERS -e MATH_GEMMLOWP -e MATH_KISSFFT \
  -e MATH_RUY -e TFLITEMICRO -e TFLITEMICRO_BENCHMARK_TOOL
kconfig-tweak --file build/.config --set-str BENCHMARK_MODEL_PATH \
  "D:/openVela/apps/mlearning/tflite-micro/tflite-micro/tensorflow/lite/micro/models/person_detect.tflite"
kconfig-tweak --file build/.config --set-val BENCHMARK_TENSOR_ARENA_SIZE 204800
vela_olddefconfig
cmake -B build -DBOARD_CONFIG="$VELA_BOARD_CONFIG" -GNinja
cmake --build build -j8
# run (sends the command to NSH via stdin, headless):
cd /d/openVela && export HOST_OS=windows HOST_ARCH=x86_64
(sleep 22; printf 'tflm_benchmark\n'; sleep 120) | ./emulator.sh nuttx/build -no-window
```

---

## Status

Spike complete. Integration + memory + op-coverage + compute-profile de-risked. The one question the emulator structurally cannot answer — absolute LX7 latency — is now clearly scoped and tied to ESP-NN availability, to be resolved at hardware bring-up.
