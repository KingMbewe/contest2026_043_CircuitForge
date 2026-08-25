---
name: spi-panel-desync-debug
description: Debug a hardware-SPI LCD panel that renders nothing (or renders garbage / only partial writes) even though the SPI peripheral is provably emitting a clean, byte-perfect signal — while bit-banging the exact same pins works fine. Use when switching an ST77xx/ILI9xxx-class panel from bit-banged GPIO init to hardware SPI (SPI2/FSPI, hardware DMA, etc.) produces a blank or corrupted screen, especially if only full-screen writes work and partial-window writes don't (or vice versa).
---

# SPI panel desync debug

A hardware-SPI display bug that survived ~60 blind build-and-flash cycles on
VelaPaw before a logic analyzer cracked it in one session. The lesson generalizes:
**once the signal is proven clean, stop changing software and go looking at the
panel's own reset/CS behavior.**

## The failure pattern this covers

- Bit-banged GPIO writes to the panel work correctly.
- The *identical* command/pixel sequence sent over the hardware SPI peripheral
  renders nothing, garbage, or only certain writes (e.g. full-screen fills "work"
  but partial-window writes never appear, or the reverse).
- Every software-side theory you'll naturally reach for first — wrong SPI mode,
  wrong clock, DMA/PSRAM coherency, chip-select configuration, frequency too high
  — can all check out clean and the bug persists anyway. If you've already ruled
  those out, you're in the territory this skill covers.

## Step 1 — stop guessing, get a logic analyzer on the wire

Before trying another software permutation, prove what's actually on the pins.
Software-only theories are cheap to test but this bug survives all of them because
it isn't in the software.

A cheap 8-channel FX2 clone (~$6-10) works fine:

- On Windows: install via **Zadig**, replace the driver with **WinUSB**. It then
  enumerates as a Saleae Logic-compatible device and PulseView's "Scan for
  devices" finds it via the `fx2lafw` driver.
- If the panel's SPI pins aren't broken out on an accessible header, **mirror them
  to spare header GPIOs** with the SoC's GPIO matrix (e.g.
  `esp32s3_gpio_matrix_out(pin, signal)`) and probe those instead. Two things that
  will cost you hours if skipped:
  - **Configure the pad as `OUTPUT` before routing it through the GPIO matrix.**
    A pin left in its default mode reads dead-flat on the analyzer even though the
    matrix routing is correct — this looks exactly like "the peripheral isn't
    driving the pin," and it's tempting to go chase that instead.
  - **Ground matters.** A floating channel or a single loose ground lead shows up
    as pure noise (every sample a 1-cycle toggle), not as a recognizably-bad
    signal — don't mistake analyzer noise for a hardware fault on the panel side.
  - If your target clock is much slower than the analyzer's sample rate can
    resolve cleanly relative to the SPI clock, don't fight it in software (an API
    call to slow the clock may be silently ignored by the driver) — either sample
    faster or verify at the peripheral's actual running frequency instead of a
    throttled one.
- Trigger a known square wave first (e.g. toggle an LED/backlight pin) and confirm
  the capture shows it correctly before trusting a real capture. This is a five
  minute sanity check that saves you from debugging a suspect capture.

**What a byte-perfect capture tells you:** if the decoded MOSI/CLK/DC stream shows
your exact intended command bytes (CASET/RASET/RAMWR or equivalent), correct DC
line alignment, and clean edges with no glitches — the peripheral and your
transaction code are **innocent**. Stop looking there. The bug is downstream, in
how the panel itself is interpreting a stream it's being sent perfectly.

## Step 2 — the root cause class: CS-tied-low + pad handover

Some panel modules hardwire chip-select low (no CS pin broken out, or it's tied on
the carrier board) as a simplification for single-device SPI buses. This has one
sharp consequence: **the panel can never resynchronize its internal bit counter on
a CS edge**, because there is no CS edge — CS toggling low-to-high-to-low is
exactly what tells a normal SPI slave "the last byte ended here, restart bit
counting." Without it, the panel is trusting the clock to always be perfectly
continuous and glitch-free from the moment it powers on.

If your init sequence **bit-bangs the pins first and then hands them to the
hardware SPI peripheral mid-stream** (a common pattern: bit-bang a few early
commands, then switch to the fast peripheral for the bulk of init/pixels), that
handover glitches the clock line by roughly one edge as pad ownership changes.
On a normal (CS-edge-resyncing) panel this is invisible. On a CS-tied-low panel,
it **permanently desyncs the bit counter** — every subsequent byte decodes as
garbage, so a real RAMWR command is never recognized, no pixel data ever reaches
GRAM, and the screen just keeps showing whatever the last *successful* (bit-banged)
write painted. That's why it looks like "hardware SPI drives nothing" even though
the analyzer shows it driving a perfect signal — the panel already stopped
listening correctly before the perfect part even started.

A software reset command sent over SPI **will not fix this** — the reset command
itself gets misread by the desynced counter, same as everything else.

## Step 3 — the fix

1. Hand the pads to the SPI peripheral first (`esp32s3_configgpio(pin, OUTPUT)`
   then route via the GPIO matrix, or your platform's equivalent) — before issuing
   *any* commands on those pins.
2. Physically (hardware) reset the panel **after** the pads belong to SPI. If the
   panel's reset line is behind an I/O expander rather than a direct GPIO, drive
   it there (e.g. `IOEXP_WRITEPIN(expander, reset_pin, false)` → hold ~120 ms →
   `true` → settle ~150 ms).
3. Run the **entire** init sequence (mode set, gamma, color format, everything)
   over SPI, from the first byte. Never bit-bang those pins again once step 1 has
   happened — any fallback to bit-bang re-glitches the pads.

This works because the physical reset re-syncs the panel's counter fresh, and
because nothing touches the pins outside SPI ownership afterward, it never
desyncs again.

## A verification pitfall to avoid

Don't judge success against a full-screen fill test while you've shrunk the write
to something tiny for the analyzer's sake (e.g. 16 pixels) — a partially-working
or already-fixed state can look identical to a still-broken one at a glance if the
visible change is a handful of pixels in the corner. Verify against a fill that's
actually visible, or re-run the full-size write once the small test looks
promising.

## Once it's working: cheap perf wins

- **Clock speed was never the bug** — a note claiming "40 MHz is too fast" earlier
  in this exact debug was a misattribution of the desync; once synced, run at the
  peripheral's real rated speed (e.g. 40 MHz gave a ~4x full-repaint speedup over
  10 MHz here, with zero correctness cost).
- **Batch the address-window setup into one transfer.** If your driver issues a
  window-set (row/column address) command before every blit, batching those
  parameter bytes into a single `SPI_SNDBLOCK`-style call instead of one call per
  parameter cuts the per-blit SPI call count significantly (11 → 5 calls per
  window in this case) — this matters because a UI toolkit like LVGL sets the
  window on essentially every draw call.
- If your SPI DMA can't read your pixel buffer's memory coherently (e.g. it lives
  in PSRAM and the DMA engine only reads internal SRAM cleanly), bounce each row
  through a small internal-RAM (`.bss`) staging buffer rather than disabling DMA
  entirely.
