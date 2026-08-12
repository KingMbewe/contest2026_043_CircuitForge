<b>English</b> | <a href="ENGINEERING_zh.md">中文</a>

# VelaPaw — Engineering Deep-Dive

Technical notes on how VelaPaw is built: the on-device AI pipeline, the depth of
openVela use, the hardest problems solved, and the performance engineering. This
is the companion to the [README](../README.md) for readers who want the details.

---

## 1. System at a glance

Everything runs on a single **ESP32-S3** (Xtensa LX7, 8 MB PSRAM, 16 MB flash)
under **openVela / NuttX**, with an **LVGL** touchscreen UI. There is no
companion phone, no cloud, and no network dependency — capture, inference,
scheduling, timekeeping and dispensing are all local.

**Flash layout (16 MB)**

| Offset | Contents | Size | Note |
|---|---|---|---|
| `0x000000` | Firmware (`nuttx.bin`) | ~750 KB | openVela + app + LVGL |
| `0x600000` | Identity model (`velapaw.tflite`) | 1.44 MB | flash-loaded to PSRAM at boot |
| `0x760000` | Body-condition model (`bcs.tflite`) | 626 KB | flash-loaded to PSRAM at boot |
| `0x800000` | Enrolled pets (magic `VPAW`) | 8 KB | names, schedules, face embeddings |
| `0x810000` | Pet photos (magic `VICN`) | 32 KB | card thumbnails |

The two neural networks are **too large to memory-map** — the DROM0 MMU window
saturates well before a 1.4 MB `const` array maps — so they are flashed as raw
blobs and read into PSRAM buffers at boot via the NuttX software `spi_flash_read`
path. Because flash reads suspend the CPU cache, that read runs on a short-lived
thread whose stack is a `.bss` (internal-SRAM) buffer.

---

## 2. The recognition pipeline

```
camera frame ─► embed (TFLM INT8, background worker) ─► cosine match vs enrolled
                                                               │
                                                     pet id + score + margin
                                                               │
                                            schedule gate: is a meal of THIS pet
                                            due today and not yet eaten?
                                                     │yes           │no
                                               dispense N g      show next meal
                                                     │
                                          log history · trends · body-condition
```

**Open-set, not closed-set.** Recognition is metric learning, not an N-way
classifier: the model maps a 128×128 frame to an L2-normalized 128-D embedding,
enrollment stores the averaged embedding of a few photos, and matching is cosine
similarity against the enrolled set. The practical payoff is that **adding a pet
requires no retraining** — enrollment is a runtime operation, and the accepted/
rejected decision is a tunable threshold. (A consequence worth knowing: rejecting
arbitrary non-pet objects is inherently hard for a generic feature extractor, so
enrollment quality — tight, well-lit face shots — is the dominant lever on
false-accepts.)

**Inference is off the UI thread.** The ~1.3 s `Invoke()` runs on a dedicated
worker thread; the LVGL loop hands it a frame and keeps rendering, so touch and
the camera preview stay live during inference. Recognition is also **on-demand**
(it runs when a pet is at the bowl, not continuously) to bound the workload.

---

## 3. Depth of openVela use

VelaPaw is not a thin app on top of a BSP — it exercises openVela across the
stack:

- **`apps/mlearning/tflite-micro`** is the AI runtime. The app links it directly
  (`backend_tflm.cc`) and runs two independent `MicroInterpreter`s (identity +
  body-condition) with PSRAM tensor arenas.
- **LVGL** (openVela graphics) drives the four-tab touch UI.
- **Custom board bring-up** (`board/esp32s3_st7789.c`) drives, from scratch on
  the ESP32-S3 peripherals:
  - the **ST7796 display** over hardware SPI2 + GDMA;
  - the **OV5640 camera** via the **LCD_CAM** peripheral with a GDMA receive
    channel (configured over SCCB/I²C) — run out on a **30 cm FFC + coupler** so
    the camera mounts independently of the board (see [`hardware/README.md`](../hardware/README.md));
  - the **PCF85063 RTC** over I²C (raw register access — no `CONFIG_RTC`, which
    keeps the USB-CDC console alive);
  - the **28BYJ-48 stepper feeder** through a **ULN2003** driver on four GPIOs
    (IN1–IN3/IN4 on GPIO9/10/11/**43** — 43 is U0TXD, free because the console
    is on the USB-Serial/JTAG port; GPIO12/13/15 are the ES8311 codec's private
    I²S bus, used instead for the voice-settable schedules), stepped in software
    through the coil sequence.
- **Auto-start** (`board/esp32s3_appinit.c`) launches the app from
  `board_app_initialize()` — NSH calls it via `boardctl(BOARDIOC_INIT)` after
  `esp32s3_bringup()` has registered the LCD/touch/camera — so the device boots
  straight into its UI while keeping the shell available for debugging.

---

## 4. The hardest problem: hardware-SPI display

This is the standout piece of hardware debugging in the project.

**Symptom.** Over hardware SPI, a *full-screen* write rendered, but *any partial
window* (`CASET`/`RASET` to a sub-rectangle + `RAMWR`) never appeared — while the
identical byte sequence over a bit-banged GPIO path worked perfectly. It survived
~60 build cycles of software and config changes (framebuffer mirroring, DMA on/
off, software CS, PSRAM-coherency fixes, pin-routing forcing).

**Method — measure, don't guess.** With a logic analyzer (sigrok/PulseView on an
FX2 clone), the SPI pins can't be probed directly — they're not on the header —
so the board firmware **mirrors** `FSPID_OUT`/`FSPICLK_OUT` onto spare header
GPIOs via the GPIO matrix, plus a trigger marker. A decisive first step was a
**ground-truth test**: drive four known square waves on the probe pins and refuse
to trust any capture until they read clean. That immediately exposed a wiring/
grounding problem that had been producing garbage — and only *then* did the real
capture become trustworthy.

**Finding.** The decoded capture showed the ESP32 emitting a **byte-perfect**
`CASET 00 28 00 8B / RASET 00 28 00 8B / RAMWR / pixels` stream — correct
commands, correct DC alignment, exactly 8 clean clock edges per byte, zero
glitches. The peripheral was innocent all along.

**Root cause.** The panel's **chip-select is hardwired low**, so it can never
re-synchronize its bit counter on a CS edge — it counts clock edges forever. The
firmware bit-banged the panel init and *then* handed the pads to the SPI
peripheral; that pad handover glitches the clock by a single edge, and from that
instant the panel is off by one bit. Every subsequent command decodes as garbage,
`RAMWR` is never recognized, no pixels reach GRAM, and the screen simply keeps
showing whatever bit-bang last painted — which looked exactly like "hardware SPI
renders nothing."

**Fix.** Give the pads to the SPI peripheral **first**, then **hardware-reset the
panel** (a software reset can't work — it would itself be misread), then run the
entire init over SPI. Once the panel is re-synced under SPI ownership, partial
windows render correctly.

**Payoff.** With the path proven, the display was moved off bit-banging entirely
and onto hardware SPI (see §5). A bug that looked like a dead end became a
performance win.

---

## 5. Performance engineering

**ESP-NN on the LX7 (≈5× inference speed-up).** The identity model is mostly
depthwise convolution, which runs slowly on the TFLite-Micro *reference* kernels
(~6.75 s / inference). Wiring in the **ESP-NN** SIMD kernels for the Xtensa LX7
brings it to **~1.3 s**. The subtle part was that the ESP-NN kernel set has to be
dropped into the tflite-micro kernel directory and the matching *reference*
kernels filtered out to avoid duplicate symbols; and a class of "crash on first
conv" turned out to be **stale incremental builds**, not a kernel bug — a clean
build runs stably.

**Hardware SPI + DMA display (≈4× UI repaint speed-up).** Once the SPI path was
correct (§4), the display went from bit-banging every pixel (CPU toggling GPIOs)
to the hardware SPI2 peripheral at **40 MHz with GDMA**. A full 480×320 repaint
dropped from **~246 ms to ~61 ms**. Two details mattered: pixel rows are bounced
through an internal-SRAM buffer because SPI DMA can't read PSRAM cache-coherently,
and the address-window setup was batched from 11 SPI calls down to 5 (LVGL sets a
window on every `putarea`).

---

## 6. Build & reproducibility notes

A few non-obvious constraints, recorded so the build is reproducible:

- **App/library define parity.** The app and the prebuilt `tflite-micro` library
  share headers, so header-visible defines must match exactly. In particular
  `TF_LITE_STATIC_MEMORY` changes the layout of `TfLiteTensor` — defining it on
  only one side lets `AllocateTensors` pass but makes the first `Invoke` write
  through a mis-offset pointer and wedge the SoC. The Makefile mirrors the
  library's define set exactly.
- **`-fno-threadsafe-statics`** is required on the C++ flags: without it the first
  function-local static in TFLM init deadlocks in `__cxa_guard_acquire` on this
  libsupc++ build (a silent hang, not a crash).
- **Config changes re-clone esp-hal**, which reverts a spinlock initializer patch;
  the first build after any defconfig change may need the patch re-applied.

### ESP-NN acceleration — a public-repo change, provided as a fork/PR

The ~1.3 s inference figure requires the **ESP-NN** SIMD kernels, and that
integration modifies a **public** openVela repo (`apps/mlearning/tflite-micro`).
Per the contest rules — *public-repo changes are submitted as a fork-and-PR to
`dev-ai-contest-2026`, not committed into the team repo* — the ESP-NN work is
delivered as a fork/PR rather than vendored here. **Out of the box, this team
repo builds and runs with the reference kernels (~6.75 s); applying the ESP-NN
fork brings it to ~1.3 s.**

The integration consists of three parts:

1. **The esp-nn library** (Espressif's Apache-2.0 SIMD kernels for the Xtensa
   LX7) added at `apps/mlearning/esp-nn/`.
2. **Kernel wrappers** (`conv`, `depthwise_conv`, `fully_connected`, `pooling`,
   `add`, `mul`, `softmax`) dropped into
   `apps/mlearning/tflite-micro/tflite-micro/tensorflow/lite/micro/kernels/esp_nn/`.
3. **A Makefile block** in `apps/mlearning/tflite-micro/Makefile` that compiles
   the esp-nn sources with `-DESP_NN=1 -DCONFIG_NN_OPTIMIZED -mlongcalls`, adds
   its include dirs, and **filters out the matching reference kernels** so there
   are no duplicate symbols.

The exact wrappers, the Makefile block, and a step-by-step reproduction recipe
are provided as reference material in [`docs/esp-nn/`](esp-nn/).

> **PR:** _`<fork/PR URL — fill in after opening the PR>`_

Two gotchas found during the integration: the esp-nn assembly sources
(`*esp32s3.c`) are excluded (the opt-C path is used); and an early "crash on the
first conv" was traced to **stale incremental builds**, not a kernel fault — a
clean rebuild runs stably.

---

## 7. Off-device: model training

The two INT8 models are trained off-device with the pipeline in [`host/`](../host/)
(TensorFlow/Keras). The identity model is a MobileNetV3-Small backbone with a
spatial-attention **TSFM** module and an ArcFace metric-learning head; it is
exported via INT8 post-training quantization to a `.tflite` whose ops are all
TFLM builtins. The body-condition model is a compact 3-class CNN trained the same
way. See [`docs/BENCHMARK_AB.md`](BENCHMARK_AB.md) for the fast-vs-accurate model
comparison.

---

*This document reflects the engineering as built and verified on real ESP32-S3
hardware.*
