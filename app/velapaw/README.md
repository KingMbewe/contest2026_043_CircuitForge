# VelaPaw — multi-pet recognition smart feeder (contest2026_043)

On-device app: a camera recognizes *which individual pet* is at the bowl and
dispenses that pet's portion, with per-pet cooldown and feeding history.
Everything runs on-device (no cloud).

This directory is the **device application**. It is exposed into the openVela
build tree via a manifest `<linkfile>`:

```
app/velapaw  ->  packages/demos/contest2026_043_velapaw
```

## Status: running on real hardware

This app now runs on the real **Waveshare ESP32-S3-Touch-LCD-3.5-C** with all the
platform pieces backed by real drivers. The same clean HAL boundaries still keep
an **emulator build** working (mocks/stubs), so it can compile and run under QEMU
without a board — each HAL just selects its backend at build time:

| Module | Emulator backend | Hardware backend (shipping) |
|--------|------------------|------------------------------|
| `hal/camera.*`   | `camera_mock.c` (synthetic scenes) | **OV5640** via the board's LCD_CAM driver (on a 30 cm FFC to the `cam_mast` mount) |
| `infer/infer.*`  | `infer_stub.c` **or** `backend_tflm.cc` (TFLM INT8, `CONFIG_VELAPAW_INFER_TFLM`) | **`backend_tflm.cc`** — host-trained INT8 embedding model, ESP-NN accelerated (~1.3 s) |
| `identity/`      | real cosine match + in-memory enroll | same, **persisted to flash** (`0x800000`) |
| `store/`         | `store_stub.c` (in-memory) | feeding history + analytics persisted to flash |
| `hal/feeder.*`   | `feeder_mock.c` (logs dispense) | **28BYJ-48 stepper** via ULN2003 (GPIO9/10/11/**43**) |

The real-hardware build (board files, flashing firmware + the two models) is in
the [root README](../../README.md); the emulator flow is below.

## Build

```bash
source /d/openVela/vela-env.sh
cd /d/openVela/nuttx
kconfig-tweak --file build/.config -e LVX_USE_DEMO_CONTEST2026_043_VELAPAW
# optional: real TFLite-Micro inference path (needs CONFIG_TFLITEMICRO):
kconfig-tweak --file build/.config -e VELAPAW_INFER_TFLM
vela_olddefconfig
cmake -B build -DBOARD_CONFIG="$VELA_BOARD_CONFIG" -GNinja
cmake --build build -j8
```

Run the emulator and at the `nsh>` prompt: `velapaw`. With the TFLM backend it
prints real per-inference latency + tensor-arena (measured ~82 KB arena,
~0.3 s/inference on the emulated cortex-a15 — not an ESP32-S3 figure).

## Pipeline (velapaw_main.c)

`init -> enroll demo pets -> loop: capture -> presence gate -> embed -> match ->
(cooldown ok?) dispense + log`.
