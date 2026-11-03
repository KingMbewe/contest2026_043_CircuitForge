# ESP-NN acceleration — reference integration

> **This folder is reference material, not part of this repository's build.**
> The ESP-NN integration modifies a **public** openVela repo
> (`apps/mlearning/tflite-micro`), so per the contest rules it belongs in a
> fork-and-PR to `dev-ai-contest-2026`, **not** committed into the team repo.
> This folder documents *exactly* what that integration is, so the ~1.3 s
> inference figure is reproducible and reviewable. See
> [`docs/ENGINEERING.md`](../ENGINEERING.md) §6 for context.
>
> **Out of the box, the team repo builds and runs with the reference kernels
> (~6.75 s / inference). Applying this integration brings it to ~1.3 s.**
> The finals firmware ([`nuttx.bin`](../../nuttx.bin)) was built with it.

## Why

The identity model is dominated by depthwise convolution, which is slow on the
TFLite-Micro *reference* kernels. **ESP-NN** provides SIMD kernels for the Xtensa
LX7, giving roughly a **5× speed-up** (~6.75 s → ~1.3 s per inference) with no
change to the model or its accuracy.

## What's here

| File | Purpose |
|---|---|
| `kernels/*.cc` | The seven ESP-NN kernel wrappers (`add`, `conv`, `depthwise_conv`, `fully_connected`, `mul`, `pooling`, `softmax`), from Espressif's `esp-tflite-micro` (Apache-2.0). These replace the reference kernels for those ops. |
| `tflite-micro-Makefile.esp-nn.block` | The Makefile block that wires esp-nn into the tflite-micro build and filters out the duplicate reference kernels. The same block is part of [`board/patches/apps-tflm-finals.patch`](../../board/patches/apps-tflm-finals.patch). |
| `shim/esp_timer.h` | A stand-in for ESP-IDF's `esp_timer.h`. All seven wrappers include it and call `esp_timer_get_time()`, which NuttX doesn't have; this one returns `CLOCK_MONOTONIC` in µs. The Makefile block already puts `$(ESP_NN_DIR)/shim` on the include path. |

## How to reproduce (in a fork of `open-vela/apps_mlearning_tflite-micro`)

Run these from the openVela workspace root, with `VP` pointing at this
repository as in [README §7](../../README.md#7-build--run-real-hardware).

1. **Add the esp-nn library.** Fetch Espressif's ESP-NN at the commit the
   finals firmware was built with, place it at `apps/mlearning/esp-nn/`, and
   add the timer shim:

   ```bash
   git clone https://github.com/espressif/esp-nn \
     apps/mlearning/esp-nn
   git -C apps/mlearning/esp-nn checkout d45b843ca5f873ca9d0706ab0e7f14eafd132e98
   mkdir -p apps/mlearning/esp-nn/shim
   cp $VP/docs/esp-nn/shim/esp_timer.h apps/mlearning/esp-nn/shim/
   ```

2. **Add the kernel wrappers.** Copy the seven `.cc` files from `kernels/` here
   into the tflite-micro kernel directory. The `esp_nn/` directory doesn't
   exist upstream, so create it first:

   ```bash
   mkdir -p apps/mlearning/tflite-micro/tflite-micro/tensorflow/lite/micro/kernels/esp_nn
   cp $VP/docs/esp-nn/kernels/*.cc \
     apps/mlearning/tflite-micro/tflite-micro/tensorflow/lite/micro/kernels/esp_nn/
   ```

3. **Apply the Makefile block.** Skip this step if you applied
   [`board/patches/apps-tflm-finals.patch`](../../board/patches/apps-tflm-finals.patch):
   it already contains the block, and that is how the finals firmware was
   built. Otherwise append the contents of `tflite-micro-Makefile.esp-nn.block`
   to `apps/mlearning/tflite-micro/Makefile` (after the CMSIS block).

4. **Clean rebuild.** ESP-NN must be built clean — a stale incremental build
   produces a false "crash on the first conv":

   ```bash
   cd nuttx && make clean && rm -rf staging/*.a
   cd .. && ./build.sh esp32s3-devkit:waveshare_lcd -j8
   ```

## Notes

- The esp-nn **assembly** sources (`*esp32s3.c`) are deliberately excluded by the
  Makefile block; the portable **opt-C** path is used, which builds cleanly under
  NuttX and already delivers the speed-up.
- Without the shim, every wrapper fails to compile at `#include <esp_timer.h>`.
- Verify a wrapper actually linked with the ESP-NN symbol, e.g.
  `xtensa-esp32s3-elf-nm nuttx | grep esp_nn_conv_s8`.

## Upstream status

This is submitted upstream as a fork/PR to the public tflite-micro repo. The
esp-nn library itself is a third-party component; whether it is adopted into the
openVela manifest is a maintainer decision.

**PR:** not yet opened. Per the contest rules, upstreaming to the public repo is a
post-award requirement for winners, not a preliminary/final judging criterion —
this document exists so the integration is reproducible and reviewable in the
meantime.

---

*Kernel wrappers © The TensorFlow Authors / Espressif, Apache License 2.0.*
