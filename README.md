<b>English</b> | <a href="README_zh.md">中文</a>

# VelaPaw — an on-device AI multi-pet smart feeder

**Team:** CircuitForge (`contest2026_043`) · **Track:** AI Hardware Product Innovation (AI 硬件产品创新)
**Platform:** openVela (NuttX) on a Waveshare **ESP32-S3-Touch-LCD-3.5-C**
**Repository:** [github.com/open-vela/contest2026_043_CircuitForge](https://github.com/open-vela/contest2026_043_CircuitForge) — source code submitted to the contest branch **`dev-ai-contest-2026`**

---

## Demo Video

https://github.com/user-attachments/assets/7cf3a67b-49a3-45f4-a800-5e8f92b570e9




▶ If the embedded video above doesn't load, [download it directly](VelaPaw_Demo.mp4) (93MB, MP4). We recommend you download the video for better quality.

---

## 1. What it is

**VelaPaw is a smart pet feeder that recognizes *which* pet is at the bowl and dispenses that individual pet's scheduled meal — with every bit of AI running on the device itself. No cloud, no phone, no account.**

In a multi-pet home, a normal timed feeder can't tell the cats apart, so the greedy one eats everyone's food and portion control is impossible. VelaPaw solves this the way a human would: it *looks* at the animal, identifies it, and feeds it only its own ration, at its own mealtimes. Because the camera, the neural networks, the clock, and the dispenser are all self-contained, it keeps working with the internet down and never sends images of your home anywhere.

**Highlights**

- 🐾 **Per-pet identification** — an on-device INT8 neural network recognizes each enrolled pet by face, not by a tag or collar.
- 🍽️ **Recognition-gated feeding** — a pet is fed only when it is *recognized at the bowl* **and** one of *its* meals is due (and not skipped), at most once per meal per day. Each meal slot has its own **skip-meal** toggle.
- 🗣️ **Voice-settable schedules** — meal times can be spoken, not just tapped: a MEAL→HOUR→CONFIRM voice dialog, driven by an on-device keyword-spotting model, sets a pet's feeding schedule hands-free. There is no wake word — the dialog is started with a tap in the edit-pet screen, then answered by voice. VelaPaw has no always-listening wake-up feature, so the contest's designated wake-word requirement (§ Participation Rules) does not apply here.
- ⚖️ **Portion & daily-limit control** — per-pet gram portions with a hard daily cap.
- 🩺 **Health monitoring** — a second on-device network estimates Body Condition (under / ideal / over), and the feeding-history analytics raise a **sudden-appetite-drop alert**, an early illness sign.
- 🤖 **On-device AI agent that speaks up first** — the openVela `ai_agent` framework runs on the board with a custom Skill that reads the *real* feeding records. On a timer it **pushes a warning by itself** when a pet crosses a concern threshold — unprompted, with nobody at the console. A quiet device means nothing is wrong. See [§4](#4-the-on-device-ai-agent-the-one-part-that-needs-internet).
- 🕑 **Real timekeeping** — a hardware RTC keeps schedules accurate across power loss.
- 🌐 **Bilingual UI** — English / 中文, with a live in-app language toggle.
- 💾 **Everything persists** — enrolled pets, schedules, and pet photos survive a power cut.
- 🖥️ **Runs as an appliance** — boots straight into a touchscreen UI; no console needed.

**Why now:** in March 2026, Xiaomi shipped the Mijia Smart Pet Feeder 2 (Visual Edition) — a mass-market feeder (¥449–549, ≈$65–80) that added a camera specifically to bring AI vision into feeding. That's independent, at-scale validation of VelaPaw's founding bet — a camera belongs at the feeder — carried one step further: from watching a bowl to knowing *whose* bowl it is, identified by face and fed entirely on-device with no app, account, or cloud. Full comparison in [§06 Market & Value](VelaPaw_Project_Intro.pdf) of the project intro.

---

## 2. On-device AI (the heart of the project)

Recognition is **open-set metric learning**, not a fixed classifier. The model is a generic pet feature-extractor (MobileNetV3-Small backbone + a spatial-attention *TSFM* module) that turns a camera frame into an L2-normalized 128-D **embedding**. Enrolling a pet stores the averaged embedding of a few photos; recognition is a cosine match against the enrolled pets. This is why **adding a pet needs no retraining** — enrollment is a first-class step, and recognition quality is driven by good enrollment photos rather than a frozen label set.

| | |
|---|---|
| **Identity model** | MobileNetV3-Small + TSFM attention, INT8, 128-D embedding, 128×128 input |
| **Body-condition model** | 3-class (under/ideal/over) INT8 CNN |
| **Runtime** | TensorFlow Lite for Microcontrollers (`apps/mlearning/tflite-micro`) |
| **Acceleration** | ESP-NN SIMD kernels on the Xtensa LX7 → **~1.3 s / inference** (vs ~6.75 s reference) |
| **Where models live** | flash-loaded at runtime (1.4 MB identity @ `0x600000`, 626 KB BCS @ `0x760000`) — too large to memory-map, so read into PSRAM on boot |

Both models were trained off-device with the pipeline in [`host/`](host/) (TensorFlow/Keras → INT8 post-training quantization → `.tflite`).

---

## 3. How it works — from camera to kibble

```
   camera frame ──► embedding (TFLM, INT8) ──► cosine match vs enrolled pets
                                                        │
                                             recognized pet + score
                                                        │
                                        is one of THIS pet's meals due & unfed today?
                                                   │yes            │no
                                              dispense N g      show "next meal 18:00"
                                       (28BYJ-48 stepper, paddle rotor)
                                                   │
                                        log to history · update trends · BCS check
```

The gate in the middle is the whole idea: recognition alone would feed a greedy pet all day; a timer alone would feed whichever animal happened to be standing there. **Both must agree.**

**Software architecture** (clean HAL boundaries, in [`app/velapaw/`](app/velapaw/)):

- `ui/` — LVGL touchscreen UI (Enroll · Recognize · My Pets · Trends)
- `infer/` — inference interface + the TFLite-Micro backend + the trained models
- `identity/` — enrollment store, cosine matcher, per-pet flash persistence, pet photos
- `store/` — feeding history & behavior analytics
- `hal/` — camera (OV5640) and feeder (28BYJ-48 stepper) hardware abstraction

**Board support** ([`board/`](board/)) — a custom board file drives the display, the OV5640 camera via the ESP32-S3 LCD_CAM peripheral, the PCF85063 RTC over I²C, and the feeder's **28BYJ-48 stepper** through a ULN2003 driver on four GPIOs (IN1–IN3/IN4 on GPIO9/10/11/**43**). The display runs over **hardware SPI at 40 MHz with DMA**. The ES8311 audio codec's private I²S bus (GPIO12–16, not routed to any header) drives the onboard mic and speaker for the voice-settable schedules.

---

## 4. The on-device AI agent (the one part that needs internet)

The openVela **`ai_agent`** framework runs on the same ESP32-S3, as a layer *on top of* the finished feeder. It gives VelaPaw a second way to be useful: instead of the owner going to the screen to check on a pet, the device reads its own feeding records and speaks up when something looks wrong.

**What runs on the device**

| | |
|---|---|
| **Custom Skill** | A feeding-digest Skill at `/data/ai_agent/skills/` — it reads VelaPaw's own feeding records (written to flash by the feeder every time it dispenses) and turns them into a per-pet summary: grams, meals, and appetite measured against that pet's own baseline. |
| **Ask it (CLI)** | `ask how much did bob eat today` — the agent reads the live records off the device's flash and answers from them, not from a canned reply. |
| **Proactive push** | A heartbeat task re-reads the records on a timer and pushes a warning **only when a pet crosses a concern threshold** — a quiet device means nothing is wrong. It fires unattended, with nobody at the console. (The demo build uses a short 3-minute interval so the push can be seen firing within a demo; a shipping product would use hours.) |

**Offline vs online — where the line is**

> **The feeder itself never needs the internet.** Recognition, the meal schedule, portion and daily-limit control, body-condition scoring, the feeding history and trends, the voice dialog and the entire UI all run on the ESP32-S3 with no network of any kind. Unplug the network and VelaPaw keeps identifying pets and feeding them on schedule, exactly as before.
>
> **The agent layer is the only part that needs the internet**, because it talks to a hosted LLM over the device's USB network interface (CDC-NCM). With the network down, `ask` and the proactive push stop — and nothing else does. No feeding decision is ever made in the cloud, and no image ever leaves the device.

Full write-up — architecture, the Skill, the framework patches, and the captured proactive push: **[docs/AI_AGENT.md](docs/AI_AGENT.md)** ([中文](docs/AI_AGENT_zh.md)). The device-side source and patches are in [`agent/`](agent/).

---

## 5. Hardware

- **Waveshare ESP32-S3-Touch-LCD-3.5-C** — ESP32-S3, 8 MB PSRAM, 16 MB flash, 320×480 ST7796 touch LCD, OV5640 camera, PCF85063 RTC.

> **Why this board — same SoC, right peripherals.** This board carries the **same ESP32-S3** as the contest-provided ESP32-S3-EYE, so the openVela port, the on-device inference, and every driver are identical work on that chip. What VelaPaw additionally needs is a **capacitive touch display** — enrolling a pet, editing meal schedules and reviewing trends all happen on-screen — and a **real-time clock** so scheduled feeding survives power loss. The ESP32-S3-EYE is a vision/voice development board with a small non-touch screen and no RTC; the Waveshare Touch-LCD-3.5-C integrates the 3.5″ touch LCD, OV5640 camera and PCF85063 RTC the product depends on, in a single board. Same platform, chosen carrier.
- **Feeder** — a **28BYJ-48 stepper**-driven **paddle-rotor** dispenser (via a ULN2003 driver): a vaned rotor meters a fixed pocket of kibble per step, so the portion depends on rotation, not on how full the hopper is. Fully parametric 3D model + printable STLs and BOM in [`hardware/`](hardware/).
- **Camera on a 30 cm FFC** — the OV5640 is unplugged from the board and run out on a 30 cm ribbon + 1:1 coupler to its own **`cam_mast`** yoke in front of the bowl, so the screen faces the owner while the lens watches the pet. (A reversed FFC is fatal on power-on — orientation is meter-verified; see [`hardware/README.md`](hardware/README.md).)

---

## 6. Repository layout

```
app/velapaw/     — the device application (linkfile'd to packages/demos/ by the manifest)
agent/           — the ai_agent layer: custom Skill, heartbeat task, and the
                   patches against packages/ai_agent (see agent/README.md)
board/           — board files: display/camera/RTC/stepper (esp32s3_st7789.c),
                   auto-start (esp32s3_appinit.c), and the board defconfig
host/            — off-device model training & export (TensorFlow/Keras)
hardware/        — 3D-printable feeder (OpenSCAD source + STLs + BOM)
docs/            — technical notes and benchmark reports
logs/            — AI-Coding session logs (this project was built with AI assistance)
```

---

## 7. Build & run (real hardware)

```bash
# 1. Pull openVela + this team repo
repo init -u https://github.com/open-vela/contest2026_043_CircuitForge \
  -b dev-ai-contest-2026 -m contest2026_043_CircuitForge.xml
repo sync -c -j8

# 2. Place the board files into the ESP32-S3 board tree
BOARD=nuttx/boards/xtensa/esp32s3/esp32s3-devkit
cp contest2026_043_CircuitForge/board/esp32s3_st7789.c   $BOARD/src/
cp contest2026_043_CircuitForge/board/esp32s3_appinit.c  $BOARD/src/
cp contest2026_043_CircuitForge/board/esp32s3_bringup.c  $BOARD/src/
cp contest2026_043_CircuitForge/board/configs/waveshare_lcd/defconfig \
   $BOARD/configs/waveshare_lcd/defconfig

# 3. Build (from the openVela workspace root)
./build.sh esp32s3-devkit:waveshare_lcd -j8

# 4. Flash firmware + the two on-device models
esptool --chip esp32s3 --port <PORT> write-flash \
  0x0        nuttx/nuttx.bin \
  0x600000   contest2026_043_CircuitForge/app/velapaw/infer/model/velapaw.tflite \
  0x760000   contest2026_043_CircuitForge/app/velapaw/infer/model/bcs.tflite
```

On boot the device runs the feeder UI directly. **Enroll a pet** (capture a few tight, well-lit face photos), then the **Recognize** tab identifies it and feeds on schedule.

> **Note on inference speed:** the ~1.3 s figure uses the ESP-NN accelerated kernels. See the [engineering deep-dive](docs/ENGINEERING.md) for the acceleration setup, the hardware-SPI display debug, and reproducibility notes.

---

## 8. AI-assisted development

This project was built end-to-end with AI-assisted (AI Coding) development, and the full session logs are committed under [`logs/`](logs/) as required. AI was used throughout the lifecycle:

- **Requirements & design** — shaping the recognition-gated feeding model, the open-set enrollment approach, and the health-monitoring analytics.
- **Firmware & drivers** — bringing up the display, OV5640 camera (LCD_CAM), RTC, and the 28BYJ-48 stepper feeder on the ESP32-S3; wiring TensorFlow Lite Micro + ESP-NN onto the Xtensa LX7.
- **Hard debugging** — the standout example: a hardware-SPI display bug that survived ~60 blind build cycles was cracked with a **logic analyzer**, root-caused to the CS-tied-low panel losing bit-sync at the pad handover, and turned into a **display speedup** (bit-bang → hardware SPI + DMA).
- **Product iteration** — the full pet lifecycle (enroll, recognize, feed, edit, delete, photos), all tested on real hardware.

---

## 9. Status

**On-device `ai_agent`:** running on hardware — the custom feeding-digest Skill, the `ask` CLI channel, and the proactive push, captured firing unattended. It is the only part of VelaPaw that needs an internet connection; everything in the feeder itself keeps working with the network down. See [§4](#4-the-on-device-ai-agent-the-one-part-that-needs-internet).

**Working on hardware today:** on-device recognition, body-condition scoring, recognition-gated 3-meals/day scheduling with a per-slot skip-meal toggle, portion & daily-limit control, feeding history & trends with appetite-drop alerts, hardware RTC, full pet lifecycle with persistent photos, auto-start, a hardware-SPI + DMA display, a bilingual EN/中文 UI with a live toggle, a spoken MEAL→HOUR→CONFIRM meal-scheduling dialog (hardware-proven end to end), the **28BYJ-48 stepper feeder** (turning on GPIO9/10/11/43), the **OV5640 relocated onto a 30 cm FFC** and confirmed working through the coupler + `cam_mast` mount, and the **3D-printed enclosure fully assembled** — stepper dispenser and relocated camera mounted into the printed structure.

---

## 10. Datasets

Datasets used in this project can be found at: https://drive.google.com/drive/folders/1-HnbS-VStbKuXCXWK-O1Nh2lNLdqMpbI

---

*Built by team CircuitForge for the 2026 openVela AI Hardware Developer Contest.*
