# Patches against the upstream trees (nuttx / apps)

These patch **upstream files outside this repo**, so a `repo sync` silently
reverts them and the affected path stops working with no error. They are kept
here because this repo is the only thing that survives a re-sync.

Always `git apply --check` first — a partial apply is worse than no apply.

Captured from the working build tree on branch `dev-ai-contest-2026`, at:

| Tree | Revision |
|---|---|
| `nuttx/` | `e02f581e235` |
| `apps/` | `e81a73794` |

(`packages/ai_agent` sits at `41723c6`; its patches are indexed in
[`agent/README.md`](../../agent/README.md).)

Record these because a `--check` failure has two very different causes. On the
revisions above it means the patch is broken. On any other revision it usually
just means the upstream file has moved under the patch — re-diff against the
tree you actually have instead of forcing it. The `nuttx-build-registration`,
`nuttx-touch-ft5x06` and `apps-mbedtls-isystem` patches touch stable enough
regions to apply well outside these revisions; the audio patches do not.

---

## 1. Required to build (step 2b of [README §7](../../README.md#7-build--run-real-hardware))

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
fix, which lives in a tree that is re-cloned *during* the build. That one still
needs `~/fix_vm_patches.sh`, and the ordering is **build → fix → build**, never
fix → build.

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

### The `nxlooper` changes

`velapaw-audio-apps.patch` is almost entirely `VP: L*`/`VP: M*` instrumentation
plus the silence-priming of TX before the record loop starts. It is a **bring-up
harness, not product code** — VelaPaw never runs `loopback`. Nothing here needs
to survive into the release build; keep it only for reproducing the audio tests.
