# Session State — "Resume From Here"

Last updated: 2026-08-08. Drop-in context so any new session (or a returning one) can continue with zero re-discovery.

## TL;DR status

- **Project:** VelaPaw — multi-pet recognition smart feeder, on-device AI (contest2026_043_CircuitForge). Track: AI Hardware Product Innovation.
- **Board:** Waveshare **ESP32-S3-Touch-LCD-3.5-C** (ESP32-S3, 8 MB PSRAM, 16 MB flash, ST7796 320×480 touch LCD, OV5640 camera, PCF85063 RTC).
- **Source checkout:** `D:\openVela` (NOT C:). Full openVela tree synced. Session anchor cwd is `C:\Users\VICTUS\openVela` (holds only `.claude`); real work is on D:.
- **Where it stands: running on real hardware.** On-device recognition (open-set metric learning, INT8), body-condition scoring, recognition-gated 3-meals/day scheduling with a **per-slot skip-meal toggle**, portion + daily-limit control, feeding history/trends with appetite-drop alerts, hardware RTC timekeeping, full pet lifecycle with persistent flash storage + photos, auto-start into the UI, a hardware-SPI + DMA display, a **bilingual EN/中文 UI** with a live toggle, and a **spoken meal-scheduling dialog** (MEAL→HOUR→CONFIRM by voice). The **28BYJ-48 stepper feeder** turns, and the **OV5640 is relocated onto a 30 cm FFC** and confirmed working.
- **Baseline / build lineage:** golden binaries live in the VM at `~/velapaw-golden/`: `pre-audio/` (2026-07-24, no audio), `b68/` (2026-08-06, first hardware-proven voice dialog). Windows-side source snapshots mirror these at `D:\openVela\velapaw-snapshots\` (`2026-08-02-pre-audio`, `2026-08-06-b68-voice-working`, `2026-08-07-b81-preopen`, `2026-08-07-b83-noaudlog`, `2026-08-08-b82r-rebuild`, `2026-08-08-skipmeal`). ⚠️ **`skipmeal` — the build currently flashed on the board — was built on top of `b82r`, not `b83`/`b81`.** It has the skip-meal feature working but has **regressed on audio**: the console-blocking fix from b83 is not in it, and the mic fails every capture. The last build with audio fully working (voice dialog + no console-blocking) was `b83`/`b81`, which does **not** have skip-meal. No single flashed image currently has both working at once — see [[velapaw-golden-baseline]] and [[velapaw-console-blocking]] in memory for the full lineage.
- **Voice / audio:** see the new "Voice & audio" section below.
- **Next:** rebuild on top of `b83` (or later) with the skip-meal patch re-applied so one image has both audio and skip-meal working; final mechanical assembly (mount the stepper dispenser + relocated camera into the printed enclosure); the intermittent `-110` I²S RX capture stall is not fully root-caused (see `[[velapaw-mic-capture]]`).
- **Console:** `py -m serial.tools.miniterm COM4 115200` from a Windows terminal (quit Ctrl-]). Scripted `SerialPort` access to COM4 gets echo but never an `nsh>` prompt — use miniterm. As of b83, the console-blocking bug is fixed and a PC does **not** need to be attached to COM4 for the mic to work (not true on the currently-flashed `skipmeal` build — see baseline note above).

## What works on hardware today

- **Recognition:** MobileNetV3-Small + TSFM attention → L2-normalized 128-D embedding; cosine match vs enrolled pets; match threshold 0.45. ESP-NN kernels bring inference to **~1.3 s** (vs ~6.75 s reference). Inference runs on a **background worker thread** so the LVGL UI stays live.
- **Body condition:** compact 3-class INT8 CNN (under/ideal/over).
- **Feeding:** recognition-gated — dispense only when *this* pet is recognized at the bowl **and** one of *its* meals is due, unfed today, **and not skipped**. Each meal slot has a per-slot **skip-meal toggle** (shipped + hardware-confirmed 2026-08-08). Portions in grams via the paddle-rotor; hard daily cap.
- **Scheduling / clock:** meal times gated by the **PCF85063 RTC** driven directly over I²C (raw register access). Meal times can be set either from the UI or **spoken** — a MEAL→HOUR→CONFIRM voice dialog in the edit-pet modal, hardware-proven end to end (build 68: "COMMIT meal 2 = 08:00", 9.6 s, zero false accepts). Voice writes only the modal's scratch copy, so it can never bypass Save/Cancel. See "Voice & audio" below.
- **UI:** LVGL four-tab touch UI (Enroll · Recognize · My Pets · Trends). Trends tab has colored intake bars + appetite line chart + a 3-zone BCS gauge. **EN/中文 bilingual string table with a live language toggle, shipped and confirmed on hardware** (CJK font subset embedded; language selection is RAM-only — it is not persisted to flash, since flash offset `0x818000` is off-limits, see the "do not" list).
- **Display:** ST7796 over hardware **SPI2 @ 40 MHz + GDMA** (full 480×320 repaint ~61 ms).
- **Persistence:** enrolled pets/schedules/embeddings at flash `0x800000`; pet photos at `0x810000`.

## Voice & audio — current status

- **What it is:** a 14-label `tiny_conv` KWS model (trained on public Speech Commands, host/kws/) drives a constrained per-step decode — each dialog step (MEAL: one/two/three; HOUR: zero..nine; CONFIRM: yes/no) takes the argmax over only its legal words, using the **raw, non-renormalized** softmax score (renormalizing lets ~86% of silence/OOV through; raw score, ~4.9%). Thresholds (hardware-tuned): MEAL 0.40, HOUR 0.55, CONFIRM 0.55, 15 s timeout, 5 tries. A **vote rule** (2 votes, 0.25 floor) also accepts a word heard twice below threshold — this is load-bearing, not redundant: it recovered a whole dialog on build 69 from three sub-threshold HOUR readings.
- **Proven working end to end on hardware**, build 68/69/81 (2026-08-06/07): capture → KWS → constrained decode → commit, e.g. "heard two 0.773 → heard eight 0.938 → heard yes 0.957 → COMMIT meal 2 = 08:00". b81 measured "3 of 5 cold boots work."
- **Console-blocking bug SOLVED in b83 (2026-08-07, hardware-confirmed):** the board used to need a PC attached to COM4 to work at all, because kernel syslog blocked on the 193-byte CDCACM TX buffer whenever the audio driver's `auderr`/`audwarn` logging fired inside the capture worker thread. Fix was turning `CONFIG_DEBUG_AUDIO_ERROR`/`_WARN` off (a bigger buffer does **not** fix it — only delays the fill). One cold cycle + five RESETs, no terminal, mic worked.
- **⚠️ Known open issue — intermittent `-110` capture stall.** A RESET tap reproduces it reliably; a cold boot does not reliably clear it, so a passing boot proves nothing. Not fully root-caused as of b83 — the TX-clock theory and the descriptor-desync theory (patch #31) have both been measured and falsified on hardware. See `[[velapaw-mic-capture]]` in memory for the live theory.
- **⚠️ The currently-flashed board build (`skipmeal`, built on `b82r`) does not include the b83 console-blocking fix or b81's audio state** — mic fails every capture on it. The skip-meal feature and working audio currently exist in two different binaries; see the baseline note in TL;DR above.
- Training details, thresholds, and the vote-rule rationale: `[[velapaw-kws-training]]`, `[[velapaw-voice-dialog]]`, `[[velapaw-mic-capture]]`, `[[velapaw-audio-bringup]]`, `[[velapaw-audio-gain]]` in memory.

## Hardware wiring — the facts that bite

- **Console is on the USB-Serial/JTAG port**, NOT UART0. This is why UART0 pins are free for other use.
- **Feeder = 28BYJ-48 stepper via ULN2003**, four GPIOs: **IN1/IN2/IN3/IN4 = GPIO9/10/11/43** (header J8 pins 14/12/10/27). 9/10/11 are shared with the unused microSD slot (its CS is on expander EXIO3 and stays deasserted); 43 is U0TXD, free because the console is USB-Serial/JTAG.
  - **Use GPIO43, NOT GPIO44.** GPIO44 (U0RXD) is clamped and won't drive the coil cleanly — this was half of the "buzzes but won't turn" symptom during bring-up (it was the pin, not the coil order).
  - **Never move the coils to GPIO12/13/14/15/16.** Those are the ES8311 codec's I²S bus and are **not routed to any header** — verified in the schematic, each net has exactly two nodes (ESP32 + codec). The other half of the buzz saga was IN1..IN3 sitting on 12/13/15, where three coils were electrically unconnected.
  - **Fully reconciled against the VM on 2026-08-02.** `board/esp32s3_st7789.c` now matches the golden copy (`~/velapaw-golden/pre-audio/esp32s3_st7789.c`): pins 9/10/11/43, **full-step** `k_fullstep[4] = {0x3,0x6,0xC,0x9}` with **bit0=IN1**, `STEP_PER_REV 2048`, `STEP_PER_POCKET ~341`, `STEP_DELAY_US 3000`, `GRAMS_PER_POCKET 5`, and `feeder_pins_output()` re-called at the top of every dispense. The repo had previously carried a half-step table with the **opposite bit order**, which would have driven the rotor backwards — that is now gone.
  - **Do not switch back to half-step.** Full-step energises two coils per phase (~2× torque); the rotor has to turn against a column of kibble.
  - Open calibration items: (1) confirm rotation actually carries food inlet→outlet; (2) weigh one pocket to replace the placeholder `GRAMS_PER_POCKET 5`; (3) `2048/6 = 341` and `341×6 = 2046`, so integer pockets lose 2 steps/rev — the vane drifts over hundreds of dispenses, wants a fractional-step accumulator.

- **Header J8 (2×16, 2.54 mm) — verified pinout.** Odd column left, even right:

  | | | | |
  |---|---|---|---|
  | 1 VBAT | 2 VBUS | 17 IO45 `CAM_D0` | 18 IO18 `CAM_HREF` |
  | 3 GND | 4 GND | 19 IO46 `CAM_D3` | 20 IO0 `IMU_INT`/strap |
  | 5 IO21 `CAM_D7` | 6 USB_N | 21 IO47 `CAM_D1` | 22 EN |
  | 7 IO38 `CAM_XCLK` | 8 USB_P | 23 IO48 `CAM_D2` | 24 PWRON |
  | 9 IO39 `CAM_D6` | 10 **IO11** | 25 IO44 `U0RXD` (clamped) | 26 IO7 `SCL` |
  | 11 IO40 `CAM_D5` | 12 **IO10** | 27 **IO43** `U0TXD` | 28 IO8 `SDA` |
  | 13 IO41 `CAM_PCLK` | 14 **IO9** | 29 GND | 30 GND |
  | 15 IO42 `CAM_D4` | 16 IO17 `CAM_VSYNC` | 31 3V3 | 32 3V3 |

  **There are no spare GPIOs left.** With the camera on J2 and the stepper on 9/10/11/43, every usable header pin is claimed. Any new peripheral must go on the I²C bus (IO7/IO8) or through the TCA9554 expander. **GPIO2 is not an option** — it is bonded to nothing at all (its net has one node, the SoC; Waveshare never routed the LCD read-back line, hence `CONFIG_ESP32S3_SPI2_MISOPIN=-1`).

- **Onboard audio is available and costs no GPIOs.** The mic (analog differential into ES8311 `MIC1P`/`MIC1N`) and the bundled 6 Ω 1 W speaker (via an NS4150B amp, enable = `PA_CTRL` on expander **EXIO7**) hang off the ES8311, whose I²S bus is the private IO12–16 group and whose control lines share the existing IO7/IO8 I²C bus. Nothing to wire. This is the only viable microphone path — an external I²S MEMS mic is impossible, there are no pins for it.
- **Camera OV5640 relocated onto a 30 cm FFC + 1:1 coupler** into the `cam_mast` yoke in front of the bowl (see `hardware/README.md`). **A reversed FFC is FATAL on power-on** — orientation is meter-verified. Verified chain: camera pin 1 → pigtail "1" → coupler P1.1(left) → P2.1(left) → 30 cm cable pin-1(dotted) → **J2 pin 1 = bottom end**. The camera's grounds are NOT bonded internally, so you can't verify orientation by probing *through* it — trust silk `1`/`24` marks + bare-copper continuity only.

## Critical "do not" list (each has cost us real time)

- **Never enable `CONFIG_RTC`** — it kills the USB-CDC console. The PCF85063 is driven directly over I²C instead.
- **Never raw-write `spi_flash` at offset `0x818000`** — it wedges the SoC.
- **`-fno-threadsafe-statics` is mandatory** on the C++ flags — without it the first function-local static in TFLM init deadlocks in `__cxa_guard_acquire` (silent hang, THE blue-screen cause).
- **App/library define parity** — the app and prebuilt `tflite-micro` share headers; `TF_LITE_STATIC_MEMORY` etc. must match on both sides or the first `Invoke` wedges the SoC. The app Makefile mirrors the library's `-D` set exactly (incl. `MicroPrintf`).
- Large models are **flash-loaded to PSRAM at boot** (too big to memory-map); the flash read runs on a short-lived thread with a `.bss` stack because flash reads suspend the cache.

## Build & run (real hardware)

```bash
# board files into the ESP32-S3 board tree, then build from the openVela root
BOARD=nuttx/boards/xtensa/esp32s3/esp32s3-devkit
cp contest2026_043_CircuitForge/board/esp32s3_st7789.c  $BOARD/src/
cp contest2026_043_CircuitForge/board/esp32s3_appinit.c $BOARD/src/
cp contest2026_043_CircuitForge/board/esp32s3_bringup.c $BOARD/src/
cp contest2026_043_CircuitForge/board/configs/waveshare_lcd/defconfig \
   $BOARD/configs/waveshare_lcd/defconfig
./build.sh esp32s3-devkit:waveshare_lcd -j8

# flash firmware + the two on-device models
esptool --chip esp32s3 --port <PORT> write-flash \
  0x0      nuttx/nuttx.bin \
  0x600000 contest2026_043_CircuitForge/app/velapaw/infer/model/velapaw.tflite \
  0x760000 contest2026_043_CircuitForge/app/velapaw/infer/model/bcs.tflite
```

(An **emulator build** still works via the HAL stubs — see `app/velapaw/README.md`.)

## ESP-NN note (reproducibility)

The ~1.3 s figure needs the **ESP-NN** SIMD kernels, which modify a **public** repo (`apps/mlearning/tflite-micro`). Per contest rules that change ships as a **fork/PR to `dev-ai-contest-2026`**, not vendored here — out of the box this repo runs the reference kernels (~6.75 s). Recipe + wrappers in `docs/esp-nn/`. See also `docs/ENGINEERING.md`.

## Where the durable context lives

- **Memory:** `C:\Users\VICTUS\.claude\projects\c--Users-VICTUS-openVela\memory\` (MEMORY.md auto-loads; per-topic notes for stepper, camera connector, RTC scheduling, recognize tuning, TFLM on-board, Trends, i18n, hardware).
- `docs/ENGINEERING.md` (+ `_zh`) — engineering deep-dive (AI pipeline, hardware-SPI debug, ESP-NN).
- `docs/PHASE0_REPORT.md` — Phase 0 (emulator boot on native Windows) history.
- `hardware/README.md` — feeder mechanics, stepper wiring, camera relocation + FFC orientation.
- `D:\openVela\vela-env.sh` — toolchain env + helpers.

## AI-Coding logs

Sessions in the openVela workspace auto-write to `logs/<github_login>/` at session end (contest-log-collector). **Write files only — no auto commit/push**; logs go up when code is pushed.
