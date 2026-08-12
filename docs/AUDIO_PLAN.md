# VelaPaw — Voice Control Plan (ES8311 onboard mic)

Status: **largely working on hardware.** Written 2026-08-02, after the schematic
netlist review that proved the onboard mic is reachable without touching the
stepper — the plan below (phases 0–3) is now history, not a roadmap. The MEAL→HOUR→
CONFIRM voice-scheduling dialog has been proven end to end on hardware (builds
68/69/81, 2026-08-06/07); the console-blocking bug that required a PC on COM4 was
solved in build 83 (2026-08-07). One issue remains open — an intermittent I²S
capture stall (`-110`) that a RESET tap reproduces but a cold boot doesn't reliably
clear — and the build currently flashed on the board (`skipmeal`, built on `b82r`)
predates the b83 fix, so its mic fails every capture. See `docs/SESSION_STATE.md`
("Voice & audio — current status") for the up-to-date picture and build lineage;
treat everything below this point as the historical bring-up log that got it there.

## Why this is possible at all

The ES8311 codec's I²S bus is **GPIO 12/13/14/15/16**, and those nets are private —
each has exactly two nodes, the ESP32 and the codec. They are not on header J8, so
nothing external can reach them *or* conflict with them. The stepper lives on
9/10/11/43 (header pins 14/12/10/27). Audio and the feeder cannot collide
electrically. See `SESSION_STATE.md` for the full J8 map.

The mic is analog-differential into `MIC1P`/`MIC1N`; the bundled 6 Ω 1 W speaker runs
through an NS4150B amp whose enable is `PA_CTRL` = expander **EXIO7**. Codec control
shares the existing IO7/IO8 I²C bus. **No new parts, no new wiring.**

---

## PHASE 0 — Rollback net ✅ DONE (2026-08-02)

No git. Deliberate call: the repo has one scaffold commit and all real work untracked,
and rather than sweep months of work into a single opaque commit right before a risky
change, the rollback net is **plain file copies** — a golden binary in the VM plus a
source snapshot on Windows.

### Leg 1 — Golden binary ✅ (the one that actually saves you)

The firmware currently on the board, copied out of the VM to a path nothing touches,
md5summed and `chmod a-w`:

```
~/velapaw-golden/pre-audio/
  nuttx.bin            816,308 B   built 2026-07-24 10:51:26
  defconfig.golden      79,066 B   the VM's full nuttx/.config
  defconfig.board        2,894 B   board/configs/waveshare_lcd/defconfig
  esp32s3_st7789.c      42,037 B   ← AUTHORITATIVE stepper source
  esp32s3_appinit.c
  MANIFEST.md5
```

Verified as the exact image running on the board: the build is sequential and
self-consistent (`.o` 10:51:19 → `.built` 10:51:23 → `nuttx` ELF 10:51:25.722 →
`nuttx.bin` 10:51:26.022) and no app source is newer than it.

Rollback, ~60 seconds, no rebuild:

```bash
esptool --chip esp32s3 --port <PORT> write-flash \
  0x0      ~/velapaw-golden/pre-audio/nuttx.bin \
  0x600000 <repo>/app/velapaw/infer/model/velapaw.tflite \
  0x760000 <repo>/app/velapaw/infer/model/bcs.tflite
```

⚠️ **Remaining gap:** the two `.tflite` models are *not* in the golden directory — the
`cp` silently failed because the contest repo isn't checked out at that path in the VM.
They are safe on Windows and are in the snapshot, but the golden set is not yet
self-contained. To close it:

```bash
chmod u+w ~/velapaw-golden/pre-audio
cp <repo>/app/velapaw/infer/model/{velapaw,bcs}.tflite ~/velapaw-golden/pre-audio/
cd ~/velapaw-golden/pre-audio && md5sum * > MANIFEST.md5 && chmod -R a-w .
```

### Leg 2 — Source snapshot ✅ (Windows)

`D:\openVela\velapaw-snapshots\2026-08-02-pre-audio\` — 215 files, 20.9 MB, the repo
minus `.git/`, `host/.venv/`, `host/data/`, `host/export*/`, `logs/`, and build
artifacts. See its `RESTORE.md`. Restore = copy directories back over the working tree.

### Leg 3 — Repo reconciled to the golden copy ✅

The Windows repo had drifted from what actually runs. Both files are now corrected:

| File | Was | Now |
|---|---|---|
| `board/esp32s3_st7789.c` | half-step `k_halfstep[8]`, `4096`/`1500 µs`/`4 g`, **bit3=IN1** | full-step `k_fullstep[4] = {0x3,0x6,0xC,0x9}`, `2048`/`3000 µs`/`5 g`, **bit0=IN1** |
| `board/configs/waveshare_lcd/defconfig` | 2,868 B | 2,894 B — added the missing `CONFIG_VELAPAW_FONT_CJK=y` |

The bit-order flip is the one that mattered: the repo's table would have driven the
rotor **backwards**, so food would never have reached the outlet.

**Rule going forward:** the VM board tree is authoritative for `board/`. Anything
edited there gets copied back to `D:` in the same session, or the drift returns.

---

## PHASE 1 — Prove the pipe (go/no-go gate)

**Goal:** speak, hear yourself. Nothing else. No model, no recognition.

### defconfig delta

Verified against `nuttx/arch/xtensa/src/esp32s3/Kconfig`:

```
CONFIG_ESP32S3_I2S=y
CONFIG_ESP32S3_I2S0=y
CONFIG_ESP32S3_I2S0_ROLE_MASTER=y      # required for MCLK
CONFIG_ESP32S3_I2S0_RX=y               # default y
CONFIG_ESP32S3_I2S0_TX=y               # default y
CONFIG_ESP32S3_I2S0_MCLK=y             # default n — MUST enable, codec is dead without it
CONFIG_ESP32S3_I2S0_MCLKPIN=12
CONFIG_ESP32S3_I2S0_BCLKPIN=13         # default 4  ← would collide with LCD CS
CONFIG_ESP32S3_I2S0_DINPIN=14          # default 19 — ASDOUT, codec → ESP (capture)
CONFIG_ESP32S3_I2S0_WSPIN=15           # default 5  ← would collide with LCD SCLK
CONFIG_ESP32S3_I2S0_DOUTPIN=16         # default 18 — DSDIN, ESP → codec (playback)
CONFIG_ESP32S3_I2S0_DATA_BIT_WIDTH_16BIT=y
CONFIG_ESP32S3_I2S0_SAMPLE_RATE=16000  # default 44100; voice only needs 16k

CONFIG_AUDIO=y
CONFIG_DRIVERS_AUDIO=y
CONFIG_AUDIO_ES8311=y
CONFIG_AUDIO_FORMAT_PCM=y
CONFIG_SYSTEM_NXLOOPER=y
```

🔴 **The four pin defaults are the dangerous part.** `BCLKPIN` defaults to 4 (LCD chip
select) and `WSPIN` to 5 (LCD SCLK). Enabling I²S without overriding them will break
the display. Set all five pins explicitly in the same edit that enables I²S.

### Board glue

`nuttx/boards/xtensa/esp32s3/common/src/esp32s3_es8311.c` already exists upstream,
gated on `CONFIG_ESP32S3_I2S && CONFIG_AUDIO_ES8311`. Call
`esp32s3_es8311_initialize()` from board init. No driver to write.

### Test

```
nsh> nxlooper
nxlooper> loopback
```

### Diagnosis table

| Symptom | Cause |
|---|---|
| Your voice, clean | Pipe works. Gate passed. |
| Console dies / no boot | Config landmine — same class as `CONFIG_RTC`. Revert, bisect. |
| Display breaks | I²S pins left at defaults (4/5). Fix `BCLKPIN`/`WSPIN`. |
| Nothing at all | Codec never initialised — I²C address, or MCLK not enabled |
| Silence, no errors | Mic gain at 0 — `ES8311_ADC_REG16` |
| Chipmunk / slow-motion | Sample-rate mismatch codec vs I²S |
| White noise / roar | Wrong bit depth or byte order |
| Choppy | DMA buffers too small |
| Playback OK, capture dead | Codec in DAC-only mode — `audio_mode` not `ES_MODULE_ADC` |

**None of these throw an error.** Your ear is the instrument.

### Order of operations (do not batch)

**Correction, 2026-08-02:** an earlier version of this plan said step 1 was "I²S and
pins only". **That will not link.** `CONFIG_ESP32S3_I2S=y` unconditionally compiles
`boards/xtensa/esp32s3/common/src/esp32s3_board_i2s.c`, and the devkit bringup
unconditionally calls its `board_i2sdev_initialize()`, which calls
`audio_i2s_initialize()`, `audio_register()` and `pcm_decode_initialize()`. Without the
audio framework those are undefined symbols. I²S and the audio core are one step,
whether you want them to be or not.

1. ✅ **DONE 2026-08-02.** **I²S + pins + audio core**, no codec. Boot. **Confirm console
   AND display still work.** This is the step that can break the LCD (pin defaults) and
   the console. Result below.
2. ✅ **DONE 2026-08-02.** **Add `CONFIG_AUDIO_ES8311` + the init call.** Boot. Confirm
   console survives. This is the step that can hang on I²C. Result below.
3. **Assert `PA_CTRL`, then `nxlooper` → `loopback`.** Test with your ears. The speaker
   amp enable is **EXIO7 on the TCA9554**, not a GPIO — `esp32s3_st7789.c` already holds
   the expander handle (`g_ioe`), so this is `IOEXP_SETDIRECTION(g_ioe, 7, OUT)` +
   `IOEXP_WRITEPIN(g_ioe, 7, true)`. Plug the speaker in for this step.

The split that matters is *codec vs no codec* — the codec driver probes I²C at init and
is the part that can hang. Step 1 only brings up a DMA peripheral on five pins that go
nowhere else.

### What step 1 actually needs

```
CONFIG_AUDIO=y
CONFIG_AUDIO_I2S=y            # audio_i2s_initialize()
CONFIG_DRIVERS_AUDIO=y        # note: "menuconfig DRIVERS_AUDIO", drivers/audio/Kconfig
CONFIG_SYSTEM_NXLOOPER=y      # also short-circuits the pcm_decode path in board_i2s.c
```

`CONFIG_AUDIO_FORMAT_PCM` defaults to `y` under `AUDIO`, so it comes free and
`savedefconfig` will strip it. Same for `I2S0_RX`/`I2S0_TX` (default y),
`I2S0_ROLE_MASTER` and `I2S0_DATA_BIT_WIDTH_16BIT` (both choice defaults). Don't be
surprised when they're absent from the file.

`ESP32S3_I2S0` also `select`s `ESP32S3_DMA`, `ESP32S3_GPIO_IRQ` and **`SCHED_HPWORK`** —
a new kernel worker thread with its own stack, on a board where internal RAM is
`CONFIG_RAM_SIZE=114688`. Watch for heap pressure at boot.

### Step 1 RESULT ✅ DONE (2026-08-02) — flashed and verified on hardware

`nuttx.bin` **817,700 B**, only **+1,392 B** over the golden 816,308. That looked wrong
(an I²S driver plus the audio core for 1.4 KB?) so it was checked before spending a
flash cycle — `xtensa-esp32s3-elf-nm nuttx` shows `board_i2sdev_initialize` (420d1e1c),
`audio_i2s_initialize`, `audio_register`, `esp32s3_i2sbus_initialize`, `i2s_send`,
`i2s_receive` all present. The small delta is just `--gc-sections`. **Check the symbol
table, not the binary size.**

| Check | Result |
|---|---|
| Console / CDCACM enumerates after reset | ✅ COM4 (`VID_0525&PID_A4A7`) returns |
| Display renders + touch responds | ✅ "UI is up and very interactive" |
| Recognize still works (heap after `SCHED_HPWORK`) | ✅ unchanged |
| `/dev` audio node present | ✅ `/dev/audio/` → **`pcm0`** (TX) + **`pcm_in0`** (RX) |

`savedefconfig` kept **BCLK=13 / WS=15**; the dangerous Kconfig defaults (BCLK=4 = LCD
CS, WS=5 = LCD SCLK) did not creep back. It also reorders `CONFIG_ESP32S3_I2S=y` to
*after* the `I2S0_*` block — the Windows repo copy was re-sorted to match, byte-identical.

### Step 2 RESULT ✅ DONE (2026-08-02) — flashed and verified on hardware

`nuttx.bin` **884,844 B**, **+67,144 B** over step 1 — a real jump this time, because
`es8311.o` is referenced from bringup and `--gc-sections` can't drop it.

**Code: a new tracked board file, `board/esp32s3_bringup.c`** (4th `cp` line in the
README's deploy block). The devkit bringup has no ES8311 branch, so one was added
mirroring `esp32s3-korvo-2`:

```c
#elif defined(CONFIG_AUDIO_ES8311)
  ret = esp32s3_es8311_initialize(ESP32S3_I2C0, 0x18, 100000, ESP32S3_I2S0);
```

🔴 **It must be `#elif`, never an added call.** `board_i2sdev_initialize()` and
`esp32s3_es8311_initialize()` both register `pcm0` *and* `pcm_in0`; whichever runs
second dies with `-EEXIST`. The generic path is gated only on `CONFIG_AUDIO_CS4344`
upstream, so `CONFIG_AUDIO_ES8311` had to be added to that guard — **and to the
`i2s_enable_tx`/`i2s_enable_rx` declaration guard higher up**, which is the easy half to
miss (korvo-2 does the same at its line 147).

`esp32s3_es8311_initialize()` lives in `boards/xtensa/esp32s3/common/src/`, not korvo-2's
private dir, so it compiles for the devkit with **nothing ported**. `PA_CTRL` is not
touched here — it is EXIO7 on the expander and only matters for playback (step 3).

| Check | Result |
|---|---|
| Console / CDCACM enumerates after reset | ✅ COM4 returns; the I²C probe did **not** hang |
| Codec ACKs on the shared bus | ✅ `[velapaw] ES8311 codec ready on I2C0@0x18 / I2S0` |
| `/dev/audio/` nodes | ✅ `pcm0` + `pcm_in0` |
| Display renders + touch responds | ✅ UI up; `[wslcd] FT6336 id(0xA3)=0x64 ret=0` |
| RTC / camera / stepper unaffected | ✅ `RTC ok`, `OV5640 id=0x5640`, `stepper ready on GPIO 9/10/11/43` |

Two things worth keeping:

- **The node names are the proof.** With the `#elif` taken, `board_i2sdev_initialize()`
  is compiled out entirely — nothing else can register those nodes, so `pcm0`/`pcm_in0`
  existing *is* confirmation that the codec init returned OK.
- **The codec grabs I2C0 first**, ahead of `[wslcd] init` and its expander/touch probes,
  and they still came up clean. That was the whole risk in this step.

### ⚠️ VM-ONLY PATCHES — re-apply after any `repo sync`

None of these live in `contest2026_043_CircuitForge/`, so nothing tracks them.
**`~/fix_vm_patches.sh` applies all six and is idempotent.** (It replaces
`~/fix_hal.sh`, which patched the wrong file: its `grep -rl … | head -1` picked
`modem_clock.c` while the actual failure was in `clk_ctrl_os.c`. The replacement loops
over *every* match.)

🔴 **Ordering: for a config-changing build it is build → fix → build, not fix → build.**
The esp-hal re-clone happens *during* the build, so patching first achieves nothing —
the build replaces the tree, then compiles the unpatched file. A first build failing on
`clk_ctrl_os.c:39 … invalid initializer` is expected; run the script and build again.
Confusingly the script's own output looks correct either way, because it reports on the
tree as it stands *before* the re-clone.

1. **`nuttx/arch/xtensa/src/esp32s3/esp32s3_i2s.c` — add `#include <nuttx/mutex.h>`.**
   Without it: `implicit declaration of nxmutex_lock` at compile, then `undefined
   reference to nxmutex_lock/nxmutex_unlock` at link. `CONFIG_LIBC_SEM_MUTEX_NOINLINE`
   is **off**, so those are `static inline` bodies in the header — there is no exported
   symbol to link against, and an implicit declaration turns them into extern calls.
   Adding the include is the whole fix.
2. **`nuttx/drivers/audio/es8311.c` — the same missing `#include <nuttx/mutex.h>`**
   (`nxmutex_lock` 1318, `nxmutex_unlock` 1383, `nxmutex_init` 2255). Anchor the insert
   on `#include <nuttx/audio/es8311.h>`. This one is an **upstream bug in this
   revision**, not VM/Windows drift — the Windows tree lacks it too.
3. **esp-hal `LOCK_INITIALIZER_UNLOCKED` → `SP_UNLOCKED`** — reverted by the re-clone that
   any Kconfig change triggers. `spinlock_t` is a struct here, so scalar `0` is an
   invalid initializer. Only rewrite the `= 0` form; leave `portMUX_INITIALIZER_UNLOCKED`
   alone.
4. **`nuttx/drivers/audio/es8311.c` — `es8311_configure()` swallows success as `-ERANGE`.**
   Symptom: `loopback` fails with `Error loopback test: 34`. `ret` is pre-loaded with
   `-ERANGE` (lines 1017 / 1064) and the only thing that clears it is
   `ret = es8311_setsamplerate(priv) == -ENOTTY ? OK : ret;` — so a **successful** call
   returning 0 leaves the error in place, because `0 == -ENOTTY` is false. Same bug on
   `es8311_setbitspersample`, in both the INPUT and OUTPUT branches. Upstream bug.
   The sed rewrites all four to `ret = es8311_setX(priv); if (ret == -ENOTTY) ret = OK;`.
   Leave the two bare calls in `es8311_reset()` (≈2191) alone — that path ignores the
   return deliberately.

   ⚠️ Do **not** read the boot-time `MCLK = 4096000 and samplerate = 48000 not supported`
   as the cause. That block is the driver's own init at its 48 kHz default, and
   `{4096000, 16000}` **is** present in `es8311_coeff_div[]` (`es8311.h:543`) — one of ten
   16 kHz entries. At our settings `getcoeff()` succeeds; the fault was purely the ternary.
5. **`apps/system/nxlooper/nxlooper.c:1150` — loop-thread priority 246 → 120.**
   Applied while chasing a console hang blamed on SCHED_FIFO starvation. **It did not fix
   anything** — the hang was `CONFIG_DEBUG_AUDIO_INFO` logging in the per-buffer path.
   Harmless, but revert it if you ever want a clean diff against upstream.
6. **`apps/system/nxlooper/nxlooper.c:476` — start the play device before the record
   device.** The real capture fix; see "ROOT CAUSE" below. Without it RX waits for a
   BCLK that only the TX module can generate, and `loopback` hangs forever.

## Step 3 IN PROGRESS — PA_CTRL + the `nxlooper` ear test

**`PA_CTRL` is done.** The NS4150B enable is **TCA9554 EXIO7, not a GPIO**, so the ES8311
driver cannot reach it. `board_speaker_enable(bool)` was added to `esp32s3_st7789.c`
(next to the existing `g_ioe` handle) and is asserted at boot under
`#ifdef CONFIG_AUDIO_ES8311`. Confirmed on hardware:
`[velapaw] speaker amp enabled (PA_CTRL = EXIO7)`. It is exported rather than inlined so
the app can gate the amp per-playback later — leaving it on idles the amp and can hiss.

**🔴 `nxlooper` cannot allocate buffers unless `CONFIG_AUDIO_DRIVER_SPECIFIC_BUFFERS=y`.**
Symptom: `loopback` returns to the prompt with no error, nothing is audible, and `dmesg`
shows `nxlooper_loopthread: ERROR: Could not allocate buffer 0` — the loop thread tears
down immediately, so **nothing ever reaches the codec** and neither the amp nor the mic
is implicated. The chain:

1. nxlooper issues `AUDIOIOC_GETBUFFERINFO`; es8311 answers `Unhandled ioctl: 4106`.
2. `audio.c:1267` sets `upper->nbuffers` **only** when the lower half answers that ioctl
   successfully, so it stays 0 from the zalloc.
3. `audio_allocbuffer()` (`audio.c:786`) opens with
   `if (upper->periods >= upper->nbuffers) return 0;` → `0 >= 0` → returns 0.
4. nxlooper requires `ret == sizeof(buf_desc)`, sees 0, and errors out.

It is **not** memory pressure — the allocator refuses before it ever tries. es8311
already implements the handler at `es8311.c:1806`; the whole `case` is wrapped in
`#ifdef CONFIG_AUDIO_DRIVER_SPECIFIC_BUFFERS`. Upstream Kconfig miss: `AUDIO_ES8388`
carries `select AUDIO_DRIVER_SPECIFIC_BUFFERS`, `AUDIO_ES8311` does not, even though
`ES8311_BUFFER_SIZE`/`ES8311_NUM_BUFFERS` are defined unconditionally. **Fix is one
defconfig line, no source patch.** Gives 4 × 8 KB per direction = 64 KB, and at 16 kHz
mono ≈256 ms per buffer, so the ear test will echo about a second late. That is expected.

**Debug-only defconfig entries to back out before shipping:** `CONFIG_DEBUG_AUDIO`,
`CONFIG_DEBUG_AUDIO_ERROR`, `CONFIG_DEBUG_AUDIO_INFO`, `CONFIG_DEBUG_AUDIO_WARN`,
`CONFIG_DEBUG_FEATURES`, `CONFIG_RAMLOG_BUFSIZE=16384`.

### Reading the RAMLOG

**`dmesg` does not drain the buffer, and a full buffer drops new messages.** The 1024 B
default filled during boot with `DEBUG_AUDIO_INFO` on, so every post-boot message was
discarded and three consecutive `dmesg` runs returned byte-identical boot text — which
reads exactly like "nothing is happening" when in fact nothing was being *recorded*.
`CONFIG_RAMLOG_BUFSIZE=16384` fixes it; RAMLOG lives in `.bss` so `nuttx.bin` is unchanged.

NSH has **no pipe** and its `dmesg` takes **no arguments** — `dmesg | grep es8311` gives
"too many arguments". `dmesg` is also not an `nxlooper` command; type it at `nsh>` only.
Prefer `q` over `stop` to end a loopback: `quit` stops the loop itself, so it is one
blocking call rather than two.

### Console: use miniterm, not a script

`py -m serial.tools.miniterm COM4 115200` (Windows, own terminal; quit Ctrl-]) gives a
working `nsh>` immediately. Driving COM4 from a `System.IO.Ports.SerialPort` script gets
**character echo but never a prompt or command output**, on CR/LF/CRLF alike — NuttX
echoes in the driver receive path with or without a reader, so echo is not evidence the
shell is listening. Don't repeat that diagnosis; just use miniterm.

### 🔴 ROOT CAUSE — capture never completes because `nxlooper` starts RX before TX

Symptom: `loopback 1 16 16000` allocates and configures cleanly, one thump comes out of
the speaker, then **nothing**, and `q` hangs. `nxlooper_stop()` joins the loop thread,
which is parked in `mq_receive` (`nxlooper.c:508`) waiting for a buffer that never
returns — so the record side never delivered a single buffer.

It is not MCLK, and it is not a pin swap:

- `arch/xtensa/src/esp32s3/Kconfig` — `choice "I2S0 role" default ESP32S3_I2S0_ROLE_MASTER`,
  and `ESP32S3_I2S0_MCLK` **depends on** `ESP32S3_I2S0_ROLE_MASTER`. Our defconfig has
  `CONFIG_ESP32S3_I2S0_MCLK=y`, so master role is necessarily active and MCLK is driven
  on GPIO12. The "slave role never configures MCLK" hypothesis is dead.
- `velapaw-hardware.md` — IO14 is the codec's ASDOUT (codec → ESP, our `DINPIN=14`) and
  IO16 is DSDIN (ESP → codec, our `DOUTPIN=16`). The defconfig matches the schematic.

The actual chain:

1. `esp32s3_es8311.c:142/188` creates **two** es8311 instances (`/dev/audio/pcm0` out,
   `/dev/audio/pcm_in0` in) that **share one `esp32s3_i2sbus_initialize(0)` instance** —
   one `priv`, one `priv->rate`, one `priv->data_width`, one clock.
2. Because that instance has both `tx_en` and `rx_en`, `esp32s3_i2s.c:1503-1506` sets
   **`I2S_SIG_LOOPBACK`** in `I2S_TX_CONF_REG`: in full duplex the **TX module is the
   clock master and RX shares its BCLK/WS**.
3. `i2s_rx_channel_start()` (`esp32s3_i2s.c:2145-2194`) only resets DMA/RX/FIFO, sets
   `I2S_RX_UPDATE` and enables the IRQ — it **never sources a bit clock of its own**.
   With TX idle there is no BCLK/WS, so RX shifts in nothing and no DMA EOF ever fires.
4. `nxlooper_loopthread` starts the **record** device first (`nxlooper.c:476-489`) and
   issues `AUDIOIOC_START` on the **play** device only inside the `AUDIO_MSG_DEQUEUE`
   handler (`nxlooper.c:576-588`) — i.e. after the first record buffer comes back.

RX waits on TX; TX waits on RX. **Deadlock by construction.** The single thump is the
play node's configure sequence (`i2s_txdatawidth`/`i2s_txsamplerate` each stop/start the
TX channel) clicking the DAC once — proving the output path, amp and speaker are fine.

**Attempted fix (VM patch #6):** start the play device up front, before the record start,
and set `loopstate = NXLOOPER_STATE_LOOPING` so the deferred start at 576 is skipped.

⚠️ **Patch #6 alone is NOT sufficient — the last assumption in it is wrong.** Instrumented
run (`VP:` probes) printed `play started ret=0` and `record started, entering loop`, then
**zero messages**. `AUDIOIOC_START` on the play node does not start the I²S TX *hardware*:
`es8311_start()` only writes codec registers and spawns the worker thread, and
`es8311_processbegin()` finds `pendq` empty so it issues no `I2S_SEND`.

`I2S_TX_START` is set in exactly one place — `esp32s3_i2s.c:709`, inside the DMA setup
that pops a buffer off `priv->tx.pend`. **Submitting a buffer is what starts the clock.**
`I2S_RX_START` is likewise set only at `:771`. So RX is armed and started but, with
`I2S_SIG_LOOPBACK` sourcing its BCLK/WS from an idle TX module, it never shifts a bit —
no EOF, no DMA interrupt, no `AUDIO_MSG_DEQUEUE`, `mq_receive` blocks forever.

**Fix (VM patch #7):** before the play `AUDIOIOC_START`, take one buffer off `playdq`,
zero it, and `nxlooper_enqueueplaybuffer()` it. Enqueueing before start is the driver's
own documented "priming the pump" path (`es8311.c:1732-1736` — with `priv->mq` still
NULL the buffer just lands on `pendq`). The subsequent start then drains it through
`I2S_SEND` → `i2s_txdma_setup` → `I2S_TX_START`, and BCLK/WS finally run.

### 🔴 SECOND BUG — `CONFIG_I2S_DMADESC_NUM=2` is two bytes too small for the ES8311

With patch #7 in, the prime finally reached the I²S layer and failed loudly:
`es8311_processbegin: I2S transfer failed: -27` (`-EFBIG`).

`i2s_send()` rejects any transfer larger than `ESP32S3_DMA_BUFLEN_MAX * I2S_DMADESC_NUM`
(`esp32s3_i2s.c:2772`):

| symbol | value | where |
|---|---|---|
| `ESP32S3_DMA_BUFLEN_MAX` | `0x1000 - 1` = **4095** | `esp32s3_dma.h:59,67` |
| `CONFIG_I2S_DMADESC_NUM` | default **2** | `arch/xtensa/src/esp32s3/Kconfig:576` |
| ceiling | 4095 × 2 = **8190** | |
| `CONFIG_ES8311_BUFFER_SIZE` | **8192** | `include/nuttx/audio/es8311.h` |

**Over by 2 bytes.** RX never hit it because `i2s_receive()` silently clamps with
`MIN(nbytes, ESP32S3_DMA_BUFLEN_MAX)` (`:2880`); only the TX path errors out. This is
**not** specific to the priming buffer — `nxlooper.c:570` sets `apb->nbytes =
apb->nmaxbytes` on every play buffer, so playback would have failed the same way. The
es8311 driver's preferred buffer size and the ESP32-S3 I²S DMA defaults are simply
incompatible as shipped.

**Fix:** `CONFIG_I2S_DMADESC_NUM=4` in the defconfig → ceiling 16380. Costs one extra
`esp32s3_dmadesc_s` array per buffer container; negligible RAM. No source patch.

⚠️ This is a **config** change, so it triggers the esp-hal re-clone — follow the
**build → fix → build** rule above (first build may fail on `clk_ctrl_os.c`; run
`~/fix_vm_patches.sh`, then build again).

### RESULT of the DMADESC=4 build (2026-08-03) — TX clock CONFIRMED, RX still silent

Flashed `nuttx-dmadesc.bin` (887,520 B, md5 `d2fe3e3307e39d0265a3af557cd2044d`).
Console:

```
nxlooper_loopthread: VP: primed TX with silence ret=0
nxlooper_loopthread: VP: play started ret=0
nxlooper_loopthread: VP: record started, entering loop
nxlooper_loopthread: VP: msg size=8 id=1
nxlooper_loopthread: VP: deq apb=0x3c4c5d70
nxlooper_loopthread: VP: deq flags=24 nbytes=0 nmax=8192
```

**What this proves (both root causes above were real and are fixed):**

* No `-27`. `CONFIG_I2S_DMADESC_NUM=4` lifted the ceiling to 16380 and the 8192-byte
  buffer went through — the second bug is closed.
* `flags=0x24` = `AUDIO_APB_PLAY (1<<5)` | `AUDIO_APB_DEQUEUED (1<<2)`
  (`nxlooper.c:57-58`, `audio.h:529`). That is the **primed silence buffer coming
  back**. A TX DMA transfer ran start to finish, which means `i2s_txdma_start()`
  reached `modifyreg32(I2S_TX_CONF_REG, 0, I2S_TX_START)` at `esp32s3_i2s.c:709` and
  **BCLK/WS actually ran**. The first root cause is closed too.
* The clock does **not** stop afterwards: `I2S_TX_START` is only ever cleared in
  `i2s_tx_channel_stop()` (`:2225`), which nothing calls here. BCLK free-runs.

**What is still broken:** the record node never returns a single buffer. Exactly one
message arrived — the TX one — and then nothing. `nxlooper`'s dequeue handler only
copies/re-feeds when `dq_count(&playdq) != 0 && dq_count(&recorddq) != 0`
(`nxlooper.c:543`), so with `recorddq` permanently empty the loop is idle by
construction, not stuck.

So RX has a free-running bit clock and still never reaches a `suc_eof`. It should
fire after `MIN(8192, 4095)` = **4095 bytes ≈ 128 ms** (`i2s_rxdma_start`,
`:763-766`) — well inside the 256 ms the primed TX buffer covered.

**Ruled out at source level (do not re-investigate):**

* TX/RX IRQs are *not* aliased — `ESP32S3_PERIPH_DMA_OUT_CH0 + ch` and
  `ESP32S3_PERIPH_DMA_IN_CH0 + ch` are distinct sources, each with its own
  `esp32s3_setup_irq` + `irq_attach` (`:3075-3119`).
* The RX submit path is complete and correct: `i2s_receive` → `esp32s3_dma_setup` →
  `sq_addlast(rx.pend)` → `i2s_rxdma_start` → in-link load, `DMA_IN` enable,
  `I2S_RX_START` (`:940-978`, `:736-776`).
* `i2s_rx_schedule` moves `rx.act` → `rx.done` and re-arms (`:1100-1160`).

**The one unknown left is whether `I2S_RECEIVE` is ever called at all** — i.e. whether
the record node's `es8311_processbegin` takes the ADC branch. There is no probe on the
record side; `auderr("I2S transfer failed")` only prints on a *failed* call, so
"never called" and "called and returned OK" are indistinguishable from this output.
Next diagnostic (cheap, one build): `auderr` in `es8311_processbegin` printing
`priv->audio_mode` and the branch taken, plus one at the top of `i2s_receive`.

### 🔴 FOURTH BUG — **THE RX STALL**: `I2S_RX_EOF_NUM` is set to an odd byte count

Probe build `nuttx-probe2.bin` (887,592 B, md5 `da85e6f62103661511cb8cf2d63ec267`) settled it:

```
VP: primed TX with silence ret=0
VP: pb mode=2 n=8192          <- play node, ES_MODULE_DAC  (I2S_SEND)
VP: play started ret=0
VP: pb mode=1 n=8192          <- record node, ES_MODULE_ADC (I2S_RECEIVE)
VP: i2s_receive rx_en=1
VP: pb mode=1 n=8192
VP: i2s_receive rx_en=1
VP: record started, entering loop
VP: msg size=8 id=1 / deq flags=24     <- only the TX buffer ever returns
```

(`es_module_t`: ADC=1, DAC=2, ADC_DAC=3.) So both nodes have the **correct** direction,
`rx_en` is true, and RX buffers really are reaching the lower half. `VP: rx_worker fired`
**never** appears — the RX DMA is armed and never completes.

**Cause**, `esp32s3_i2s.c:2876-2880`:

```c
nbytes = apb->nmaxbytes;                        /* 8192 */
nbytes -= (nbytes % (priv->data_width / 8));    /* 8192 % 2 == 0 -- no change */
nbytes = MIN(nbytes, ESP32S3_DMA_BUFLEN_MAX);   /* -> 4095   ODD */
```

The alignment runs **before** the clamp, so the clamp destroys it. 4095 propagates into
`i2s_rxdma_start` (`:763-766`) and is written to `I2S_RX_EOF_NUM`. The RX module fills
the DMA in 32-bit FIFO words, so its received-byte counter advances 4 at a time —
0, 4, … 4092, 4096 — and **never equals 4095**. `in_suc_eof` never fires,
`i2s_rx_schedule` is never called, `i2s_rx_worker` never runs. Capture stalls forever.

TX is immune: its EOF comes from the descriptor chain, not a byte-count register.
ESP-IDF avoids this by 4-byte-aligning `dma.buf_size` before
`i2s_ll_rx_set_eof_num()`. Same family as the DMADESC bug — `ESP32S3_DMA_BUFLEN_MAX`
is a descriptor-sizing maximum being used as a word-aligned hardware threshold.

**VM patch #9:** re-align after the clamp — `nbytes &= ~UINT32_C(3);` → 4092, which is
divisible by 2, 3 and 4, so 16/24/32-bit widths are all safe.

### ✅ Patch #9 CONFIRMED on hardware — and two more bugs behind it

Build `nuttx-eofalign.bin` (887,592 B, md5 `35eea0c23b622ad99fb975aa624f95b6`):

```
VP: record started, entering loop
VP: rx_worker fired                    <- NEW.  RX completes.  Patch #9 was correct.
VP: pb mode=1 n=8192
VP: i2s_receive rx_en=1
VP: msg size=8 id=1
VP: deq flags=1c nbytes=0 nmax=8192    <- record buffer: RECORD|FINAL|DEQUEUED, EMPTY
VP: msg size=8 id=1
VP: deq flags=24 nbytes=0 nmax=8192    <- play buffer
```

`I2S_RX_EOF_NUM` was the stall. Two further defects are now visible:

#### 🔴 FIFTH BUG — RX length sum can't handle a multi-descriptor chain

`i2s_rx_worker` (`esp32s3_i2s.c:1292-1300`):

```c
while (dmadesc != NULL && (dmadesc->ctrl & ESP32S3_DMA_CTRL_EOF))
  { apb->nbytes += DATALEN(dmadesc); dmadesc = dmadesc->next; }
```

`esp32s3_dma_setup` sets `suc_eof` **in software only for TX** (`esp32s3_dma.c:324-331`);
for RX the hardware sets it, and only on the **last** descriptor. So on any chain longer
than one the test fails at descriptor 0 and `nbytes` stays 0.

It is always longer than one here: `esp32s3_dma.c:248` checks `esp32s3_ptr_extram()`, and
with `CONFIG_ESP32S3_SPIRAM=y` the 8 KB audio buffers are in PSRAM, so `dma_size` becomes
`0x1000 - alignment` (4080 for 16-byte blocks) and 4092 bytes splits in two.

**VM patch #10:** sum every descriptor, `break` *after* the one carrying EOF.

#### 🔴 SIXTH BUG — `AUDIO_APB_FINAL` on every captured buffer kills the record worker

`i2s_rx_worker:1306-1309` stamps `AUDIO_APB_FINAL` whenever `priv->streaming == false`.
`priv->streaming` is only ever set by `I2S_IOCTL(AUDIOIOC_START)` (`:2966-2972`) — and
**`es8311.c` contains no `I2S_IOCTL` call at all**, so it is false forever.

`es8311_returnbuffers` (`es8311.c:1248-1262`) reads `AUDIO_APB_FINAL` as end-of-stream and
sets `priv->terminating = true`, so the record worker exits after **one** buffer. That is
exactly the single record dequeue in the log above.

**VM patch #11:** set `priv->streaming = true` in `i2s_receive` when a transfer is queued.
⚠️ The upstream-correct fix is for `es8311_start`/`es8311_stop` to forward
`AUDIOIOC_START`/`AUDIOIOC_STOP` down via `I2S_IOCTL`; patch #11 is the low-risk local
equivalent, chosen because the VM's `es8311.c` already carries the `-ERANGE` fix and its
text differs from the D: copy. Revisit if this is ever sent upstream.

### 🔴 SEVENTH BUG — nxlooper's copy path underflows (crash, 2026-08-03)

Build with patches #10 + #11 (887,592 B, md5 `e8f4b6528a73f8c1db165c9b3dd4ec34`) panics:

```
es8311_setsamplerate: Failed to set sample rate: -22   (x2, benign)
xtensa_user_panic: User Exception: EXCCAUSE=001c task: nxlooper
```

`EXCCAUSE=0x1c` is Xtensa **LoadProhibited** — a load from an unmapped address.

⚠️ **Do not read the missing `VP:` lines as "it crashed early".** `CONFIG_SYSLOG_BUFFER=y`
means syslog passes through an intermediate buffer whose tail is lost on panic. The probes
almost certainly ran.

Both new patches were cleared by inspection:

* **Patch #11 is inert.** A grep shows `priv->streaming` is read in exactly ONE place,
  `esp32s3_i2s.c:1306`. It can only suppress `AUDIO_APB_FINAL`; it cannot fault.
* **Patch #10 does not walk off the chain.** `esp32s3_dma.c:313` sets
  `dmadesc[i].next = &dmadesc[i + 1]` in the loop, but `:316-319` breaks once `bytes == 0`
  and `:333` then writes `dmadesc[i].next = NULL`. At 4092 bytes with `dma_size` 4080 the
  chain is exactly 2 descriptors and is properly NULL-terminated.

**Cause: the patches worked, and that woke up dead code.** Until now the record worker died
after one buffer, so `dq_count(&playdq) != 0 && dq_count(&recorddq) != 0`
(`nxlooper.c:543`) was never both-true. Patch #11 keeps the recorder alive and patch #10
gives it a real `nbytes`, so the copy body below now executes — in nxlooper's own task,
which is the task the panic names. `nxlooper.c:551-555`:

```c
copy = MIN(apbrec->nbytes - apbrec->curbyte,
           apb->nmaxbytes - apb->curbyte);
memcpy(apb->samp + apb->curbyte, apbrec->samp + apbrec->curbyte, copy);
```

`nbytes` and `curbyte` are unsigned. `apbrec` is a `dq_peek` (`:548`), so it can be a
partially-consumed buffer carrying a non-zero `curbyte` while a requeue writes a smaller
`nbytes` — the subtraction wraps to ~4 G and `memcpy` leaves the buffer.

**VM patch #12:** print all four operands plus both `samp` pointers, and clamp `copy` to 0
on a bad state. Note the guard converts the panic into a *spin* (`:559`'s
`curbyte == nbytes` never advances), which is intentional — a hang preserves console
output, a panic eats it. Expect to need RESET rather than `q`.

## ⛔ RETRACTED: "every audio test must start from a cold boot" ⛔

**This rule was wrong and has been withdrawn (2026-08-03, later the same day).** Cold-booting
is still harmless and still worth doing for consistency, but it does **not** prevent the
`EXCCAUSE=001c` panic and must not be treated as a precondition for a valid test.

### How the retraction was established

The original rule rested on a single control experiment (n = 1 warm panic, n = 1 cold clean).
Two further binaries were then cold-booted:

| Binary | Delta | Cold-boot result |
|---|---|---|
| `nuttx-cpguard.bin` | #9–#12 | clean (the original n=1) |
| `nuttx-rxreset2.bin` | +#15 | clean |
| `nuttx-txstop.bin` | −#15, +#16 | **panic ×2** |
| `nuttx-cpguard.bin` (re-flashed as control) | #9–#12 | **panic** |

The last row is decisive: the *same* binary that cold-booted clean also cold-booted into the
panic. The fault is **intermittent regardless of boot state**, and the two clean runs were
luck. Consequences:

* **Patch #16 is exonerated** — it was suspected only because the panic followed it.
* A single clean run proves nothing. **Any claim that a change fixed or caused the panic
  needs repeated runs**, because the base rate of the fault is high but not 100 %.
* The "stale ES8311 register state" explanation below is **not supported** by the evidence
  and should be treated as an open question, not a finding.

### The superseded reasoning (kept for the retractions it contains)

Five consecutive `EXCCAUSE=001c` panics were blamed, in turn, on patch #13's probes, patch
#14's probes and patch #15's RX reset. Then the known-good patch-12 binary was reflashed as
a control:

```
nuttx-cpguard.bin  887,688 B  md5 2c77c9b50ca3b4e4a477c560998acd40
```

That is the *exact* binary that had earlier produced `VP: cp copy=4092` with no panic — and
it panicked too. After a full USB power-cycle the same binary ran clean.

**Retractions — these earlier conclusions were wrong:**

* Patches **#13, #14 and #15 did NOT cause any panic.** All three remain **untested**.
  (This retraction still stands.)
* ~~The `EXCCAUSE=0x1c` panic is **stale ES8311 register state after a soft reset**, not a
  code defect in any recent patch.~~ — **itself retracted, see the table above.** The panic
  is not code-defect-free and is not explained by soft-reset state; it is an unexplained
  intermittent LoadProhibited in nxlooper's setup path, and it is still open.
* **Never localise a fault by absence of console output.** This console is USB CDC-ACM
  (`VID_0525&PID_A4A7`); on panic the queued IN endpoints are never serviced and the tail is
  discarded. That is also why no stack dump ever appears — it is not a locked UART and not
  a faulting panic handler.

### Also ruled out by inspection during this hunt

* **The `-22` sample-rate errors are benign.** `MCLK = 4096000` is 256 × 16000, which proves
  the 16 kHz request *did* reach the driver; the failing call is a separate configure at the
  codec's default 48 kHz. The same two `-22` pairs appear in runs that go on to work, and
  `es8311.c:687-691` returns before touching any I²S call, so it cannot leave state
  half-initialised. Do not "fix" this.
* **Patch #7 (TX priming) is clean.** `es8311_enqueuebuffer` guards the message send with
  `if (priv->mq.f_inode != NULL)` (`es8311.c:1739`), so enqueueing before start really does
  just land on `pendq`.

⚠️ **Instrumentation hazard:** `syslog()` in ISR context dispatches through the syslog
channel's `sc_force` method, which CDCACM does not provide — the NULL call faults with
`EXCCAUSE=0x14` (InstrFetchProhibited). `i2s_rx_schedule` is called *from*
`i2s_rx_interrupt`, so it counts as ISR context. Never put `syslog` there. Record into a
static and print from thread context.

### Confirmed cold-boot baseline (control build, patches #9–#12, no #15)

```
VP: primed TX with silence ret=0
VP: pb mode=2 n=8192 / VP: play started ret=0
VP: pb mode=1 n=8192 / VP: i2s_receive rx_en=1     (x2)
VP: record started, entering loop
VP: deq flags=24 nbytes=0 nmax=8192                 <- only the primed TX buffer returns
```

No `VP: rx_worker fired`. **The RX stall is real and reproducible from cold** — which is
what patch #15 targets.

### 🔴 THIRD BUG (found by inspection 2026-08-03, not yet the cause of the RX stall)

`/dev/audio/pcm0` and `/dev/audio/pcm_in0` are two separate `es8311_dev_s` instances
driving **one physical codec** over I²C. `nxlooper` configures record first
(`AUDIO_TYPE_INPUT`, `nxlooper.c:1077-1084`) then play (`AUDIO_TYPE_OUTPUT`, `:1095-1097`).

`es8311_configure` calls `es8311_reset(priv)` in **both** cases, and `es8311_reset`
writes `ES8311_RESET_REG00 = 0x80` (`es8311.c:2169`) — "puts all ES8311 registers back
in their default state". Then `es8311_setbitspersample` programs only the path matching
that instance's `audio_mode`:

* ADC → `ES8311_SDPOUT_REG0A` (`:633-639`)
* DAC → `ES8311_SDPIN_REG09` (`:641-647`)

So the play node's reset **wipes the REG0A word length the record node just programmed**,
and nothing reprograms it. The mic runs at the codec's power-on default format.

This produces garbage/silent capture, **not** a missing `suc_eof` — so it is not the
current stall. But it will bite immediately once the stall is fixed.

**Planned fix (VM patch #8, NOT yet applied):** in the `AUDIO_TYPE_OUTPUT` case, set
`priv->audio_mode = ES_MODULE_ADC_DAC` across `es8311_reset` +
`es8311_setsamplerate` + `es8311_setbitspersample`, then restore `ES_MODULE_DAC` before
returning — the restore is mandatory, otherwise `es8311_processbegin` (`:1365`) would
take the `I2S_RECEIVE` branch for the playback node. Note the record node's
`es8311_start` re-enables the ADC (`:1425-1433`) and, with VM patch #6, record starts
*after* play, so the ordering works in our favour.

⚠️ The VM's `es8311_configure` already carries the `-ERANGE` fix, so its text around
`:1037-1051` differs from the D: copy — check the anchor before scripting this one.

### Ruled out (do not re-investigate)

* Record node is **not** stuck in DAC mode — it is configured `AUDIO_TYPE_INPUT`, which
  calls `es8311_audio_input()` → `audio_mode = ES_MODULE_ADC` (`:1084`, `:1971`).
  The two `es8311_setsamplerate: -22` pairs at loopback time are the two `es8311_reset()`
  calls, one per node, which is positive proof both configures ran.
* `CONFIG_AUDIO_I2S` does **not** guard the direction branch — `es8311_processbegin` is
  unconditional. Our `es8311_processbegin` takes **one** argument
  (`FAR struct es8311_dev_s *priv`), has no `type`/`apb` parameter and no
  `AUDIO_TYPE_RECORD` dispatch; `I2S_RECEIVE` takes **five** args with callback
  `es8311_processdone`. Advice written against the other, multi-arg variant of this
  driver does not apply here.

### ABORT CRITERIA

If Phase 1 is not passing after **~2 working sessions**, stop and reflash the golden
binary. Voice control is a stretch feature; the enclosure assembly and the open JLC
reorders are committed deliverables and outrank it.

---

## PHASE 1.5 — Borrow TFLM's micro_speech (do this before collecting any data)

Found 2026-08-02: the vendored tflite-micro tree already contains a complete,
**pre-trained** keyword-spotting example. Nothing to train, nothing to record.

```
apps/mlearning/tflite-micro/tflite-micro/tensorflow/lite/micro/
  examples/micro_speech/models/micro_speech_quantized.tflite    18,800 B  yes/no/silence/unknown
  examples/micro_speech/models/audio_preprocessor_int8.tflite    8,772 B  MFCC frontend, as a graph
  models/keyword_scrambled_8bit.tflite                          29,632 B  second KWS model
```

Two things this changes:

1. **The feature frontend is a tflite graph, not code you write.** Phase 3 originally
   assumed hand-rolling MFCC against `CONFIG_MATH_KISSFFT`. Invoking
   `audio_preprocessor_int8.tflite` instead removes that code entirely *and* removes
   the classic KWS failure mode — host-side and device-side features diverging, which
   shows up as a model that scores 95 % in training and 40 % on the board, with nothing
   to debug because both halves "work".
2. **You can prove the whole chain before investing in a dataset.** 18.8 KB is inside
   the arena budget Phase 4 assumes. Wire it up, say "yes" at the board, and if it
   lands then I²S capture → preprocessing → third interpreter → result is all verified.

**Gate:** the board recognises "yes" and "no" spoken at the enclosure. Only then start
Phase 2. If this doesn't pass, the fault is in the pipe, not in your data — and finding
that out now costs a day instead of a fortnight.

⚠️ `micro_speech` is trained at 16 kHz mono, which matches the Phase 1 I²S settings.
Keep them aligned; a sample-rate mismatch here degrades accuracy silently rather than
erroring.

## PHASE 2 — Dataset  *(REVISED 2026-08-05 — no recording session)*

**This phase originally specified recording ~10–15 words yourself**, through this mic,
in the printed enclosure, with the stepper running, and rated the outcome *"roughly a
coin flip"* against the schedule. That risk was priced against building a dataset from
scratch. It turns out not to be necessary, and the paragraphs below supersede it.

**Google Speech Commands v0.02** (Warden 2018 — 105,829 one-second 16 kHz clips,
2,618 speakers, 35 words, CC BY 4.0) already contains every word the scheduling dialog
needs. The vocabulary was chosen to fit the dataset rather than the other way round:

| dialog step | words | in dataset |
|---|---|---|
| meal slot | `one` `two` `three` | ✅ (reuses the digits) |
| hour | `zero`–`nine` | ✅ |
| confirm | `yes` `no` | ✅ — *already shipping on the board* |
| **pet** | — | **never spoken; the camera already identifies the pet** |

The words the dataset *lacks* — "schedule", "breakfast", a wake word — are exactly the
ones a button and three numbered meal slots replace at no cost. See
[PHASE 5](#phase-5--wire-to-the-ui) for what that costs the UX.

This is not a new dependency either: `micro_speech`, the model Phase 1.5 already put on
the board at yes 0.980 / no 0.789 live, was trained on this same dataset. **The domain
gap between crowdsourced laptop audio and an ES8311 in a printed enclosure is therefore
measured on this hardware, not hypothetical** — and it is survivable.

Custom recording is not deleted, only demoted: it becomes *fine-tuning on top of a
working model* if the digits underperform on-device for accent reasons. That is hours
of work from a good starting point, not a fortnight from zero.

## PHASE 3 — Train (host side)  *(REVISED 2026-08-05)*

Built and documented at **`host/kws/`** — see `host/kws/README.md` for the full recipe.

Architecture is **`tiny_conv` retrained at 14 labels**, not DS-CNN-S. Retraining
`micro_speech`'s own architecture keeps the input shape, the preprocessor graph, and the
op set identical to what Phase 1.5 already proved on this board; the only thing that
changes is the number of output classes. DS-CNN-S would be smaller at 14 labels
(~38.6 KB vs ~50 KB, because global average pooling collapses the head) but needs
`DEPTHWISE_CONV_2D` + `AVERAGE_POOL_2D` added to the resolver and re-proves nothing.
~11 KB is not worth spending a bring-up on when 5 MB of flash is unallocated.

**Label order is load-bearing.** `input_data.prepare_words_list()` returns
`[_silence_, _unknown_] + wanted_words`, so putting `yes,no` first *extends* the device
enum instead of renumbering it:

```
 0 _silence_   1 _unknown_   2 yes     3 no
 4 zero   5 one   6 two   7 three   8 four
 9 five  10 six  11 seven 12 eight  13 nine
```

`VELAPAW_KWS_YES = 2` and `NO = 3` stay valid, so build 54's code keeps working.
Reordering `WORDS` without editing `kws.h` makes the board confidently report the wrong
word with nothing in the logs to explain it.

The ~23 dataset words we don't ask for fold into `_unknown_` automatically — free
negative training data, which is the main reason a public dataset beats a hand-recorded
one at this vocabulary size.

Feature geometry is asserted equal to the board (49 frames × 40 bins = 1960, 16 kHz,
30 ms window / 20 ms stride); `PREPROCESS='micro'` is the same `audio_microfrontend`
algorithm `audio_preprocessor_int8.tflite` implements on-device. Training is CPU-only —
TF dropped native Windows GPU after 2.10 and this venv has the `tensorflow-intel` wheel.
18,000 steps is an overnight job.

**Gate:** int8 test accuracy ≥ 90 % → proceed; 85–90 % → usable, expect more "say
again"; < 85 % → stop before spending device time. Expect the digits below yes/no —
`three`/`free` and `nine`/`five` are genuinely close.

### Result — 2026-08-05: PASSED, on the metric that matters

18,000 steps, 84.2 % validation. Test split n=5623: **float 80.90 %, int8 80.85 %**,
model 59,024 B. Under the gate line above — and the gate line was measuring the wrong
thing. The deficit is nearly all `_unknown_` at 34.1 %, a class the product never names.

VelaPaw never makes a 14-way decision; each dialog step knows its legal answers, so
argmax restricted to that subset is the real number:

| step | int8 | with a raw-probability threshold |
|---|---:|---|
| MEAL 3-way | **95.68 %** | p ≥ 0.45 → accept 74.8 %, correct **99.24 %** |
| HOUR 10-way | **84.95 %** | p ≥ 0.73 → accept 64.6 %, correct **96.87 %** |
| CONFIRM 2-way | **97.45 %** | p ≥ 0.59 → accept 80.7 %, correct **99.55 %** |

🔴 **Threshold on the raw softmax probability of the best legal word, not on a
confidence renormalised over the legal subset.** Renormalising discards the probability
mass on `_unknown_` — precisely the mass that means "not one of my words" — and lets
**86 % of silence/OOV clips through at CONFIRM** versus 4.9 % for the raw test at a
comparable accept rate. This is a device-code decision, not an analysis detail.

End to end, three tries per step: a wrong time reaches the confirm screen 3.7 % of the
time and the owner rejects it there, so a **wrong feeding time is saved in 0.017 % of
dialogs**; 6.0 % of dialogs are abandoned after three tries (HOUR's threshold is the
dial). Full derivation and the per-word confusion table in `host/kws/README.md`.

These are laptop-mic clips — on-board figures through the ES8311 will be lower, so
re-tune the three thresholds on hardware.

## PHASE 4 — On device  *(BUILT in b54; REVISED 2026-08-05 for 14 labels)*

Already done — `app/velapaw/voice/kws.cc` runs the third and fourth interpreters
(frontend + classifier) with their own arenas, exactly the `backend_tflm.cc` pattern.
Actual numbers, not the estimates this section used to carry:

| | planned | as built |
|---|---|---|
| classifier arena | ~30 KB | `KWS_CLASSIFIER_ARENA` **8 KB** |
| frontend arena | — | `KWS_FRONTEND_ARENA` **12 KB** |
| classifier resolver | `<8>` | `MicroMutableOpResolver<4>` (**`AddConv2D`**, not depthwise, from b55) |
| frontend resolver | — | `MicroMutableOpResolver<18>` |

**Embed the model in rodata**, do not flash-load it — it rides inside `nuttx.bin` as a
`const uint8_t[]`, no flash offset, no `esptool` line, no cache-suspend `.bss`-stack
thread. Simplest of the four models by far.

```bash
python3 host/gen_model_data.py --input host/kws/models/kws_int8.tflite \
    --symbol g_micro_speech_quantized_model_data --outdir app/velapaw/voice/gen
```

What the 14-label model changes — **as done in b55**, two items differing from what this
section predicted:

1. `voice/kws.h` — `VELAPAW_KWS_CATEGORIES` 4 → 14, `ZERO`…`NINE` at 4–13, plus
   `VELAPAW_KWS_DIGIT_OF()` / `LABEL_OF()` so callers never open-code the offset.
2. `voice/kws.cc` — label-name table extended, `static_assert`ed against the enum.
3. 🔴 **`AddConv2D()`, not `AddDepthwiseConv2D()`.** This section previously said the
   resolver needed no change at all. Half right: the slot count stays `<4>`, but the op
   is different — the shipped micro_speech model used a depthwise conv and `tiny_conv`
   emits a plain `CONV_2D` (verified against the op list in `kws_int8.tflite`). This is
   not a link error; it is `AllocateTensors` failing at boot with "Didn't find op for
   builtin opcode".
4. 🔴 **`KWS_CLASSIFIER_ARENA` 8 KB → 16 KB.** Also predicted wrong here: arenas do not
   scale with *class* count, true, but they do scale with the activations, and this
   architecture is different from the one that was measured at 6916 B. The conv
   activation is 25×20×8 = 4000 B and the flattened 4000 B feeding `final_fc` is live
   alongside the 1960 B input → peak ~10 KB. The board prints
   `kws: classifier ready, arena used N/16384 B`; shrink to the measured value.
5. **Leave `CONFIG_VELAPAW_KWS_SELFTEST` ON for the first boot** — reversing the
   instruction that stood here. The claim that "its vectors assert 4-label indices" was
   wrong: the two bit-exactness vectors test the *frontend* graph, which is unchanged,
   and the end-to-end clips assert `YES`/`NO`/`SILENCE` = 2/3/0, which the training
   label order deliberately preserved. It is the cheapest end-to-end proof that the new
   classifier works on known audio, before any microphone is involved. Turn it off
   afterwards to reclaim its ~98 KB (`Makefile:109-114`).

Model size goes 18,800 B → **59,024 B**. The `final_fc` head is 4000 × n_labels and
dominates — this architecture essentially *is* its classifier head.

### The two real risks (neither is model count)

1. **Continuous duty cycle on a single core.** `CONFIG_SMP` is off. Ungated KWS at a
   100 ms stride costs ~40 % of the core, permanently, against LVGL + camera + a
   1310 ms recognition. **Gate it behind VAD** — `apps/audioutils/speexdsp` is already
   in the tree. Cheap DSP decides "is anyone talking"; the expensive model only runs
   when the answer is yes. Quiet kitchen → model runs ~never.

2. **Audio underrun during the 1310 ms inference.** DMA keeps filling; if the drain
   thread isn't scheduled, samples vanish silently and the wake word is missed at
   exactly the moment a pet is at the bowl. Two mitigations, both must be designed in:
   - audio drain thread priority **above** the recognition worker
   - DMA buffers sized for ≥2 s (16 kHz × 16-bit mono = 32 KB/s → 64 KB PSRAM)

Do not enable `CONFIG_SMP` to solve this. Recall what `CONFIG_RTC` did to the console.

## PHASE 5 — Wire to the UI

Recognized word → the existing schedule path. Voice only has to produce a
**(slot, minute-of-day)** pair; storage already exists and is untouched:

```c
/* identity.h:20 */  int meal_min[VELAPAW_MAX_MEALS];   /* minute of day, -1 = off */
                     velapaw_identity_set_meals(..., const int *meal_min);
```

**Entry is a 🎤 button on Edit Schedule — there is no wake word.** Two reasons, and
both are decisions rather than shortcuts: "Hi VelaPaw" is not in Speech Commands, and
continuous listening costs ~40 % of the single core permanently (see the risks above).
A button is one tap the user was already making on that screen.

The dialog, one word per step:

```
🎤  "Meal? say 1, 2 or 3"        →  one|two|three   →  slot
    "Meal 1. Hour? say 0-9"      →  zero..nine      →  hour
    "08:00 — yes or no?"         →  yes|no          →  commit / retry
```

Each capture is ≈2 s of wall time; the whole dialog runs ≈10–12 s. A single spoken
digit plus the AM/PM sense carried by the confirm step reaches **00–09 and 12–21** —
which is every hour anyone actually feeds a pet. 10, 11, 22 and 23 fall back to the
`+`/`−` buttons that already exist, so nothing is unreachable.

**The pet is never spoken.** The camera identifies it — asking the owner to say a name
the model was never trained on would be strictly worse than the sensor already on the
board.

State machine: `IDLE → MEAL → HOUR → CONFIRM → DONE`, replacing `voice_apply()`, with
per-step thresholds — **0.45 MEAL / 0.73 HOUR / 0.59 CONFIRM on the raw softmax
probability of the best word legal at that step** (Phase 3 result; do not renormalise
over the subset) — a "say again" retry capped at three, a cancel button, and a
timeout. `VOICE_YES_SCORE 0.80` / `VOICE_NO_SCORE 0.60` at `ui_lvgl.c:451-452` are b54's
4-label numbers and are superseded for the dialog path. The screen
shows "🎤 Listening…" and echoes the recognized word — the 3.5" display is the audio
feedback channel, which is better than a beep and works even if the speaker path lags.
### The Chinese strings need no font work

A glyph outside the shipped 150-glyph subset renders **blank**, silently, with no
build error — so this was checked rather than assumed. The natural wording needs 13
missing glyphs (`、再几午否听或是次清聆说，`) plus the em-dash `—`, and
`NotoSansSC-Regular.otf` is not on the build machine.

It doesn't matter. The spoken words are English (`yes`, `one`, `eight`) because that
is what the model knows, so the Chinese only carries connective text and quoting the
English verbatim is correct rather than a compromise. Verified against the real glyph
set — every step covered, nothing regenerated:

```
S_VC_MEAL     第 %d 餐?  1 / 2 / 3
S_VC_HOUR     第 %d 餐。时? 0-9
S_VC_CONFIRM  %02d:00 - "yes" / "no"?     <- ASCII hyphen, the em-dash is missing too
S_VC_SAVED    已设置: 第 %d 餐 %02d:00
S_VC_RETRY    请重试
S_VC_CANCEL   已取消
S_VC_LISTEN   等待中...                    <- ASCII dots, U+2026 is missing
```

Downloading Noto Sans SC (OFL) and re-running `lv_font_conv` at all three sizes buys
nicer phrasing (请说…, 聆听中). That is an upgrade, not a dependency.

---

## Scope honesty

Free-form speech — *"velapaw schedule the first feeding time for pet king at 8"* as one
spoken sentence — **will not work on-device.** No ASR engine exists in the openVela
tree and there is no room for one. On-device gets you a guided dialog tree. The literal
sentence requires cloud NLU via `external/bailian_sdk` (`libqwen_sdk.a`,
`libc_mmi_cmd_voice_translate.a` are vendored in-tree). This is unchanged.

**Confidence, revised 2026-08-05.** This section used to read *"a usable keyword model
inside the remaining schedule is roughly a coin flip — the bet is the dataset and
training."* That bet no longer exists: the dataset is public, complete, and is the same
one that trained the model already recognising "yes" on this board at 0.98. What remains
is an overnight CPU run and a state machine, both of which are ordinary work with known
failure modes. The residual risk is narrow and named — **accent**, since Speech Commands
skews US English, and **digit confusability** (`three`/`free`, `nine`/`five`), which the
confirm step exists to absorb.

---

## OPEN: the intermittent setup panic (2026-08-03)

Two distinct, independent problems are now in play. Do not conflate them again.

**Problem A — the RX stall.** Capture delivers at most one buffer (usually zero), and
`VP: rx_worker fired` never recurs. **Still open.**

Patch **#16** (clear `I2S_TX_STOP_EN`, bit 13 of `I2S_TX_CONF_REG`, hardware default 1, in
`i2s_configure` at `esp32s3_i2s.c:1503`) was the clock-starvation theory: the bit stops
BCK/WS when the TX FIFO empties, and in full duplex RX is a slave borrowing those signals via
`I2S_SIG_LOOPBACK`. **#16 is now TESTED AND DISPROVEN** — build `nuttx-m18.bin` contains it,
reached the loop cleanly, and still produced zero RX completions. Keep or revert as
preferred; it changes nothing observable.

### The arithmetic that makes this hard to explain

The primed silent TX buffer is 8192 B = 4096 samples = **256 ms** of bit clock at
16 kHz/mono/16-bit. RX needs only 4092 B = 2046 samples = **128 ms** to reach
`I2S_RX_EOF_NUM`. Capture should complete *twice over* inside the single playback buffer we
know was transmitted. It completes zero times. So either the RX module is not being clocked
at all despite `I2S_SIG_LOOPBACK`, or the DMA in-channel is not advancing.

**The next test should be a measurement, not a patch:** from thread context (safe), on the
second `i2s_receive` call, dump `DMA_IN_INT_RAW_CH0`, `DMA_IN_SUC_EOF_DES_ADDR_CH0`,
`I2S_RX_CONF_REG`, `I2S_TX_CONF_REG`, and the `ctrl` word of each RX descriptor. That
distinguishes "no clock" from "DMA never armed" from "EOF fired but the ISR path is broken",
which three builds of inference have failed to separate.

### Ruled out for Problem A (do not re-investigate)

* RX IRQ never enabled — **false.** `esp32s3_i2s_initialize` calls `i2s_rx_channel_start`
  (`:3198`), which does `up_enable_irq(priv->rx_irq)` and sets
  `DMA_IN_SUC_EOF_CH0_INT_ENA` (`:2181-2184`) at boot.
* Codec register corruption (drafted patch **#8**, the shared `es8311_reset` wiping REG0A)
  cannot be the cause of *this* symptom. A wrong serial-port format yields **wrong data, not
  absent data** — the RX DMA counts clocked-in bytes regardless of content, so a format
  mismatch would still fire EOF. #8 may still be worth applying for audio *quality*, but it
  will not fix a stall.
* `i2s_tx_channel_stop` / `i2s_rx_channel_stop` are not called mid-run; `I2S_TX_START`
  stays set (`:2225`, `:2276` are the only clear sites, both on the stop path).

**Problem B — the intermittent panic.** `EXCCAUSE=001c` (LoadProhibited) in task `nxlooper`,
landing *after* the two benign `-22` configure lines and *before* the loopthread starts. High
base rate, not deterministic. Blocks testing Problem A, because most runs die before the loop.

### Localised, but not yet caught in the act

Probe patches **#17** (`VP: L1`…`VP: L6`, in `nxlooper_loopthread`) and **#18**
(`VP: M1`…`VP: M7`, in `nxlooper_loopback`) bracket every statement from the second
`AUDIOIOC_CONFIGURE` to the loopthread's first buffer. A run that *survived* printed all of
them in order, which rules the whole setup path healthy when it doesn't fault:

```
VP: M1 both configures done   VP: M2 bufinfo n=4   VP: M3 mq opened
VP: M4 record mq registered   VP: M5 play mq registered   VP: M6 pre pthread_create
VP: L1 loopback entry   VP: L2 rec bufinfo size=8192 n=4
VP: L3 rec[0..3] ret=8 want=8 apb=0x3c4bdac0/0x3c4bfb60/0x3c4c1c00/0x3c4c3ca0
VP: L4 enq rec[0..3] ret=0
VP: L5 play bufinfo size=8192 n=4   VP: L6 play[0..3] ret=8 (all non-NULL)
VP: primed TX with silence ret=0   VP: M7 loopthread created
```

**A panicking run must be captured with these probes in place** to name the statement. Until
then the location is only bounded, not known.

#### Inspection candidate, NOT confirmed and NOT consistent with the clean run

`nuttx/audio/audio.c`:

* `audio_allocbuffer:786-789` returns **0** *without writing* `*bufdesc->u.pbuffer` when
  `upper->periods >= upper->nbuffers`.
* `audio_enqueuebuffer:861-865` indexes `upper->apbs[priv->head % upper->periods]` on the
  `bufdesc->u.buffer == NULL` branch — with `periods == 0` that is a modulo by zero and a
  load through a possibly-NULL `upper->apbs`, which would give exactly `EXCCAUSE=001c`.

But the clean run shows `ret=8` (not 0) on all eight allocations and `nbuffers=4`, so the
early return did **not** fire there. This candidate explains a fault only if `upper->nbuffers`
is somehow 0 on the failing runs. Treat as unproven.

### Method rules earned the hard way

* Probes in `i2s_rx_schedule` / `i2s_rx_interrupt` are **ISR context** — `syslog()` there
  faults (`EXCCAUSE=0014`). Record into a static, print from thread context. Probes in
  `nxlooper.c` are thread context and safe.
* Absence of console output localises nothing (CDC-ACM discards the tail on panic).
* One clean run is not evidence. Repeat before attributing.

---

## 2026-08-03 — patch #20 tested and DISPROVEN; RX stall still open

### The fix that was tried

`esp32s3_i2s.c i2s_set_datawidth()` has the PCM / non-PCM branches **swapped for RX**
(compare the TX block ~30 lines above):

* TX: PCM -> `TX_TDM_WS_WIDTH = 1`, else -> `data_width - 1`
* RX: PCM -> `RX_TDM_WS_WIDTH = data_width - 1`, else -> **`1`**

We run standard I2S (non-PCM), so TX generated a 32-BCLK frame while RX was told the WS
pulse was 2 BCLK wide. This is a genuine upstream transposition bug and was fixed as
**patch #20a**, together with **#20b** (the RX branch never pulsed `I2S_RX_UPDATE`; TX does).

### Result: no change whatsoever

The RXDUMP came back **bit-for-bit identical** to the pre-#20 run:

```
rxconf=0008960c txconf=08089204 eofnum=00000ffc
raw=000000a0 link=001bb2c4 state=003fb2d0 fifo=0f810f16
eofdes=00000000 dscr=00000000 act=1 pend=1 done=0
d[0] ctrl=80000ff0   d[1] ctrl=80000010
```

Patch #20 **did execute** — `es8311_setbitspersample()` calls `I2S_RXDATAWIDTH` which reaches
`i2s_set_datawidth()`. So this is a tested-and-disproven fix, not an untested one. Keep the
patch (it is still correct), but it is not the cause.

### What the identical dump means

Changing the RX frame geometry changed nothing at all. If RX were merely *mis-framing* a
signal it could see, altering WS width would have changed *something* — a different byte
count, a descriptor error, garbage data. Zero change across a framing change means the RX
front end is receiving **nothing**, not misinterpreting something.

### Register decode, for the record

* `txconf=08089204` -> TX_START=1, master, TDM_EN=1, `SIG_LOOPBACK=1` (bit 27),
  `TX_STOP_EN=0` (patch #16 confirmed live on hardware).
* `rxconf=0008960c` -> RX_START=1, RX_SLAVE_MOD=1, RX_TDM_EN=1, RX_PDM_EN=0, PCM bypassed,
  RX_MONO=0. Configuration is correct.
* `link=001bb2c4` -> inlink armed at d[0], AUTO_RET set, `INLINK_PARK=0` (FSM running).
* `state=003fb2d0` -> `INLINK_DSCR_ADDR` = d[1]: loaded d[0], prefetched d[1], waiting.
* `raw=0xa0` -> only `INFIFO_FULL_WM` (5) and `INFIFO_UDF_L1` (7). Bits 0-4 (`IN_DONE`,
  `IN_SUC_EOF`, `IN_ERR_EOF`, `IN_DSCR_ERR`, `IN_DSCR_EMPTY`) all clear.

The GDMA is healthy and armed. Nothing is being handed to it.

### ⛔ METHOD FAILURE: the D: tree is NOT what runs in the VM

Proven twice on 2026-08-03:

1. The VM's `esp32s3_i2s.c` (git HEAD) lacks `#include <nuttx/mutex.h>`; the D: copy has it.
   A restore-from-git therefore would not link until the include was re-added.
2. `es8311.c` on D: still shows the `-ERANGE` configure bug: `ret = -ERANGE` is set, then
   `ret = es8311_setsamplerate(priv) == -ENOTTY ? OK : ret;` leaves `ret` at `-ERANGE` on
   success, and the following `if (ret < 0) break;` skips `es8311_setbitspersample()` and
   returns `-ERANGE` from `AUDIOIOC_CONFIGURE`. If that were the running code,
   `nxlooper_loopback` would abort at `nxlooper.c:1086` and never reach `VP: L1`. It reaches
   `L1`, so **the VM's es8311.c is patched and D:'s is stale.**

**Rule: do not reason about runtime behaviour from the D: sources.** Pull the actual file
from the VM first, or the analysis is worthless. Much of this hunt was inference from files
that do not match the binary — which is why repeated plausible fixes changed nothing.

### ⚠️ Source file was destroyed and rebuilt

A patch script used `io.open(p, "w", newline="\n")` where the escape had been eaten by a
quoted heredoc. `io.open` truncates the file when it opens the raw handle and validates
`newline` afterwards, so `esp32s3_i2s.c` went to **0 bytes**, taking patches #9/#10/#11/#16/#19
with it. Recovered via `git checkout` + a rebuild script.

**Rule: every patch script writes to `<file>.new` and `os.replace()`s it.** Never open the
target for writing directly.

### Status: RX capture does not work. Two paths remain.

1. **Hardware measurement.** Whether the ES8311 ADC drives anything on DIN (GPIO14) is now a
   scope/logic-analyser question, exactly as the SPI-LCD partial-window bug turned out to be.
   No amount of further register archaeology can answer it.
2. **Codec register readback.** Verify over I2C that the ADC is powered, unmuted and clocked.
   Needs `CONFIG_SYSTEM_I2CTOOL` (not currently in the defconfig) or a purpose-built probe.

Everything on the ESP32 side that can be verified from software has been verified. TX is
proven; RX is armed and starved.

---

## ✅ RESOLVED 2026-08-03 — Phase 1 step 3 complete

The section above is superseded. RX capture works, the loopback is audible, and the
captured stream is clean enough for `micro_speech`. No logic analyser was needed.

### Root cause: the RX TDM slot map was never programmed (patch #25)

Nothing in `esp32s3_i2s.c` ever wrote `I2S_RX_TDM_CTRL_REG`. `i2s_txchannels()`
programs `I2S_TX_TDM_TOT_CHAN_NUM` and the per-slot `CHANn_EN` bits for TX, but
`i2s_rxchannels()` is a **pure no-op** — it validates the channel count, stores
`priv->channels`, and programs no registers at all.

RX therefore sat at the hardware reset default `RX_TDM_TOT_CHAN_NUM = 0`, i.e. **one
slot per frame**, while `I2S_RX_CONF1_REG` described a **two-slot 32-bit frame**. The
receiver clocked and *completed* frames — `I2S_RX_DONE_INT_RAW` really did set — but
every slot was discarded before reaching the GDMA. Fix: mirror `i2s_txchannels()` on the
RX side inside `i2s_configure()`. `nbytes` went 0 -> 4092 immediately and stayed there.

### Why it took so long: every register read healthy

`RX_START=1`, clocks/dividers/framing/bit-width all correct, GDMA bound (`perisel=3`),
descriptors loaded, FSM running, `I2S_RX_DONE_INT_RAW` set. The only honest tells were
`DMA_IN_INT_RAW` bit 7 `INFIFO_UDF_L1` plus `EMPTY_L1` in `DMA_INFIFO_STATUS` (the GDMA
starving), and `IN_DONE` never firing.

**Lesson worth keeping: immunity to change is itself evidence.** Four consecutive
bit-identical RXDUMPs across four unrelated fixes meant the data was being discarded
downstream of everything being touched. That was under-weighted for several builds.

### Two throwaway diagnostics worth remembering

* **Pad-share loopback (#22)** — set `DINPIN = DOUTPIN = 16` and configure the pin
  `INPUT_FUNCTION_2 | OUTPUT`. Feeds TX's own data into RX with the codec out of circuit;
  proves the digital path with no scope and no soldering. Safe **only** on DOUT (the
  codec's high-Z input); never on DIN, which would contend with the codec's output.
* **Force RX master (#23)** — clear `I2S_RX_SLAVE_MOD` to test whether `I2S_SIG_LOOPBACK`
  really shares TX's BCLK/WS with a slave-mode RX. It does; RX was fine as a slave.

### Audio quality: three further real defects (#28, #30)

Getting data flowing was not the same as getting *usable* data.

1. **Digital feedback through the right slot (#28).** `es8311_start` writes
   `ES8311_GPIO_REG44 = 0x50` — ADC on the left slot, **the DAC's own output on the
   right**. Patch #25 enabled both slots, so playback was being fed straight back into the
   record stream, independent of any acoustics. Fixed by setting `I2S_RX_MONO` (BIT 5 of
   `I2S_RX_CONF_REG`); `I2S_RX_MONO_FST_VLD` (BIT 9) already defaults to 1 = first slot =
   the ADC. Keep `TOT_CHAN_NUM=1` and both `CHAN_EN` bits — that is the state that moves
   data.
2. **54 dB of stock mic gain railed the ADC (#30).** `REG14=0x1a` is the analog PGA at its
   **30 dB maximum** (bits[3:0], 3 dB/step) and `REG16=0x24` is a further **24 dB** digital
   scale-up (bits[2:0], 6 dB/step). Every buffer clipped. **The clipping was the "noise".**
   Retuned to `REG14=0x18` (24 dB) + `REG16=0x20` (0 dB).
3. **`CONFIG_ES8311_INPUT_INITVOLUME` does nothing.** `es8311_start` clobbers
   `ES8311_ADC_REG17` to `0xbf` (0 dB) on every start, after `es8311_reset` has applied the
   Kconfig value. Do not try to tune input level with it.

Output level comes from `CONFIG_ES8311_OUTPUT_INITVOLUME` via `29*ln(v)` into
`ES8311_DAC_REG32`, where unity is `0xBF` at 0.5 dB/step: 400 -> `0xAD` = **-9 dB**
(the default, quiet), 1000 -> `0xC8` = **+4.5 dB**.

### Never judge this by ear on `nxlooper loopback`

The mic and speaker are inches apart on one board, so loopback is an acoustic feedback
loop. Loop gain crossing unity sounds exactly like "noise, can't hear my voice", and it
moves with every gain change — which made ear testing actively misleading and cost two
build cycles. Measure the samples instead, with the `VP: mic` probe in the RX worker
(thread context, so `syslog` is safe there): per buffer `peak`, `mean` |sample|, signed
`dc`, `rail` (count >= 32000), `m8` (count == -32768).

Note `peak=32768` is not a valid magnitude — it means a sample is exactly `-32768`
(`0x8000`), whose negation overflows back to itself.

### Measured result at 24 dB gain

| condition | peak | mean | rail |
|---|---|---|---|
| silence | 2-10 | 0 | 0 |
| speech  | 16k-32767 | 2000-2600 (~-22 dBFS) | up to 32 / 2046 (~1.5%) |

Clean noise floor, healthy speech level, mild peak clipping only. `dc` swings +-1500 over
~1 s periods — sub-10 Hz acoustic coupling rather than a true DC offset; it eats headroom
and the ES8311 high-pass could remove it if that ever matters.

### Still open (real, but not blocking)

`i2s_receive` computes `MIN(nbytes, ESP32S3_DMA_BUFLEN_MAX)`, splitting transfers
4080 + 16 = 4096 bytes against `I2S_RX_EOF_NUM = 4092`, with a NULL-terminated non-EOF
tail. A genuine defect, but ruled out as the cause of both the stall and the railing
(`m8=0` throughout once gain was sane).

### ⚠️ DOWNGRADED 2026-08-03 — two frozen samples per buffer (probably not a real defect)

**Read this before acting on anything below it.** After the cold power cycle that cleared
the `RX_HUNG` latch (see the incident section further down), the *same* build-31 binary
produced freely-moving `dc` values — -527, -67, 0, -6193, +4068, -9511 — with no sign of
the -17 pin. The frozen `dc` was almost certainly an early symptom of the capture path
sliding into the hang that followed a few minutes later, not an independent bug: a buffer
holding nothing but two leftover values is exactly what a half-dead RX looks like.

The arithmetic below still stands *as arithmetic*. It just no longer describes a live
condition, and the wake-word argument that follows from it is therefore moot. **Do not
spend a build cycle on this unless it reappears on a freshly power-cycled board.** If it
does, build 32's instrumentation is already built and ready to flash.

Original finding, retained in full because the reasoning is reusable:

Found in the build-31 log, and sharper than the "stale descriptor tail" guess above.
Over ~100 consecutive buffers `dc` was pinned at **exactly -17** while `mean` moved
freely (17..193). `dc` is `sum/n` with `n=2046`, so the buffer carried a fixed component
of about -34,800 — i.e. **exactly two samples of -17404** (`2 x -17404 / 2046 = -17.01`).
Confirmed independently: in the quiet tail `mean` bottomed out at exactly **17**, which
is `2 x 17404 / 2046`, meaning those two samples were the only content left in the
buffer. They arrived during a loud burst, persisted ~13 s, then cleared.

So it is **two samples**, not the 6-sample 4092/4096 gap. Those are separate facts and
the arithmetic does not support blaming the seam without more evidence.

Why it matters: a -5.4 dBFS impulse repeating once per 128 ms buffer is a strong 7.8 Hz
periodic feature that `micro_speech` will see in every spectrogram frame. Unlike the
playback-loudness question, this one lands directly on the wake-word path.

**Diagnostic built and flashed but never read — build 32, md5
`766278060bd24dd980b2b07dca1e4708`, 888400 B** (scratchpad `nuttx-idx32.bin`; the VM
source carries patch #32). The meter now reports `peak=V@IDX` plus the last 8 samples as
`tail=`, and the stride went `& 7` -> `% 5`. The old stride was aliasing: with 4 rotating
record buffers, `vp_seq & 7` always inspected the same `apb` (8 mod 4 == 0), so it watched
128 ms out of every ~1.02 s of ONE buffer. 5 is coprime with 4, so all four now rotate.

Geometry to compare an index against: `d[0]` = 4080 B = samples 0..2039; `d[1]` starts at
sample **2040**; `RX_EOF_NUM` stops at 2045. Index 2040/2041 => desc seam; 2044/2045 =>
end of transfer; a random index with `tail=` moving => the pair is in the body and the
seam theory is wrong.

Test that reproduces it: one loud clap, then ~15 s of silence — the quiet tail is where
the pair is unmissable, because `mean` drops to just their own contribution.

Predicted tell: if only one buffer is affected the artifact should now appear on roughly
every *fourth* meter line, not every line. Every line => all four buffers => the
descriptor chain itself rather than one stale allocation.

Caveat if the meter ever appears to stall capture: it prints from inside the RX worker,
and stride 5 with 8 extra integers per line is roughly **3.2x** build 31's meter console
traffic — the same failure mode that forced the original hot-probe strip. Gate it on
`vpeak > 8000`; that costs nothing diagnostically, since the frozen pair is 17404.

Note `VP: RXDUMP` is emitted from **`i2s_tx_worker`**, one-shot behind `static bool
vp_dumped`. Its position relative to `VP: mic`, and its `act/pend/done` counts, are
scheduling jitter between two workers. Do not read state into them — that nearly cost a
build cycle.

### Playback loudness — closed, not a defect

`nxlooper loopback` sounds quiet and that is expected. The speaker amp *is* on:
`board_speaker_enable(true)` runs unconditionally in `board_lcd_initialize`, with `g_ioe`
assigned 8 lines earlier, so the `g_ioe == NULL` early-return cannot fire (verified
against the live VM copy of the board file, not the D: tree).

`DAC_REG32` sits at `0xC8` = **+4.5 dB** from `CONFIG_ES8311_OUTPUT_INITVOLUME=1000`,
the top of the `29*ln(v)` Kconfig mapping — the mapping is the ceiling, not the silicon
(the register reaches `0xFF`). Speech captures at `mean ~1500-2900`, about **-24 dBFS**
average, with `peak` already at 32767: a ~24 dB crest factor, which is simply what speech
is. **Those peaks are already full-scale, so any further gain — mic or DAC — clips them.**
The average is low because speech is peaky, not because a stage is mis-set. The only tool
that raises average without touching peaks is compression; the ES8311 ALC could do it but
would pump the noise floor between words, which is actively bad for wake-word detection.

It also never reaches the product: nothing in VelaPaw plays back a live microphone. The
mic feeds `micro_speech`, which normalises its own input, and every prompt or chime is a
full-scale asset — roughly 24 dB louder than the loopback. The quietness is a property of
the test rig, so **loopback audibility is not a valid acceptance criterion.** The measured
table above is.

### 🔴 INCIDENT 2026-08-03 — `RX_HUNG`: only a real power cycle clears it

Capture died mid-session and stayed dead across three flashes, **including a reflash of
the exact binary that had worked an hour earlier** (build 31, md5
`4a30fe17854667c535dbe9c2675bd458`, `Hash of data verified.`). Symptom set, bit-identical
on every run:

```
i2sraw=0000000f    bit2 = RX_HUNG SET
eofdes=00000000    RX DMA never reached a single successful EOF
raw=000000a0   fifo=0f810f16   act=1 pend=1 done=0
(and zero `VP: mic` lines)
```

versus the healthy run: `i2sraw=00000003`, `eofdes=3fcbb280`, `raw=00000000`,
`fifo=0010000a`, `act=1 pend=0 done=1`. TX was healthy throughout in both.

**Fix: unplug USB, wait ~10 s, replug.** `esptool`'s RTS reset — and the board's RESET
button — never drop power to the ES8311 or the AXP, so the codec stays latched. After a
true cold start the same binary ran clean immediately.

Note this does **not** revive the retracted "every audio test must start from a cold boot"
rule further up this document. That claim was about *reproducibility of results* and it
remains disproven. This is narrower: one specific latched peripheral state that a warm
reset cannot clear.

**Method failure worth not repeating.** The stall began right after a meter patch, so it
was blamed on that patch and two "fixes" were built for it. Both were wasted. The register
evidence was in the *first* failing log:

1. Decode `i2sraw` bit2 and `eofdes` **before** forming any theory — together they say
   outright whether the peripheral ever received anything. `RX_HUNG` set with `eofdes=0`
   is not a software symptom.
2. `eofdes=0` means the failure is upstream of *every* buffer, so nothing that runs after
   a buffer completes — the meter, the RX callback — can possibly be the cause. That alone
   exonerated the patch, without a build.
3. Reflashing the last-known-good binary is a near-free control. **Run it first**, not
   third. It is what finally settled this.
4. The config registers (`rxconf`, `eofnum`, `rxclkm`, `rxdiv`, `rxconf1`, `rxtdm`,
   `txtdm`) and both descriptors were **identical** between working and hung runs. When
   only *status* differs and configuration doesn't, stop reading the source.

### Patch inventory to propagate (VM-only — `repo sync` will eat these)

✅ **Exported 2026-08-03 to `board/patches/`** — `velapaw-audio-nuttx.patch` (467 lines,
md5 `9bb79ad5826e4995727b58536030c0f9`) and `velapaw-audio-apps.patch` (237 lines, md5
`e57bd955a279d44fe3308c1027dee0d7`), with apply instructions and a strip-before-shipping
list in `board/patches/README.md`. Verified by grep that `I2S_RX_TDM_CTRL_REG`,
`I2S_RX_MONO`, `REG14=0x18`, `REG16=0x20`, `nbytes &= ~3`, `streaming = true` and both
`#include <nuttx/mutex.h>` lines are present in the diff — the `#21`/`#28` comment markers
don't appear verbatim, so match on the register writes, not the marker text.

The esp-hal `LOCK_INITIALIZER_UNLOCKED` fix is **not** in these patches (its tree is
re-cloned during the build) and still needs `~/fix_vm_patches.sh`, build → fix → build.

`#9` odd `I2S_RX_EOF_NUM`; `#10` RX byte-count sum aborting on the first non-EOF
descriptor; `#11` `priv->streaming` never set (es8311 issues no `I2S_IOCTL`); `#16`
`TX_STOP_EN` gating BCLK/WS when the TX FIFO drains; `#20a` RX PCM/non-PCM WS-width
branches transposed; `#20b` RX never pulsing `I2S_RX_UPDATE`; `#21` missing
`#include <nuttx/mutex.h>`; **`#25` RX TDM slot map (the root cause)**; **`#28` `I2S_RX_MONO`**;
**`#30` es8311 gain retune**; `#32` meter peak-index + tail dump, stride `& 7` -> `% 5`
(diagnostic only — drop it once the frozen-sample question is closed).
Add all of these to `~/fix_vm_patches.sh`.
