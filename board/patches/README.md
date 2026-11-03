# Patches against the upstream trees (nuttx / apps / tflite-micro)

These patch **upstream files outside this repo**, so a `repo sync` silently
reverts them and the affected path stops working with no error. They are kept
here because this repo is the only thing that survives a re-sync.

Always `git apply --check` first — a partial apply is worse than no apply.

Captured from the working build tree on branch `dev-ai-contest-2026`, at:

| Tree | Revision |
|---|---|
| `nuttx/` | `e02f581e235` |
| `apps/` | `e81a73794` |
| `apps/mlearning/tflite-micro/tflite-micro/` | `94f7cee178a` |

(`packages/ai_agent` sits at `41723c6`; its patches are indexed in
[`agent/README.md`](../../agent/README.md).)

Upstream has since rewritten the `dev-ai-contest-2026` branches of `nuttx`,
`apps` and `packages/ai_agent`, so a fresh `repo sync` no longer contains
these commits. Step 1b of [README §7](../../README.md#7-build--run-real-hardware)
fetches all three by full hash and checks them out. It leaves tflite-micro
on its branch: `94f7cee178a` is on openVela's Gitee mirror only (the team's
build tree synced from Gitee), but the two files its patch touches, `max.h`
and `min.h`, are byte-identical on the GitHub branch.

Record these because a `--check` failure has two very different causes. On the
revisions above it means the patch is broken. On any other revision it usually
just means the upstream file has moved under the patch — re-diff against the
tree you actually have instead of forcing it. The `nuttx-build-registration`,
`nuttx-touch-ft5x06` and `apps-mbedtls-isystem` patches touch stable enough
regions to apply well outside these revisions; the audio patches do not.

**For the finals firmware, apply the full series in [§3](#3-finals-image)**,
which starts with the §1 and §2 patches. §1 and §2 are as written for the
preliminary round. With the finals app and its voice features on, they no
longer link by themselves (see `nuttx-i2s-finals` in §3).

---

## 1. Required to build (step 3 of [README §7](../../README.md#7-build--run-real-hardware))

Without these three the firmware does not link, or links and comes up with a
touchscreen that reports rotated coordinates.

| File | Target | Files touched |
|---|---|---|
| `nuttx-build-registration.patch` | `nuttx/` | `boards/…/esp32s3-devkit/src/Make.defs`, `boards/xtensa/esp32s3/common/scripts/esp32s3_rom_aliases.ld`, `drivers/syslog/Make.defs` |
| `nuttx-touch-ft5x06.patch` | `nuttx/` | `drivers/input/ft5x06.c` |
| `apps-mbedtls-isystem.patch` | `apps/` | `crypto/mbedtls/Make.defs` |

```bash
cd ~/openvela/nuttx
for p in nuttx-build-registration nuttx-touch-ft5x06; do
  P=../contest2026_043_CircuitForge/board/patches/$p.patch
  git apply --check $P && git apply $P
done

cd ~/openvela/apps
P=../contest2026_043_CircuitForge/board/patches/apps-mbedtls-isystem.patch
git apply --check $P && git apply $P
```

### What each one fixes

- **`Make.defs`** — the board builds its sources from an explicit `CSRCS` list,
  and both `esp32s3_st7789.c` and `esp32s3_composite.c` are files this project
  adds. Without the two `ifeq` blocks neither is ever compiled, and the link
  fails on `board_lcd_initialize` (called from `esp32s3_bringup.c`) and
  `board_composite_connect`. Copying the `.c` files into `src/` is not enough.
- **`esp32s3_rom_aliases.ld`** — the script `PROVIDE`s
  `cache_invalidate_addr = Cache_Invalidate_Addr`, but nothing in the tree
  defines `Cache_Invalidate_Addr`; it is expected from the esp-hal ROM scripts
  that are re-cloned *during* the build. The patch pins it to its ROM address.
- **`drivers/syslog/Make.defs`** — `ifneq ($(CONFIG_RAMLOG_BUFFER_SECTION),"")`
  compares against a literal two-character `""` that the variable can never
  equal, so `-DRAMLOG_BUFFER_SECTION` was defined unconditionally. Matters here
  because the board runs `CONFIG_RAMLOG_SYSLOG=y`.
- **`drivers/input/ft5x06.c`** — the panel is mounted rotated relative to the
  controller's native frame, so raw `(x, y)` arrives transposed; the patch maps
  it and clamps to 480×320. It also answers `TSIOC_GETMAXPOINTS`, which
  upstream `lv_nuttx_touchscreen.c` calls and which stock ft5x06 rejects with
  `-ENOTTY`.
- **`crypto/mbedtls/Make.defs`** — switches the include flags to `-isystem` so
  the bundled `mbedtls/*.h` win over any same-named headers already on the
  search path. This is `packages/ai_agent/fix_esp32s3.sh`'s header-priority
  fix; without it the agent's TLS does not build.

---

## 2. Audio

Captured 2026-08-03 from the working tree, against a verified-good capture run.

| File | Target checkout | Files touched |
|---|---|---|
| `velapaw-audio-nuttx.patch` | `nuttx/` | `arch/xtensa/src/esp32s3/esp32s3_i2s.c`, `drivers/audio/es8311.c` |
| `velapaw-audio-apps.patch` | `apps/` | `system/nxlooper/nxlooper.c` |

```bash
cd ~/openvela/nuttx && git apply --check ../contest2026_043_CircuitForge/board/patches/velapaw-audio-nuttx.patch \
  && git apply ../contest2026_043_CircuitForge/board/patches/velapaw-audio-nuttx.patch
cd ~/openvela/apps  && git apply --check ../contest2026_043_CircuitForge/board/patches/velapaw-audio-apps.patch \
  && git apply ../contest2026_043_CircuitForge/board/patches/velapaw-audio-apps.patch
```

Note these do **not** cover the esp-hal `LOCK_INITIALIZER_UNLOCKED` → `SP_UNLOCKED`
fix, which lives in a tree that is re-cloned *during* the build. That one is
the `sed` in step 4b of [README §7](../../README.md#7-build--run-real-hardware),
and the ordering is **build → fix → build**, never fix → build.

### What's load-bearing in the audio patches

- **RX TDM slot map** (`i2s_configure`) — `i2s_rxchannels()` is a pure no-op
  upstream, so RX sat at `TOT_CHAN_NUM=0` (one slot per frame) while `RX_CONF1`
  described a two-slot frame. The receiver completed frames and discarded every
  slot: `nbytes=0` forever. This is the single fix that made capture work.
- **`I2S_RX_MONO`** (`I2S_RX_CONF_REG` bit 5) — `es8311_start` sets
  `GPIO_REG44=0x50`, putting the mic on the left slot and *the DAC's own output*
  on the right. With both RX slots enabled, playback fed straight back into the
  record stream — a digital feedback loop independent of acoustics.
- **`nbytes &= ~3`** in `i2s_receive` — re-aligns after the `DMA_BUFLEN_MAX`
  clamp, which can leave a non-multiple of the sample width.
- **`priv->streaming = true`** in `i2s_receive`.
- **Gain chain** in `es8311.c`: `SYSTEM_REG14=0x18` (24 dB analog PGA, was
  `0x1a` = 30 dB max) and `ADC_REG16=0x20` (0 dB digital, was `0x24` = 24 dB).
  Stock total was 54 dB, which railed the ADC on every buffer — the clipping
  *was* the "noise". Do not raise these.
- **`es8311_configure` returning `-ERANGE` on success** — upstream bug; `ret` is
  pre-loaded `-ERANGE` and the clearing expression never fires.
- **`#include <nuttx/mutex.h>`** in both files — missing upstream; without it
  `nxmutex_lock`/`unlock` become extern calls that can never link, because
  `CONFIG_LIBC_SEM_MUTEX_NOINLINE` is off and they are `static inline`.

### ⚠️ Strip before shipping

The nuttx patch also carries **diagnostics only**, which must come out of the
release build:

- `VP: mic` — the per-buffer meter in the RX worker (`peak`/`mean`/`dc`/`rail`).
- `VP: RXDUMP` — the one-shot register dump in `i2s_tx_worker`.

Both are `syslog(LOG_ERR, ...)` on every capture buffer and are noisy enough to
affect timing. They are thread context, so `syslog` is safe there — do **not**
move either into ISR context, which faults on this board (`EXCCAUSE=0x14`).

Matching cleanup in the defconfig: `CONFIG_DEBUG_AUDIO`, `_ERROR`, `_WARN`,
`CONFIG_DEBUG_FEATURES`, `CONFIG_SYSLOG_CONSOLE`.

**Neither probe is in the finals image.** `nuttx-i2s-finals.patch` (§3)
compiles them out (`#define VELAPAW_I2S_PROBES 0`), and the finals defconfig
leaves `CONFIG_DEBUG_AUDIO_ERROR` and `_WARN` off. That is also the fix for a
board running without a PC: kernel syslog blocked on the 193-byte CDC-ACM TX
buffer. `CONFIG_DEBUG_AUDIO`, `CONFIG_DEBUG_FEATURES` and
`CONFIG_SYSLOG_CONSOLE` are still on.

### The `nxlooper` changes

`velapaw-audio-apps.patch` is almost entirely `VP: L*`/`VP: M*` instrumentation
plus the silence-priming of TX before the record loop starts. It is a **bring-up
harness, not product code** — VelaPaw never runs `loopback`. Nothing here needs
to survive into the release build; keep it only for reproducing the audio tests.

---

## 3. Finals image

Captured 2026-11-03 from the build tree that produced the root
[`nuttx.bin`](../../nuttx.bin) (md5 `0793ff78a1e4933e6c194d2b2b36d02c`), at the
revisions above. The full series below, on top of the board files that
[README §7](../../README.md#7-build--run-real-hardware) step 2 copies in,
reproduces that tree's changes to `nuttx/`, `apps/` and `tflite-micro` byte
for byte, apart from the ones under [Not carried here](#not-carried-here).
The rest of that tree is in this repository as well:

- the app: [`app/velapaw/`](../../app/velapaw/) matches that tree's copy; only
  a Makefile comment and the app's README differ;
- the configuration:
  [`board/configs/waveshare_lcd/defconfig`](../configs/waveshare_lcd/defconfig)
  agrees with that tree's `.config` on all 198 of its entries;
- ESP-NN: see [ESP-NN](#esp-nn) below.

| File | Target checkout | Files touched |
|---|---|---|
| `nuttx-i2s-finals.patch` | `nuttx/` | `arch/xtensa/src/esp32s3/esp32s3_i2s.c` |
| `nuttx-usb-ncm-tx.patch` | `nuttx/` | `arch/xtensa/src/esp32s3/esp32s3_otg_device.c`, `drivers/usbdev/cdcncm.c` |
| `nuttx-mic-lib-buffer.patch` | `nuttx/` | `libs/libc/audio/lib_buffer.c` |
| `nuttx-lcd-dc-pin.patch` | `nuttx/` | `boards/xtensa/esp32s3/esp32s3-devkit/src/esp32s3-devkit.h` |
| `apps-tflm-finals.patch` | `apps/` | `mlearning/tflite-micro/Makefile`, `mlearning/tflite-micro/float_shim.h` (new) |
| `apps-lvgldemo-prototype.patch` | `apps/` | `examples/lvgldemo/lvgldemo.c` |
| `tflite-micro-fminmax.patch` | `apps/mlearning/tflite-micro/tflite-micro/` | `tensorflow/lite/kernels/internal/max.h`, `…/min.h` |

The full series, §1 and §2 included, on a freshly synced tree:

```bash
cd ~/openvela/nuttx
for p in nuttx-build-registration nuttx-touch-ft5x06 velapaw-audio-nuttx \
         nuttx-i2s-finals nuttx-usb-ncm-tx nuttx-mic-lib-buffer nuttx-lcd-dc-pin; do
  P=../contest2026_043_CircuitForge/board/patches/$p.patch
  git apply --check $P && git apply $P || { echo "STOP at $p"; break; }
done

cd ~/openvela/apps
for p in apps-mbedtls-isystem velapaw-audio-apps apps-tflm-finals apps-lvgldemo-prototype; do
  P=../contest2026_043_CircuitForge/board/patches/$p.patch
  git apply --check $P && git apply $P || { echo "STOP at $p"; break; }
done

cd ~/openvela/apps/mlearning/tflite-micro/tflite-micro
P=../../../../contest2026_043_CircuitForge/board/patches/tflite-micro-fminmax.patch
git apply --check $P && git apply $P
```

The loops check and apply one patch at a time on purpose: `nuttx-i2s-finals`
edits `esp32s3_i2s.c` after `velapaw-audio-nuttx` has, and a dry run of both at
once checks each against the unpatched file.

### What each one fixes

- **`nuttx-i2s-finals`**: the I²S driver as the finals image has it.
  - **Needed to link** with `CONFIG_VELAPAW_VOICE=y`, as in the finals
    defconfig. The voice diagnostics in
    [`app/velapaw/ui/ui_lvgl.c`](../../app/velapaw/ui/ui_lvgl.c) read the
    driver's RX counters (`g_velapaw_rx_*`, `velapaw_i2s_rxdbg()`,
    `velapaw_i2s_rxdesc()`) as plain externs, and only this patch defines them.
  - **The RX safety net (#35).** The resets in `i2s_rx_channel_start()` wipe
    the GDMA IN channel and the RX FIFO. A buffer that was in flight stayed on
    the active queue with no EOF to come, and RX never re-armed for the rest
    of the session. The patch moves it back to the pending queue and re-arms.
    `velapaw_i2s_rx_kick()` runs the same restart on demand;
    [`app/velapaw/voice/mic.c`](../../app/velapaw/voice/mic.c) calls it
    through a weak reference.
  - **The `VP:` probes are compiled out** (`#define VELAPAW_I2S_PROBES 0`),
    which is what `i2s-gate-probes.py` does. `i2s-rx-eof-walk.py` (#31) and
    `i2s-rx-eof-align.py` (#32) are not in the finals image. Don't run any
    `i2s-*.py` script on top of this patch.
- **`nuttx-usb-ncm-tx`**: the USB network wedge, where `ask` hung after
  `REQ written`.
  - `ESP32S3_FIFO_SIZE` 1280 → 800 in `esp32s3_otg_device.c`. The OTG core has
    256 words (1 KB) of FIFO RAM, of which ESP-IDF treats 200 as usable. At
    1280 bytes (320 words) the EP3/EP4 TX FIFOs sat past the end of it and
    wrapped onto the RX FIFO, so outgoing NCM data and received packets
    overwrote each other. At 800 bytes the whole layout fits in the 200 words.
  - `esp32s3_epin_abort()`, which drops a stuck IN transfer.
  - In `cdcncm.c`: double-buffered TX staging under a lock; a bounded 2 s wait
    that cancels a stalled transfer (`EP_CANCEL`) and recovers; a zero-length
    packet after an NTB that is a whole multiple of 64 bytes; real
    `GET/SET_NTB_INPUT_SIZE` answers; and checked submit return codes.
  - On the board: three `ask` turns in 9 s, 4 s and 5 s, and 190/190 host
    pings. The recovery path never fired, so it is untested. Which change was
    decisive was not isolated; the FIFO overlap is the leading cause.
  - Check the build: `grep -ac "otg: EP%d abort" nuttx/nuttx.bin` and
    `grep -ac "cdcncm: TX stalled" nuttx/nuttx.bin` must each print 1.
- **`nuttx-mic-lib-buffer`**: the microphone stall. The mic device has no
  buffer allocator of its own, so its buffers come from libc's `apb_alloc()`,
  which aligns to `MAX(up_get_dcache_linesize(), 1)`. Without
  `CONFIG_ARCH_DCACHE` that is 1, so a buffer gets malloc's 8-byte alignment.
  The GDMA cannot complete an RX descriptor whose PSRAM address is not on a
  16-byte boundary, so capture stalled. The buffers are allocated back to back
  with a stride that is a multiple of 16, so the first one decided the whole
  boot, which made it look like a hardware fault. The patch aligns to 64.
  Stale libc objects survive a rebuild, so from `~/openvela/nuttx`:

  ```bash
  find libs/libc -name lib_buffer.o -delete; rm -f libs/libc/libc.a staging/libc.a
  # after the build: must show a10, 64 (a stale object shows a10, 1)
  xtensa-esp32s3-elf-objdump -d --disassemble=apb_alloc nuttx | grep -B4 '<memalign>'
  ```

- **`nuttx-lcd-dc-pin`**: `GPIO_LCD_DC` 14 → 3 in `esp32s3-devkit.h`. The
  upstream board code drives `GPIO_LCD_DC`, and on this board GPIO14 is the
  I²S0 data-in pin from the audio codec (`CONFIG_ESP32S3_I2S0_DINPIN=14`). The
  panel's D/C line is GPIO3 (`PIN_DC` in
  [`board/esp32s3_st7789.c`](../esp32s3_st7789.c)).
- **`apps-tflm-finals`**: the TFLite-Micro build settings in
  `apps/mlearning/tflite-micro/Makefile`:
  - the `signal/` library sources, which the voice model's audio front end
    (Window, Rfft, …) needs;
  - `-fno-threadsafe-statics`. Without it `__cxa_guard_acquire` deadlocks on
    the first function-local static: the blue screen at recognition and the
    `kws: init` hang;
  - `-DTF_LITE_USE_GLOBAL_CMATH_FUNCTIONS`, `-fpermissive` and the new
    force-included `float_shim.h`, because NuttX's `<cmath>` has no `float`
    overloads ([`app/velapaw/Makefile`](../../app/velapaw/Makefile)
    force-includes the same header);
  - the ESP-NN block from [`docs/esp-nn`](../../docs/esp-nn/README.md). It
    only switches on when `apps/mlearning/esp-nn` exists; without that clone
    the build uses the reference kernels.

  It replaces `tflm-signal.py` and `tflm-signal-guards.py`, which the app's
  Kconfig help and Makefile comments still name. If either script has already
  run on your tree, restore the file first with
  `git -C ~/openvela/apps checkout mlearning/tflite-micro/Makefile`, or the
  patch won't apply.
- **`apps-lvgldemo-prototype`**: VelaPaw's first screen from the start of the
  project; `lvgldemo velapaw` draws a Feed Now button and a counter.
  `CONFIG_EXAMPLES_LVGLDEMO=y` builds it into both the preliminary and the
  finals `nuttx.bin`, but VelaPaw never runs it. It is here so the source
  matches the image; leaving it out changes nothing VelaPaw does.
- **`tflite-micro-fminmax`**: the generic `TfLiteMax`/`TfLiteMin` templates
  call `std::fmax`/`std::fmin`, which NuttX's `std::` lacks; the patch uses a
  plain comparison.

### ESP-NN

The finals image was built with ESP-NN. The esp-nn clone is not a repo
project, so its pin lives here: commit `d45b843ca5f`, set up as in
[`docs/esp-nn`](../../docs/esp-nn/README.md). That folder's kernel wrappers
and `esp_timer.h` shim are identical to the build tree's copies.

### Not carried here

The build tree had a few more changes. None of them is in the finals image:

- Wi-Fi changes in `esp32s3_wifi_adapter.c` and `esp32s3_wifi_utils.c`, and a
  heap print in `esp32s3_wireless.c`. The finals image networks over USB with
  `CONFIG_ESP32S3_WIFI` off, and the print sits inside
  `#ifdef CONFIG_ESP32S3_WIFI`; its string is not in `nuttx.bin`.
- `drivers/usbdev/rndis.c`: CDC-NCM replaced RNDIS, and `CONFIG_RNDIS` is off.
- Debug prints in `apps/nshlib/nsh_fileapps.c`; `CONFIG_NSH_FILE_APPS` is off.
- Workspace state that changes no firmware source: a local Kconfig path fix in
  `tests/`, submodule pointers in `external/freetype` and the Infineon AURIX
  library, the unused ARM toolchain removed from `prebuilts/`, a deleted
  `apps/builtin/registry/.gitignore`, and the contest's starter apps and
  board template.
