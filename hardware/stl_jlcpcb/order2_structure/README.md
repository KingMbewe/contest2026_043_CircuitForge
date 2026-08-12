# Order 2 of 2 — the structure

Upload **all 6 files** in this folder as one JLCPCB job. Apply the second ¥50
3D-printing voucher here; the first goes on `../order1_mechanism`.

**Print this order in FDM (PLA/PETG).** Nothing here has a printed-to-printed fit
— every interface is an M3/M4 clearance hole (see *Why these six are together*),
so FDM's looser ±0.5 mm costs nothing, and this is 403 cm³ (64 % of the whole
job), most of it a flat base plate. Paying MJF nylon prices for a plate that fits
nothing would be wasted money; the precision mechanism is what goes to MJF in
`../order1_mechanism`. Est. **~¥40–70 after the voucher**. (The one fit that was
re-solved for FDM, the board pocket, lives here and is already at 1.0 mm/side.)

| File | Qty | Vol cm³ |
|---|---|---|
| `velapaw_base_front_x1.stl` | 1 | 178.8 |
| `velapaw_base_back_x1.stl` | 1 | 94.1 |
| `velapaw_pedestal_x1.stl` | 1 | 46.3 |
| `velapaw_board_cradle_stand_x1.stl` | 1 | 45.5 |
| `velapaw_cam_pod_x1.stl` | 1 | 2.2 |
| `velapaw_cam_mast_x1.stl` | 1 | 35.9 |
| **6 files, 6 pieces** | | **402.8 cm³** |

The screen was **turned to face the owner**, so the camera can no longer look
over the top of it. It was **decoupled onto its own bracket**: the pedestal
reverted to its plain form (101.8 → 46.3 cm³, gantry removed) and the new
**`cam_mast`** is a two-leg **yoke** that bolts to `base_front` and stands
**straddling the chute** (feet at X ±34, the trough is only X ±23), carrying the
pod **centred just in front of the drum** where it aims steeply down — its
sightline passes *under* the owner-facing screen's bottom edge to reach the pet.
`base_front` gained 2 M3 holes for the yoke feet. See `../ORDER.md` → *The
relocated camera*.

## Why these six are together

They hold things up. **None of them has a printed-to-printed fit** — every
interface here is an M3 or M4 clearance hole, which does not care what machine
its mate came off. That is what makes this folder separable from
`order1_mechanism`, where all the real fits live. (The `cam_pod`→mast and
`cam_mast`→base joints are the same story: M3 through Ø3.4 clearances into pilots
or nuts — machine independent.)

`base_front` + `base_back` are two tiles of one plate, spliced with 2× M3 × 22.
They are two pieces only because the plate is longer than a 220mm bed.

## This folder is where the money is

**403 of the project's 632 cm³ — 64%.** And 273 cm³ of that (43% of the whole
job) is `base_front` + `base_back`: a flat 5mm plate with fourteen M3 holes.

**DECIDED (2026-07-25): it gets printed.** Substituting a cut sheet of MDF or
acrylic was considered and rejected, so upload all six files. This note exists
so nobody re-opens the question at the cart.

The floor thickness is **not** a free parameter if volume ever has to come out:
the M3 × 12 countersunk tower screws are dimensioned around exactly 5mm (csk
sinks 1.4, screw protrudes 7 into a 9mm flange). Thinning the floor means
re-solving the screw stack. Pocketing the floor *between* the tower feet and the
bowl ring is the change that saves volume without touching the fastener scheme.

See `../ORDER.md` for process settings, verification results and fasteners.
