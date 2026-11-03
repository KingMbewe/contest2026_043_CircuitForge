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

## Finals update — what's new since the preliminary round

Everything below was added after the preliminary submission (2026-09-06) and is verified on the board. The details are in §3.7.1 of the finals edition of the technical report ([English](docs/VelaPaw_Technical_Report-Finals.pdf) · [中文](docs/VelaPaw_技术报告-决赛版.pdf)).

- **The agent edits meals** — one new tool, `set_meal_schedule`: a typed sentence such as `ask change bob's dinner to 18:20` changes the time and saves it to flash, in about 5 s.
- **The agent on the touchscreen** — each `ask` and its reply appear as a toast under the bell; a heartbeat warning shows a red toast and turns the bell red; My Pets redraws live.
- **Appetite scored against the day's plan** — grams served ÷ grams due so far, with an alert below 50%; the baseline averages only days with food. A full day of planned meals now reads 100% instead of 65%, and an unplug no longer raises a false alert.
- **Stability** — recognition re-arms itself, console prints never block, `fed.bin` makes feeding at-most-once across a RESET, a software stall monitor logs to `health.log` and resets the board, and a lost clock starts the next day at 08:00.
- **USB network fix** — the USB FIFO overlap behind hung `ask`s was removed (`ESP32S3_FIFO_SIZE` 1280 → 800, plus OTG and CDC-NCM fixes): three asks in a row took 9 / 4 / 5 s, and ping went 190/190.
- **Microphone fix** — audio buffers aligned to 64 bytes, which ended intermittent capture stalls: voice worked on 3 of 3 boots.

**Firmware:** [`nuttx.bin`](nuttx.bin) at the repository root is the finals build (1,565,456 B, md5 `0793ff78a1e4933e6c194d2b2b36d02c`), the firmware on the demo board. Flash it as in [§7](#7-build--run-real-hardware) step 5. The finals source is in this repository too: the app in [`app/velapaw/`](app/velapaw/), and the changes to the openVela trees as patches in [`board/patches/`](board/patches/README.md#3-finals-image) and [`agent/patches/`](agent/README.md); [§7](#7-build--run-real-hardware) builds it. The USB and microphone fixes have not been submitted upstream yet.

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
- 🤖 **On-device AI agent that speaks up first** — the openVela `ai_agent` framework runs on the board with a custom Skill that reads the *real* feeding records. On a timer it **pushes a warning by itself** when a pet crosses a concern threshold — unprompted, with nobody at the console. A quiet device means nothing is wrong. Since the preliminary round it can also change a pet's meal times from one typed sentence, and its replies and warnings appear on the touchscreen. See [§4](#4-the-on-device-ai-agent-the-one-part-that-needs-internet).
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
                                                   │
                                  status.md / feed_log.md on littlefs (/data)
                                                   │
                                     read by the on-device ai_agent — see §4
```

The gate in the middle is the whole idea: recognition alone would feed a greedy pet all day; a timer alone would feed whichever animal happened to be standing there. **Both must agree.**

**Software architecture** (clean HAL boundaries, in [`app/velapaw/`](app/velapaw/)):

- `ui/` — LVGL touchscreen UI (Enroll · Recognize · My Pets · Trends)
- `infer/` — inference interface + the TFLite-Micro backend + the trained models
- `identity/` — enrollment store, cosine matcher, per-pet flash persistence, pet photos
- `store/` — feeding history & behavior analytics; also writes the Markdown records the agent reads
- `voice/` — keyword spotting + the MEAL→HOUR→CONFIRM meal-scheduling dialog
- `hal/` — camera (OV5640) and feeder (28BYJ-48 stepper) hardware abstraction

**Agent layer** ([`agent/`](agent/)) — the custom Skill, the heartbeat task list, and the patches against `packages/ai_agent`. It sits *on top of* the finished feeder: it reads the records `store/` writes and never takes part in a feeding decision. In the finals build it can also move a meal time through its `set_meal_schedule` tool, just as an edit on the touchscreen would; the feeder still serves that meal only when the pet is recognized at the bowl. See [§4](#4-the-on-device-ai-agent-the-one-part-that-needs-internet).

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
| **Change a meal** (finals build) | `ask change bob's dinner to 18:20` — the agent calls its `set_meal_schedule` tool, which moves that meal and saves it to flash. Food still goes out only when the pet is recognized at the bowl and the meal is due. |
| **On the screen** (finals build) | Each `ask` and its reply appear as a toast under the bell; a heartbeat warning shows a red toast and turns the bell red, so the owner sees it without a console. |

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
                   USB console+network (esp32s3_composite.c), auto-start
                   (esp32s3_appinit.c), the board defconfig, and the patches
                   against nuttx/, apps/ and tflite-micro
                   (see board/patches/README.md)
host/            — off-device model training & export (TensorFlow/Keras)
hardware/        — 3D-printable feeder (OpenSCAD source + STLs + BOM)
docs/            — technical notes, benchmark reports, and the technical report
                   (preliminary and finals editions, English and 中文)
logs/            — AI-Coding session logs (this project was built with AI assistance)
nuttx.bin        — prebuilt finals firmware (flash at 0x0, see §7 step 5)
```

---

## 7. Build & run (real hardware)

You need an x86-64 Linux host (the team builds in an Ubuntu 26.04 VM) with
about 30 GB free. Step 1 creates the openVela workspace; run every later step
from its root, in the same shell.

```bash
# 0. One-time host setup: build tools, Git LFS (openVela keeps its prebuilt
#    toolchains in it), Google's repo tool, esptool in the system python3
#    (the build's image step looks for it there) and serial-port access.
#    Then log out and back in once, so the dialout group applies.
sudo apt-get install -y git git-lfs curl unzip build-essential gperf flex bison \
  gettext texinfo libncurses-dev pkg-config gawk automake libtool python3 \
  python3-pip python3-venv kconfig-frontends u-boot-tools genromfs xxd
git lfs install
curl -sSL https://storage.googleapis.com/git-repo-downloads/repo -o repo \
  && chmod +x repo && sudo mv repo /usr/local/bin/
python3 -m pip install --user --break-system-packages 'esptool>=4.8'
#    (Ubuntu 22.04 and older: leave out --break-system-packages)
sudo usermod -aG dialout $USER

# 1. Create the workspace and pull openVela + this team repo. Keep --git-lfs:
#    without it, repo leaves the LFS files (the cross-compiler among them) as
#    small text pointers. To repair a workspace synced without it:
#    git -C prebuilts/gcc/linux-x86_64/xtensa-esp32s3-elf lfs pull
mkdir openvela && cd openvela
repo init --git-lfs -u https://github.com/open-vela/contest2026_043_CircuitForge \
  -b dev-ai-contest-2026 -m contest2026_043_CircuitForge.xml
repo sync -c -j8

VP=$PWD/contest2026_043_CircuitForge
BOARD=nuttx/boards/xtensa/esp32s3/esp32s3-devkit

# 1b. Upstream has since rewritten the nuttx, apps and ai_agent branches, and
#     the step-3 patches don't fit the code now on them. Fetch the commits
#     the patches were made on (board/patches/README.md) by their full hash,
#     and check them out. tflite-micro can stay on its branch: the two files
#     its patch touches are unchanged there.
git -C nuttx             fetch -q openvela e02f581e235fc7b527d57ff62b668ce625d139ab
git -C apps              fetch -q openvela e81a73794786189f15e6c9fe9931ffddd561fd73
git -C packages/ai_agent fetch -q openvela 41723c61725c4e845bfee724f3ad2fafc416b6e1
git -C nuttx             checkout -q e02f581e235fc7b527d57ff62b668ce625d139ab
git -C apps              checkout -q e81a73794786189f15e6c9fe9931ffddd561fd73
git -C packages/ai_agent checkout -q 41723c61725c4e845bfee724f3ad2fafc416b6e1

# 2. Place the board files into the ESP32-S3 board tree.
#    The config directory is new — create it first, or the cp silently fails.
mkdir -p $BOARD/configs/waveshare_lcd
cp $VP/board/esp32s3_st7789.c    $BOARD/src/
cp $VP/board/esp32s3_composite.c $BOARD/src/
cp $VP/board/esp32s3_appinit.c   $BOARD/src/
cp $VP/board/esp32s3_bringup.c   $BOARD/src/
cp $VP/board/configs/waveshare_lcd/defconfig $BOARD/configs/waveshare_lcd/

# 3. Patch the upstream trees. Copying the .c files in is NOT enough — the two
#    new board sources have to be registered in CSRCS, and several upstream
#    defects have to be fixed, or the build does not link. Apply one patch at
#    a time, in this order: some build on an earlier one, so a single
#    `git apply --check` over all of them fails.
#    Details: board/patches/README.md §3 and agent/README.md
(cd nuttx && for p in nuttx-build-registration nuttx-touch-ft5x06 velapaw-audio-nuttx \
                      nuttx-i2s-finals nuttx-usb-ncm-tx nuttx-mic-lib-buffer nuttx-lcd-dc-pin; do
   git apply --check $VP/board/patches/$p.patch && git apply $VP/board/patches/$p.patch \
     || { echo "STOP at $p"; break; }; done)
(cd apps  && for p in apps-mbedtls-isystem velapaw-audio-apps apps-tflm-finals apps-lvgldemo-prototype; do
   git apply --check $VP/board/patches/$p.patch && git apply $VP/board/patches/$p.patch \
     || { echo "STOP at $p"; break; }; done)
(cd apps/mlearning/tflite-micro/tflite-micro \
   && git apply --check $VP/board/patches/tflite-micro-fminmax.patch \
   && git apply $VP/board/patches/tflite-micro-fminmax.patch)
(cd packages/ai_agent && for p in $VP/agent/patches/*.patch; do
   git apply --check "$p" && git apply "$p" || { echo "STOP at $p"; break; }; done)

# 3b. Optional: the ESP-NN kernels, ~1.3 s per recognition instead of ~6.75 s.
#     The finals nuttx.bin was built with them. Details: docs/esp-nn/README.md
git clone https://github.com/espressif/esp-nn apps/mlearning/esp-nn
git -C apps/mlearning/esp-nn checkout d45b843ca5f873ca9d0706ab0e7f14eafd132e98
mkdir -p apps/mlearning/esp-nn/shim
cp $VP/docs/esp-nn/shim/esp_timer.h apps/mlearning/esp-nn/shim/
K=apps/mlearning/tflite-micro/tflite-micro/tensorflow/lite/micro/kernels/esp_nn
mkdir -p $K && cp $VP/docs/esp-nn/kernels/*.cc $K/

# 4. Build. The cross-compiler comes with the workspace, but nothing puts it
#    on PATH.
export PATH=$PWD/prebuilts/gcc/linux-x86_64/xtensa-esp32s3-elf/bin:$PATH
xtensa-esp32s3-elf-gcc --version   # should report crosstool-NG esp-12.2.0
./build.sh esp32s3-devkit:waveshare_lcd -j8

# 4b. On a fresh workspace that build stops with "invalid initializer" in
#     clk_ctrl_os.c and modem_clock.c. It downloads esp-hal-3rdparty into
#     nuttx/arch/xtensa/src/esp32s3/ on the way, and the HAL version nuttx
#     pins predates NuttX's struct spinlock_t: it initialises its locks with
#     a plain 0. Fix the HAL, then build again. A distclean or a config
#     change downloads the HAL afresh, so redo 4b after either.
grep -rlE --exclude-dir=.git 'define[[:space:]]+LOCK_INITIALIZER_UNLOCKED[[:space:]]+\(?0\)?' nuttx/arch/xtensa \
  | xargs -r sed -i -E 's/(define[[:space:]]+LOCK_INITIALIZER_UNLOCKED[[:space:]]+)\(?0\)?/\1SP_UNLOCKED/'
./build.sh esp32s3-devkit:waveshare_lcd -j8

# 5. Flash firmware + the two on-device models. Put the board in download
#    mode first: hold BOOT, tap RESET, release BOOT. The port is then
#    usually /dev/ttyACM0. (esptool 4.x spells the command write_flash.)
#    $VP/nuttx.bin is this source prebuilt with ESP-NN (step 3b), the firmware
#    on the demo board; to flash it, use it in place of nuttx/nuttx.bin.
python3 -m esptool --chip esp32s3 --port <PORT> write-flash \
  0x0        nuttx/nuttx.bin \
  0x600000   $VP/app/velapaw/infer/model/velapaw.tflite \
  0x760000   $VP/app/velapaw/infer/model/bcs.tflite
#    Then tap RESET to boot the new firmware.
```

**6. Provision the agent's data files — once per board.** The custom Skill and
the heartbeat task list are runtime files on the board's littlefs, not build
inputs, so nothing above installs them: write `agent/skills/velapaw-feeding-digest.md`
to `/data/ai_agent/skills/` and `agent/HEARTBEAT.md` to `/data/ai_agent/`. They
survive reboots and reflashes, because `/data` starts at `0x180000` and step 5
writes only up to ~`0x17ec80`. The procedure and its two gotchas are in
[`agent/README.md` §1](agent/README.md). Skip this and the feeder still works —
only the agent's proactive push has nothing to read.

On boot the device runs the feeder UI directly. **Enroll a pet** (capture a few tight, well-lit face photos), then the **Recognize** tab identifies it and feeds on schedule.

> **Note on the audio patch:** `velapaw-audio-nuttx.patch` carries per-buffer
> `VP: mic` / `VP: RXDUMP` diagnostics alongside the fixes. They are noisy on the
> console; `nuttx-i2s-finals.patch`, applied after it in step 3, compiles them
> out. See the "Strip before shipping" section of
> [`board/patches/README.md`](board/patches/README.md).

> **Note on inference speed:** the ~1.3 s figure uses the ESP-NN accelerated kernels (step 3b, [`docs/esp-nn`](docs/esp-nn/README.md)); without them recognition takes ~6.75 s. See the [engineering deep-dive](docs/ENGINEERING.md) for the acceleration setup, the hardware-SPI display debug, and reproducibility notes.

**If a step fails:**

- **`cp: cannot stat …`, or `xtensa-esp32s3-elf-gcc: command not found`.**
  `VP`, `BOARD` and the `PATH` export last only for the shell they were typed
  in. In a new terminal, `cd` to the workspace root and run them again.
- **The `--version` check prints anything but a version** (typically
  `version: command not found`). The toolchain is still Git LFS pointers:
  run `git -C prebuilts/gcc/linux-x86_64/xtensa-esp32s3-elf lfs pull`.
- **Step 3 prints `STOP at …` or `patch does not apply`.** Most often step 3
  has already run in this workspace, and a patch can't apply twice. Otherwise
  step 1b was skipped, and the patches don't fit today's upstream branches.
  Either way, undo the patches, run step 1b if you skipped it, then steps 2
  and 3 again:

  ```bash
  for t in nuttx apps apps/mlearning/tflite-micro/tflite-micro packages/ai_agent; do git -C $t reset -q --hard; done
  rm -f apps/mlearning/tflite-micro/float_shim.h packages/ai_agent/src/tools/tool_meal.[ch]
  ```

- **`invalid initializer` again after step 4b.** The HAL was downloaded
  afresh: repeat step 4b.
- **Hundreds of undefined `ets_*` / `esp_rom_*` symbols at link time.** The
  build left empty `*.ld.tmp` linker scripts behind:
  `find nuttx -name '*.ld.tmp' -size 0 -delete`, then build again.
- **`Permission denied` on `/dev/ttyACM0`.** The `dialout` group from step 0
  applies only after you log out and back in.
- **Downloads crawl or time out (mainland China).** Get `repo` from
  `https://mirrors.tuna.tsinghua.edu.cn/git/git-repo` and run
  `export REPO_URL=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo` before
  step 1, add `-i https://pypi.tuna.tsinghua.edu.cn/simple` to the `pip`
  line, and let `repo sync` fetch openVela from its Gitee mirror. This team
  repo is on GitHub only, hence the third rule:

  ```bash
  git config --global url."https://gitee.com/open-vela/".insteadOf "https://github.com/open-vela/"
  git config --global url."https://gitee.com/openvela-toolchain-external/".insteadOf "https://github.com/openvela-toolchain-external/"
  git config --global url."https://github.com/open-vela/contest2026_043_CircuitForge".insteadOf "https://github.com/open-vela/contest2026_043_CircuitForge"
  ```

---

## 8. AI-assisted development

This project was built end-to-end with AI-assisted (AI Coding) development, and the full session logs are committed under [`logs/`](logs/) as required. AI was used throughout the lifecycle:

- **Requirements & design** — shaping the recognition-gated feeding model, the open-set enrollment approach, and the health-monitoring analytics.
- **Firmware & drivers** — bringing up the display, OV5640 camera (LCD_CAM), RTC, and the 28BYJ-48 stepper feeder on the ESP32-S3; wiring TensorFlow Lite Micro + ESP-NN onto the Xtensa LX7.
- **Hard debugging** — the standout example: a hardware-SPI display bug that survived ~60 blind build cycles was cracked with a **logic analyzer**, root-caused to the CS-tied-low panel losing bit-sync at the pad handover, and turned into a **display speedup** (bit-bang → hardware SPI + DMA).
- **Product iteration** — the full pet lifecycle (enroll, recognize, feed, edit, delete, photos), all tested on real hardware.

---

## 9. Status

**Finals update:** the six changes listed under [Finals update](#finals-update--whats-new-since-the-preliminary-round) are verified on the board, are in the prebuilt finals `nuttx.bin`, and their source is in this repository.

**On-device `ai_agent`:** running on hardware — the custom feeding-digest Skill, the `ask` CLI channel, and the proactive push, captured firing unattended. It is the only part of VelaPaw that needs an internet connection; everything in the feeder itself keeps working with the network down. See [§4](#4-the-on-device-ai-agent-the-one-part-that-needs-internet).

**Working on hardware today:** on-device recognition, body-condition scoring, recognition-gated 3-meals/day scheduling with a per-slot skip-meal toggle, portion & daily-limit control, feeding history & trends with appetite-drop alerts, hardware RTC, full pet lifecycle with persistent photos, auto-start, a hardware-SPI + DMA display, a bilingual EN/中文 UI with a live toggle, a spoken MEAL→HOUR→CONFIRM meal-scheduling dialog (hardware-proven end to end), the **28BYJ-48 stepper feeder** (turning on GPIO9/10/11/43), the **OV5640 relocated onto a 30 cm FFC** and confirmed working through the coupler + `cam_mast` mount, and the **3D-printed enclosure fully assembled** — stepper dispenser and relocated camera mounted into the printed structure.

---

## 10. Datasets

Datasets used in this project can be found at: https://drive.google.com/drive/folders/1-HnbS-VStbKuXCXWK-O1Nh2lNLdqMpbI

---

*Built by team CircuitForge for the 2026 openVela AI Hardware Developer Contest.*
