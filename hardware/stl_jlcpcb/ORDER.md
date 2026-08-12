# VelaPaw feeder — JLCPCB 3D printing order

Generated from `../velapaw_feeder.scad`. Do **not** hand-edit these STLs; change
the `.scad` and re-export, or the two will drift apart.

## Order settings

| Field | Value |
|---|---|
| Process | **MJF or FDM** — this set assembles at either (SLS substitutes for MJF) |
| Material | **PA12 nylon** (MJF) / **PLA or PETG** (FDM) |
| Finish | as-printed / bead-blast natural grey (dyeing optional, purely cosmetic) |
| Tolerance | ±0.3 mm (MJF) or ±0.5 mm (FDM) — every fit is sized for the worse of the two |
| Units | **millimetres** |

**Either process works, and that is deliberate.** The general fit clearance is
`FIT = 0.6` per side, which already exceeds FDM's ±0.5 requirement, so the
mechanism assembles on a filament machine without touching the model. The one
fit that needed re-solving for FDM is the board pocket — see *The board pocket*
below. For a demo build FDM is roughly a fifth of the MJF price; MJF is the
better part.

**Recommended material split — MJF `order1_mechanism`, FDM `order2_structure`.**
The two orders are already divided along the only line that matters for this
choice: **`order1` holds every printed-to-printed fit, `order2` holds none**
(its parts touch the mechanism only through M3/M4 bolt holes). So print the
229 cm³ mechanism in **MJF PA12**, where dimensional accuracy earns its keep on
the rotor/housing/plate fits, and print the 403 cm³ of structure — a flat base
plate and brackets that fit nothing precisely — in cheap **FDM**. That buys
accuracy only where it's needed and pays filament prices for the 64 % of the
volume that is bulk. This is optional (both orders print either way), but it is
the cost-optimal way to order and the two folders were split to make it clean.
See *Material volume* for the per-order cost.

## Parts — 13 files, 14 pieces, split across TWO orders

JLC allow **one coupon per order**, and there are two ¥50 3D-printing vouchers
(`openvela`, no minimum, expiring 2026-10-31). Two orders redeem both. The files
are already sorted into the folders to upload — and each folder is also zipped
(`order1_mechanism.zip`, `order2_structure.zip`, STLs only) so you can **drag one
zip per JLC job** instead of selecting files. Re-zip if you re-export any STL.

### `order1_mechanism/` — 7 files, 8 pieces, 229 cm³

| File | Qty | Size (mm) |
|---|---|---|
| `velapaw_rotor_x1.stl` | 1 | 50.3 × 56.4 × 38 |
| `velapaw_housing_x1.stl` | 1 | 64 × 64 × 39.2 |
| `velapaw_plate_stepper_x1.stl` | 1 | 72 × 72 × 4 |
| `velapaw_plate_idle_x1.stl` | 1 | 72 × 72 × 11 |
| `velapaw_hopper_x1.stl` | 1 | 96 × 96 × 80.7 |
| `velapaw_chute_x1.stl` | 1 | 46 × 108 × 92.5 |
| `velapaw_support_x2.stl` | **2** | 90 × 167 × 16 |

### `order2_structure/` — 6 files, 6 pieces, 403 cm³

| File | Qty | Size (mm) |
|---|---|---|
| `velapaw_base_front_x1.stl` | 1 | 164 × 176 × 15 |
| `velapaw_base_back_x1.stl` | 1 | 164 × 92 × 15 |
| `velapaw_pedestal_x1.stl` | 1 | 110 × 88 × 102 |
| `velapaw_board_cradle_stand_x1.stl` | 1 | 100.7 × 81.4 × 81.0 |
| `velapaw_cam_pod_x1.stl` | 1 | 35 × 20 × 8 |
| `velapaw_cam_mast_x1.stl` | 1 | 78 × 23 × 123 |

The screen was **turned to face the owner**, which killed the old over-the-top
camera path (the display now sits between any behind-the-bowl lens and the pet).
So the camera was **decoupled from the pedestal**: the pedestal reverts to its
plain form (46 cm³, was 102 with the gantry), and the OV5640 now rides its own
**`cam_mast`** — a two-leg **yoke** that bolts to `base_front` and stands
**straddling the chute**, carrying the pod **centred just in front of the drum**
where it aims steeply down; its sightline passes *under* the owner-facing screen's
bottom edge to reach the pet. `cam_pod` is the small open-back enclosure that holds
the bare module and bolts to the yoke's central pad with 2× M3. All ride
`order2_structure` because every joint here is an M3 clearance — machine
independent. See *The relocated camera* below. **Pod
pocket is built to the wide-angle MAX OV5640 envelope; if the real module is
calipered, tightening `POD_PCB` re-exports both the pod and the mast (the pad
hole spacing tracks the pod outline).**

`support` is the **only** part with quantity 2 — the drum sits in an A-frame at
each end. The quantity is in the filename because it is the single easiest thing
to get wrong at the cart, and a missing A-frame means the machine cannot be
assembled at all.

### Where the split line was drawn

**Not by volume — by fits.** Every printed-to-printed fit in the model is inside
`order1_mechanism`: the rotor's running fit in the housing, its stub socket in
the idle plate, the hopper's saddle over the housing OD and its post scallops,
the A-frames' saddles over the plate ODs, the chute under the drum. Parts printed
in one batch share their systematic error — same machine, same flow calibration,
same spool — so they miss in the *same* direction. Split across two orders they
can miss in opposite directions, which is the worst corner every clearance was
sized against. Keeping the cluster together doesn't make the fits legal; it makes
them comfortable.

Nothing in `order2_structure` touches the mechanism except through an M3 or M4
clearance hole, which does not care what machine its mate came off. That folder
can be printed on a different machine, in a different material, on a different
day, with no consequence.

Both orders clear ¥50 of value at the bottom of the FDM range, so neither
voucher is wasted. **Do check the second shipping fee at checkout** — it is the
one cost that splitting adds, and it comes straight off the ¥50 gained.

To order everything as a single job instead, upload both folders together — but
that redeems only one ¥50.

## Deliberately NOT in this folder

* **`board_cradle`** — the plain cradle. It is an *alternative* to
  `board_cradle_stand`, not an addition: the stand version contains the cradle.
  Ordering both wastes money and yields a part with nowhere to go. The stand
  version is the one the finished appliance uses.
* **`plate_servo`** — legacy MG90S mount, superseded by the stepper. Unused.
* **`stand`, `product`, `all`** — preview assemblies, not parts. `stand` is
  268 mm and exists only to show how the five stand pieces bolt together.

## The board pocket

`board_cradle_stand` holds the ESP32-S3-Touch-LCD-3.5-C in a pocket, and that
pocket is the **only fit in the model that had to be re-solved for FDM**. It is
also the only one where a bad outcome is not recoverable: everything else can be
reamed, filed or shimmed, but if this comes out tight the display does not seat
and the demo has no screen.

The pocket is 95.11 × 63.67 mm — the largest feature in the model, so it sees
the full tolerance swing. At the old 0.6 mm/side (sized for MJF's ±0.3) an FDM
pocket printed to the tight end of ±0.5 would have left **0.1 mm**. It is now
**1.0 mm/side**, which leaves 0.5 mm at the same bad corner.

Opening a pocket costs grip, because the retention lips start at the pocket wall
and retreat with it. `BM_LIP_W` went 2.0 → 2.8 to pay that back, restoring the
designed 1.8 mm of grip nominal / 0.8 mm with the board shoved fully to one side.
Two supporting changes came with it: the pocket wall went 2.4 → 2.8 nominal so
the *printed* wall stays at 1.8 mm (1.3 mm worst case) instead of dropping under
JLCPCB's 1.2 mm FDM minimum, and the lip's retaining face went 1.2 → 1.6 mm to
get off that minimum, since it is the face that takes the whole pull-out load.

Verified by solid-geometry probe, not by arithmetic alone:

| Probe | Expected | Result |
|---|---|---|
| case seats in pocket, nominal | empty intersection | clear |
| case seats with pocket 0.3 mm undersize (MJF worst case) | empty | clear |
| case seats with pocket 0.5 mm undersize (**FDM worst case**) | empty | clear |
| far lip still over the board, board shoved 1.0 mm away | **non**-empty | material present |
| enlarged cradle vs. every neighbour in the assembly | 13 separate bodies | 13 |

Net cost: the part grew 0.8 mm in each outer dimension and 3 cm³ in volume.
Nothing is competing for that space.

## The relocated camera

The OV5640 was unplugged from the board's 24-pin DVP connector and moved onto a
30 cm FFC so it can look at the bowl instead of riding the screen. **The screen
now faces the owner** (the board is un-rotated on its wedge), which means a
camera behind or above the bowl would have to shoot *through* the display — so
the camera is decoupled onto its own bracket. Two printed parts carry it:

* **`cam_pod`** — a small open-back enclosure for the bare module. Built to the
  **wide-angle MAX OV5640 envelope** (14 mm square PCB, 15 mm barrel aperture)
  because the exact module was not calipered; a smaller real part just sits
  looser and is taken up by a bead of silicone from the open back. Nothing rigid
  touches the barrel — it may be an auto-focus VCM element that jams if pressed.
  The lens tip stands proud of the front plate, so even a 120° cone never
  vignettes. Two Ø3.4 ears bolt it to the yoke pad.
* **`cam_mast`** — a two-leg **yoke** (78 × 23 × 123 mm, ~36 cm³) that bolts to
  `base_front` with **2× M3** and stands **centred, straddling the chute**: the
  feet are at X ±34 (the trough is only X ±23), so the legs rise vertically
  *outboard* of the chute, then knee inward above the rails and each arm lands on
  its own ear of a tilted pad. That pad, built in the pod's own frame, carries the
  pod's two ears **just in front of the drum**, and is cut with a central
  **window** so the pod's open back stays open — that is how the OV5640 seats
  (drop the PCB in, silicone bead from behind) and how the FFC tail routes up past
  the pad. It is a **separate bolt-on part**, so the pedestal reverted to its
  plain 46 cm³ form.

The pod sits central (X = 0), so the aim is a **pure steep down-tilt**: the pod
swings 180° about Z and tilts 68.3° about X to lay its lens onto the pet-face
target. The key trick is occlusion — a level camera here would shoot into the
back of the owner-facing screen, but from this low eye (z ≈ −13) the steep ray
drops below the screen's z = −20 bottom edge before reaching it and lands on the
pet over the bowl. Validated in the assembly by CGAL: pod, yoke and the aim
sightline each clear the board, hopper, drum, bowl, chute and pedestal (empty
intersections), and `cam_mast` is one closed solid (`Simple: yes`, one volume).

**Caution — orientation before power.** A reversed FFC is fatal on first power:
board pin N mates camera pin 25−N, which puts a 1.5 V rail onto the camera's
ground and 2.8 V onto data lines. Before powering, buzz chassis ground to the
coupler's far contacts and confirm ground lands on the low-numbered (pin-2) end;
flip the cable if it shows at the pin-23/24 end.

**Caliper note.** If the real module is measured, tightening `POD_PCB` in the
`.scad` re-exports **both** `cam_pod` and `cam_mast` — the yoke pad's hole
spacing tracks the pod outline (`EAR_X = POD_OUT/2 + POD_EAR_L/2`). Re-run the
two exports and the numbers above will shift slightly.

## Pre-upload verification (measured on THESE files, not on the source)

Run `verify_stl.py` and `bedcontact.py` against **each order folder** to
reproduce. Both folders were re-verified after the split — the checks below are
the union of the two runs, and nothing changed in the move.

**Watertight — all 13 pass all four conditions.** "Watertight" is four separate
things, and a mesh can pass the naive check and still be rejected. The order2
files touched by the front-camera pivot (`base_front` gained 2 yoke-foot holes,
`pedestal` reverted, `cam_pod` unchanged, `cam_mast` the drum-front yoke) were re-verified by
CGAL (`Simple: yes`, one closed volume each) and z=0 seating:

| Condition | Result |
|---|---|
| every edge shared by exactly 2 triangles (no holes/open edges) | 0 open edges |
| consistent winding — each directed edge used once | 0 bad |
| signed volume > 0 (normals point **out**, not in) | all positive |
| exactly 1 connected component per file | all 1 |
| zero-area/degenerate triangles | 0 |

**Orientation — all 13 sit on z = 0.000 on a real FACE**, not an edge or a
vertex. First-layer contact area, which is the check that actually matters:

| Part | bed contact | height |
|---|---|---|
| base_front | 28738 mm² (100% of footprint) | 15 |
| base_back | 14835 mm² (98%) | 15 |
| plate_idle | 4028 mm² (78%) | 11 |
| plate_stepper | 3904 mm² (75%) | 4 |
| support | 4652 mm² (31%) | 16 |
| rotor | 549 mm² (19%) | 38 |
| cam_pod | 208 mm² (front plate, 54%) | 8 |
| cam_mast | 240 mm² (2 feet, 100%) | 123 |
| housing | 629 mm² (15%) | 39.2 |
| hopper | 886 mm² (10%) | 80.7 |
| chute | 430 mm² (9%) | 92.5 |
| pedestal | 709 mm² | 102 |
| board_cradle_stand | 388 mm² (5%) | 81.0 |

The **pedestal is back to 102 mm tall** — the camera gantry is gone, so the post
reverts to its plain form and lands on the same 709 mm² of foot pads at z=0.
`cam_mast` is a 123 mm two-leg yoke that stands on its two 10 × 12 feet (both flat
on z=0); on FDM the inward-leaning struts and the tilted pad at the top want
support, on MJF it is a non-issue.

The low-ratio parts are tall and open by design; every one still lands on a flat
face. **On MJF none of this is binding** — it is a powder bed, JLC nest the parts
themselves, and there is no bed adhesion to lose. The poses are submitted correct
anyway because they make JLC's preview legible and they are what proves the set
could be made on a filament machine if it ever has to be.

## Material volume — read this before ordering

**632 cm³ total, ~0.63 kg of PA12** (includes support ×2). MJF is quoted mostly
by volume, so this is the number that sets the price. (Was 651 with the pedestal
gantry; the front-camera pivot dropped the gantry −55.5 cm³ off the pedestal and
added the `cam_mast` yoke +35.9 cm³, a net −20.)

Nearly half of it is still the base plate:

| | cm³ | share |
|---|---|---|
| base_front | 178.8 | 28% |
| base_back | 94.1 | 15% |
| **base plate subtotal** | **272.9** | **43%** |
| everything else (11 parts, 12 pieces) | 359.1 | 57% |

By order: `order1_mechanism` 229 cm³ (36%), `order2_structure` 403 cm³ (64%).
The structure order is the bulkier one, and it carries the camera as well:
base plate 273, pedestal 46, board cradle 45, pod 2, yoke 36.

**Per-order cost, using the recommended MJF/FDM split** (estimates in ¥; JLC
rates and region shift these, so treat as order-of-magnitude, and the 5-min
voucher check at the cart is what confirms them):

| Order | Material | Volume | Est. before voucher | After −¥50 |
|---|---|---|---|---|
| `order1_mechanism` | **MJF PA12** | 229 cm³ | ~¥170–230 | ~¥120–180 |
| `order2_structure` | **FDM PLA/PETG** | 403 cm³ | ~¥90–120 | ~¥40–70 |
| **both** | | 632 cm³ | | **≈ ¥160–250** |

For comparison: **all-FDM** ≈ ¥60–110 (weak fits on the mechanism), **all-MJF**
≈ ¥300–500 (overkill on the base plate). The split lands between the two and
puts the money where the tolerance actually matters. Watch the **second shipping
fee** — splitting adds one, and it comes off the ¥50 the second voucher gains.

That base plate is a 5 mm solid floor over 164 × 176 and 164 × 92 mm.

**DECIDED (2026-07-25): the base plate is being printed.** Substituting a cut
sheet of MDF or acrylic was considered and rejected, so the full base plate
stands (273 cm³ of the 632) and the two-order split holds as described above. Do
not re-open this at the cart.

If volume ever does have to come out of it, the thing to know is that the floor
thickness is **not** a free parameter: the M3 × 12 countersunk tower screws are
dimensioned around exactly 5 mm (csk sinks 1.4, screw protrudes 7 into a 9 mm
flange). Thinning the floor means re-solving the screw stack. Pocketing or
ribbing the floor *between* the tower feet and the bowl ring is the change that
saves volume without touching the fastener scheme.

## Sanity checks to run in JLC's uploader

1. **Scale.** `velapaw_base_front_x1` must read ~**164 × 176 × 15 mm**. STL
   carries no units; if the viewer shows inches or centimetres the whole order
   is wrong by a constant factor and every part will be useless.
2. **Watertight / manifold.** All thirteen pass locally — CGAL reports
   `Simple: yes`, one closed volume, zero bad edges. If JLC's checker disagrees,
   suspect the upload, not the geometry.
3. **No trapped powder.** No part has an internal void, so there is nothing for
   un-sintered powder to be sealed inside. The deepest blind holes are the four
   ⌀1.7 × 12 pilots down the cradle posts, which are vestigial on the -C board
   (see `BM_SCREW_D` in the .scad) — they may be ignored or filled.

## Fasteners — buy separately, NOT printed

| Item | Qty | Where |
|---|---|---|
| M3 × 12 countersunk | 12 | towers → base plate (2 per foot, 6 feet) |
| M3 × 22 socket cap | 2 | base_front → base_back splice |
| M3 (plates → housing) | 8 | end plates onto the drum |
| M4 | 2 | 28BYJ-48 stepper flange |
| M3 × 8 self-tapping | 2 | camera pod → yoke pad (through Ø3.4 ears into Ø2.6 pilots) |
| M3 × 12 | 2 | camera yoke feet → base_front (Ø3.4 clearance, nut under the plate) |

## After delivery

The 28BYJ-48 D-shaft socket in the rotor is the one fit deliberately held
tighter than the MJF clearance rule, because it is the only joint transmitting
torque and slop there means mis-dosing. At the tight end of the tolerance it
will need a 5 mm drill run through by hand and a light file on the two flats.
Budget 30 seconds for it; it is intentional, not a defect.
