# VelaPaw — feeder hardware

Gravity hopper → **paddle rotor** → chute → bowl, driven by a **28BYJ-48 stepper** (via a ULN2003 board), with **IN4 on GPIO43**.

## Why a rotor and not a flap

A flap's dose depends on how long it stays open **and** how full the hopper is — the same 800 ms drops noticeably more food when the hopper is heavy. A rotor pocket holds a fixed volume, so the dose depends only on how many pockets you index. Since the product claim is *per-pet portion control*, a mechanism that drifts as the hopper empties would quietly undermine it.

The rotor is driven by a **28BYJ-48 stepper** (via a ULN2003 board), which rotates **continuously in one direction** — so pockets ratchet inlet→outlet and per-pocket dosing is exact:

```
grams = pockets × GRAMS_PER_POCKET
```

> **Why a stepper, not a servo.** An MG90S servo can only *oscillate*, and with the inlet and outlet 180° apart a zero-net-rotation wobble can never carry a pocket across — it dispenses the initial charge then dead-heads. The stepper's continuous spin is what makes the rotary valve actually work, and it makes the portion-control claim literally true. (The geared 28BYJ-48 also has strong low-speed torque, which resists kibble jams.)

## Bill of materials

| Item | Notes |
|---|---|
| **28BYJ-48 stepper + ULN2003 driver** (5 V bundle) | The **5 V** version — runs off the same 5 V supply below. Get the motor+driver bundle. |
| **5 V supply, ≥2 A** | Powers the ULN2003/motor. Common **ground** with the ESP32; never tie the 5 V rails together. |
| Female–female jumpers | 4 signal lines (IN1–IN4) + 5 V + GND to the ULN2003. |
| PLA / PETG (or JLC nylon) | PLA is fine for dry kibble; PETG/nylon if you want to wash the parts. |

The **ULN2003 board is the driver** — no separate PWM chip; the ESP32-S3 just toggles 4 GPIOs through the half-step sequence. (No servo → the 470 µF servo-inrush cap is no longer needed.)

## Wiring

```
  ESP32-S3 (3.3 V logic)            ULN2003 driver board          28BYJ-48
    GPIO9  ─────────────────────────> IN1
    GPIO10 ─────────────────────────> IN2                         (5-pin
    GPIO11 ─────────────────────────> IN3            motor  ────►  connector)
    GPIO43 ─────────────────────────> IN4
                                      +   ◄── 5 V PSU +
                                      −   ◄──┬─ 5 V PSU −
  board GND ───────────────────────────────┘   ← COMMON GROUND, mandatory
```

- **IN1–IN3 = GPIO9/10/11** (header J8 pins 14/12/10), shared with the unused microSD slot (its CS is on expander EXIO3 and stays deasserted, so it's safe to drive). **Never put these on GPIO12/13/14/15/16** — those are the ES8311 audio codec's private I²S bus, not routed to any header at all, and VelaPaw now uses them for voice-settable meal schedules (onboard mic + speaker). Putting the stepper there would silently short-circuit the coil drive against the audio path — this used to be write-up guidance from before audio bring-up and is now out of date.
- **IN4 = GPIO43** (U0TXD). This works because the console runs on the **USB-Serial/JTAG** port, not UART0, so U0TXD is free to drive. **Use GPIO43, not GPIO44** — GPIO44 (U0RXD) is clamped and the coil won't drive cleanly (this was the cause of the early "stepper buzzes but won't turn" during bring-up; it was the pin, not the coil order).
- Change any of these in the board file (`STEP_IN1..4`) if you ever need UART0.
- **Power the ULN2003 from the separate 5 V supply, not the board**, and share **ground only**. The 28BYJ-48 draws ~240 mA/phase — fine for a 2 A supply.
- The ULN2003's on-board LEDs show the coil sequence — handy for confirming it's stepping.

## Printing

All parts print without supports. PLA/PETG, 0.2 mm layers, 3 perimeters.

```bash
openscad -D 'part="rotor"'         -o rotor.stl         velapaw_feeder.scad
openscad -D 'part="housing"'       -o housing.stl       velapaw_feeder.scad
openscad -D 'part="plate_stepper"' -o plate_stepper.stl velapaw_feeder.scad   # drive end (28BYJ-48)
openscad -D 'part="plate_idle"'    -o plate_idle.stl    velapaw_feeder.scad
openscad -D 'part="hopper"'        -o hopper.stl        velapaw_feeder.scad
openscad -D 'part="cam_pod"'       -o cam_pod.stl       velapaw_feeder.scad   # relocated OV5640 enclosure
openscad -D 'part="cam_mast"'      -o cam_mast.stl      velapaw_feeder.scad   # drum-front camera yoke
```

`plate_servo` still exists as legacy reference but is **not** printed — the drive
end is the stepper (`plate_stepper`). The pre-batched print/order set for the
structural parts (base, pedestal, chute, cradle, cam pod + mast) lives in
[`stl_jlcpcb/`](stl_jlcpcb/) with an [`ORDER.md`](stl_jlcpcb/ORDER.md).

Open the file with no `-D` (`part="product"`) to preview the whole assembly.

## Board cradle (mounting the ESP32-S3-Touch-LCD-3.5-C)

`board_cradle` holds the Waveshare board so the **screen faces the owner** (the
board is now un-rotated — the front-eye layout). The board drops into the tray and
is fixed with four **M2.00** self-tapping screws through its corner mounting holes,
cutting into the posts' printed pilot holes; the bottom GPIO header (and the
stepper **IN4 wire on GPIO43**) exits through the header slot.

The OV5640 **no longer looks down through a window in the cradle** — it has been
**decoupled from the board and relocated** (see *Camera relocation* below), so the
screen can face the owner while the camera watches the bowl from its own mount.

Dimensions are taken from the Waveshare -C outline drawing (95.11 × 63.67 × 14.1 mm,
corner R7.3, mounting holes 72.00 × 48.50 mm apart, camera centred). `BM_SCREW_D`
(a 1.7 mm self-tap pilot bore for **M2.00** screws — the bare-board drawing labels
the mounting holes M2.00, not M2.5 as earlier assumed) is confirmed from the
drawing. **One value is still not on the drawing and remains assumed — verify on
the real board before printing:** `BM_CAM_WIN` (camera lens-barrel diameter,
assumed Ø14). Both constants are near the bottom of the `.scad`.

> ### Stepper mechanical parts — done (verify two dimensions)
> The drive end is now built for the **28BYJ-48**: print **`plate_stepper`** (its
> Ø28 body clears the central bore; it bolts on through the two 35 mm flange
> holes) — *not* `plate_servo`, which is kept only as legacy reference. The
> **rotor** now has a **5 mm double-D socket** in its driven face that presses
> straight onto the stepper shaft (no horn/spline). Body Ø is confirmed 28 mm;
> two values are nominal — **caliper before printing** and adjust at the top of
> the `.scad`: `STEP_SHAFT_FLAT` (shaft width across the flats) and
> `STEP_SHAFT_LEN` (shaft length). `housing`, `plate_idle`, `hopper` and
> `board_cradle` are unchanged.

## Camera relocation (OV5640 on a 30 cm FFC)

The OV5640 plugs into the board's 24-pin 0.5 mm DVP connector (schematic **J2**,
"0.5-24pin-cam"). Unplugging it and running a **30 cm FFC + a 1:1 coupler** lets
the camera live on its own mount instead of riding the board, so the screen can
face the owner while the lens watches the bowl. The camera sits in **`cam_pod`**
(open-back OV5640 enclosure) carried by **`cam_mast`** — a two-leg yoke that
straddles the chute and holds the pod just in front of the drum, aimed steeply
down-and-forward at the pet (the sightline passes *under* the owner-facing
screen's bottom edge). `cam_mast` bolts to `base_front` with 2× M3.

> **⚠️ A reversed FFC is FATAL on first power-on** — board pin *N* would meet
> camera pin *25−N*, dead-shorting a power rail into a data/ground pin. **Verify
> orientation with a meter before powering.** Verified-good chain (2026-07-30):
>
> ```
> camera pin 1 → pigtail "1" → coupler P1.1 (left) → P2.1 (left)
>              → 30 cm cable pin-1 (dotted edge) → J2 pin 1 (BOTTOM end of J2)
> ```
>
> Key facts for this module: the **coupler is a straight 1:1 extender** (silk
> `1`-left / `24`-right on both ports), **J2 pin 1 = the bottom end** (board silk
> prints `24` at the top), and the camera's **grounds are not bonded internally**
> — so you can't confirm orientation by probing *through* the camera; trust the
> silkscreen `1`/`24` marks and bare-copper (`000.x`) continuity only. Keep the
> ~30 cm run in a small **service loop** at the yoke foot for strain relief, and
> route it away from the stepper leads to keep noise off the DVP clock/data lines.

## Assembly

1. Mount the **28BYJ-48** to `plate_stepper` (flange screws).
2. Press the rotor's **D-socket** onto the stepper shaft.
3. Drop the rotor into the `housing` bore.
4. Fit `plate_idle` on the far end — its boss is the rotor's stub axle.
5. Bolt both plates to the housing (4 × M3).
6. Plug the `hopper` spigot into the inlet; put a bowl under the outlet.

## Calibration — do this once

1. Fill the hopper.
2. Dispense a single pocket.
3. Weigh what lands in the bowl.
4. Put that number in `GRAMS_PER_POCKET` in `board/esp32s3_st7789.c` and rebuild.

Portions are then correct at any hopper level, for the life of the machine. If you want a different dose granularity, change `WIDTH` in the `.scad` (pocket volume scales linearly with it) and re-calibrate.

`POCKETS` in the `.scad` must always equal `ROTOR_POCKETS` in the board file.
