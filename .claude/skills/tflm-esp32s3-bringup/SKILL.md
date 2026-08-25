---
name: tflm-esp32s3-bringup
description: Bring TensorFlow Lite for Microcontrollers (TFLM) inference up and running on an ESP32-S3 openvela/NuttX app. Use when integrating TFLM into an openvela app, when init silently hangs after printing a model version (no crash dump, LVGL/UI just freezes), when a `const` model array partially renders or bus-stalls, or when inference works for a while and then wedges after N calls.
---

# TFLM bring-up on ESP32-S3 / openvela

Four gotchas cost a combined multi-day debugging session on VelaPaw (an on-device
pet-recognition feeder). None of them show up in TFLM's own docs because they're
specific to the ESP32-S3 + openvela/NuttX combination. Read this before wiring TFLM
into a new app, not after hitting the hang.

## Quick diagnosis

| Symptom | Cause | Jump to |
|---|---|---|
| Init prints up to `model version=N`, then **silent hang, no crash dump**, UI freezes solid | missing `-fno-threadsafe-statics` | [§1](#1-the-silent-hang-at-init----fno-threadsafe-statics) |
| A `const` model array compiles, but reads past a few hundred KB stall the bus / return garbage | model too big to memory-map as flash rodata | [§2](#2-model-too-big-for-flash-mapped-rodata) |
| Changed only a `Makefile` flag, behavior didn't change | stale object file — the flag never reached the compiler | [§3](#3-flag-only-changes-dont-recompile) |
| Runs fine for ~20-40 inferences (model-size-independent), then hard-hangs, heap flat (not a leak) | a DMA peripheral (e.g. camera) left mid-transaction between captures | [§4](#4-the-n-inferences-in-then-wedge) |

## 1) The silent hang at init — `-fno-threadsafe-statics`

**Symptom:** init logs run up to loading the model (`model version=3` or similar),
then nothing — no crash, no dump, the UI task starves and the screen just freezes.
It looks exactly like `AllocateTensors` is hanging, which sends you debugging the
wrong function.

**Root cause:** the first C++ function-local `static` your code reaches at runtime
(commonly `MicroMutableOpResolver` inside your TFLM init) triggers the C++
thread-safe static-init guard, `__cxa_guard_acquire`. On this libsupc++/NuttX
combination that guard deadlocks.

**Fix:** add to the app's Makefile, *before* `include $(APPDIR)/Application.mk`:

```make
CXXFLAGS += -fno-threadsafe-statics
```

Then `build interpreter → AllocateTensors OK → init ret=0` and the app proceeds
normally. This is a flag-only change — see §3, it will not take effect until you
force the relevant `.cc` to recompile.

## 2) Model too big for flash-mapped rodata

An ESP32-S3 with 8 MB PSRAM maps only a limited number of MMU pages for
flash-mapped (DROM) rodata. In practice a `const` model array larger than roughly
**256 KB** only partially maps — reads past that point either bus-stall or return
garbage, and `esp32s3_mmap`-based paths return `-ENOBUFS` even for a single extra
page.

**Decision rule:**
- Model **< ~100 KB** → embed as `const` rodata (`#include "model_data.h"`), it fits
  the mapped window, loads instantly. Simplest path — use it whenever the model is
  small enough.
- Model **larger** → do not embed it. Flash it to a raw offset and load it into
  PSRAM at runtime (§2a).

### 2a) Loading a model from flash (byte-perfect, verified)

```
esptool write-flash 0x600000 model.tflite
```

Reading it back needs three things, or it silently fails or returns error 24577:

1. `CONFIG_ESP32S3_SPI_FLASH_DONT_USE_ROM_CODE=y` (+ `ESP32S3_SPIFLASH=y`,
   `ESP32S3_MTD=y`) so NuttX's **software** `spi_flash_read` gets compiled in. The
   ROM `spi_flash_read` (the default, `PROVIDE`d at a fixed address) returns error
   24577 on this setup; `spi_flash_read_encrypted` / `esp32s3_mmap` both need MMU
   pages and hit the same `-ENOBUFS` as §2.
2. The software read path needs its on-stack SPI buffer in **internal RAM**. If
   your app's task stack lives in PSRAM (common for a PSRAM-heavy app), the read
   will corrupt or fail. Run the read on a **short-lived pthread whose stack is a
   static `.bss` buffer** (internal RAM by default), and read straight into your
   PSRAM destination buffer — the driver cache-toggles correctly across that
   boundary.
3. Budget the time: ~45 s for 1.4 MB on this path. Slow but correct — do it once
   at boot, not per-inference.

Confirmed working end-to-end: a 1.4 MB MobileNetV3-class model loaded this way,
`AllocateTensors OK` with a PSRAM arena, real inference producing correct results.

## 3) Flag-only changes don't recompile

A `Makefile`-only change (a new `-D` flag, a new `CXXFLAGS`/`CFLAGS` entry) does
**not** trigger a rebuild of the `.c`/`.cc` files that need it — the build system
only recompiles on a source content change, and if the object is already archived
into a static lib (`libapps.a`), even deleting the loose `.o` is not enough because
of a `.built` sentinel.

**What actually forces the recompile:**

```bash
find apps/<your_app> -name '.built' -delete
find apps/<your_app> -name '*.o' -delete
```

...plus an *actual* content change to the source file that consumes the flag (a
`touch` alone is not reliably enough on all these toolchains — a trivial content
edit, e.g. appending a comment, is what worked). Then rebuild.

**Verify the flag actually landed**, don't just trust the rebuild:

```bash
xtensa-esp32s3-elf-nm <object.o> | grep <symbol_only_built_with_the_flag>
```

A `-j4`+ parallel build interleaves its log output and can hide the one `CC:` line
that would have told you the file never recompiled.

## 4) The N-inferences-in, then-wedge

**Symptom:** inference works correctly for a while — roughly N calls, where N is
*not* strongly dependent on model size (a tiny 2.7 KB model wedges around the same
ballpark call count as a 1.4 MB one, just a bit later) — then the whole board
hard-hangs. No leak: instrument `mallinfo` per inference and the heap is dead
flat, identical every call. This rules out the model/arena and points at a
resource invisible to the heap allocator.

**Root cause (found via a capture-once isolation test):** a DMA-driven peripheral
running concurrently with inference — in this case a camera feeding live preview
frames via LCD_CAM + GDMA — was left **enabled and mid-stream** between captures.
The DMA descriptor / AFIFO state accumulates corruption over N frames and
eventually wedges the whole SoC (symptom at the console: a semaphore timeout, not
a crash).

**Fix pattern:** make every DMA capture/transfer fully atomic — explicitly disable
the DMA channel and clear the peripheral's "start" bit at the *end* of every
capture, and defensively disable-then-reload right before the *next* one starts:

```c
/* end of each capture */
esp32s3_dma_disable(g_dma_chan, false /* tx=false for an RX channel */);
/* clear the peripheral's own START/STREAMING bit, e.g. */
creg(R_PERIPH_CTRL1, B_PERIPH_START);

/* right before re-arming for the next capture */
esp32s3_dma_disable(g_dma_chan, false);
esp32s3_dma_load(...);
```

This took the wedge threshold from ~20-40 captures to 140+ in isolation, and to
fully demo-stable once combined with bounding inference to on-demand (only run
inference when the feature is actually in use, not continuously) rather than
free-running.

**If you don't have a DMA peripheral competing with inference** and still see this
pattern, mallinfo-instrument first to rule out a leak, then look for any other
peripheral (audio, another sensor) sharing DMA channels or interrupt priority with
your inference loop — the signature (call-count-correlated, heap-invisible,
model-size-independent) is the tell.

## Optional: SIMD acceleration (ESP-NN)

Reference TFLM kernels on the Xtensa LX7 run INT8 conv at roughly **6.75 s per
inference** for a MobileNetV3-Small-class model. `-DCMSIS_NN` is a no-op here —
CMSIS-NN's fast paths are ARM-only and silently fall back to the C reference on
LX7. The real SIMD path is **ESP-NN**, wired in as a per-kernel override
(mirroring however your tree already structures a `-DCMSIS_NN` block): copy just
the `conv.cc` wrapper into `.../kernels/esp_nn/`, add ESP-NN's C sources to the
build, exclude the matching reference kernel so there's no duplicate symbol, and
build with `-DESP_NN=1 -DCONFIG_NN_OPTIMIZED -mlongcalls` (leave out
`-DCONFIG_IDF_TARGET_ESP32S3` — the plain opt-C path was the stable one here).

**Two things that will burn time if you skip them:**
- **`make clean` before judging any ESP-NN result.** A stale incremental build
  produced a reproducible-looking "crashes on first conv" that had nothing to do
  with ESP-NN — it was purely the stale build.
- **Conv-only is the sweet spot.** Conv dominates the MACs, so wrapping only the
  conv kernel gets ~all the speedup (measured 6.75 s → 1.73 s, ~3.9x) with far
  less surface area than wrapping every op — fully wrapping add/mul/pooling/
  softmax/fully_connected as well did not measurably beat conv-only and reduced
  stability with no upside.
