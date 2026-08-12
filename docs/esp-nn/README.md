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

## Why

The identity model is dominated by depthwise convolution, which is slow on the
TFLite-Micro *reference* kernels. **ESP-NN** provides SIMD kernels for the Xtensa
LX7, giving roughly a **5× speed-up** (~6.75 s → ~1.3 s per inference) with no
change to the model or its accuracy.

## What's here

| File | Purpose |
|---|---|
| `kernels/*.cc` | The seven ESP-NN kernel wrappers (`add`, `conv`, `depthwise_conv`, `fully_connected`, `mul`, `pooling`, `softmax`), from Espressif's `esp-tflite-micro` (Apache-2.0). These replace the reference kernels for those ops. |
| `tflite-micro-Makefile.esp-nn.block` | The Makefile block that wires esp-nn into the tflite-micro build and filters out the duplicate reference kernels. |

## How to reproduce (in a fork of `open-vela/apps_mlearning_tflite-micro`)

1. **Add the esp-nn library.** Fetch Espressif's ESP-NN and place it at
   `apps/mlearning/esp-nn/`:

   ```bash
   git clone https://github.com/espressif/esp-nn \
     apps/mlearning/esp-nn
   ```

2. **Add the kernel wrappers.** Copy the seven `.cc` files from `kernels/` here
   into the tflite-micro kernel directory:

   ```bash
   cp docs/esp-nn/kernels/*.cc \
     apps/mlearning/tflite-micro/tflite-micro/tensorflow/lite/micro/kernels/esp_nn/
   ```

3. **Apply the Makefile block.** Append the contents of
   `tflite-micro-Makefile.esp-nn.block` to
   `apps/mlearning/tflite-micro/Makefile` (after the CMSIS block).

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
