# Order 1 of 2 — the mechanism

Upload **all 7 files** in this folder as one JLCPCB job. Apply one ¥50 3D-printing
voucher here; the other goes on `../order2_structure`.

**Print this order in MJF PA12 nylon.** This folder holds every printed-to-printed
fit in the model (see below), and those fits want MJF's ±0.3 mm accuracy. The
229 cm³ here is the part of the job worth paying the nylon price for — the bulky,
fit-free structure goes to cheap FDM in `../order2_structure`. Est. **~¥120–180
after the voucher**. (The parts *will* assemble on FDM too if you must — every fit
is sized for ±0.5 — but MJF is the better mechanism.)

| File | Qty | Vol cm³ |
|---|---|---|
| `velapaw_rotor_x1.stl` | 1 | 21.0 |
| `velapaw_housing_x1.stl` | 1 | 17.5 |
| `velapaw_plate_stepper_x1.stl` | 1 | 15.6 |
| `velapaw_plate_idle_x1.stl` | 1 | 16.3 |
| `velapaw_hopper_x1.stl` | 1 | 49.2 |
| `velapaw_chute_x1.stl` | 1 | 22.6 |
| `velapaw_support_x2.stl` | **2** | 43.2 ea |
| **7 files, 8 pieces** | | **229 cm³** |

`support` is the only quantity-2 part — the drum sits in an A-frame at each end,
and one A-frame means the machine cannot be assembled. Set the quantity at the
cart; the filename is a reminder, not something JLC reads.

## Why these seven are together

**Every printed-to-printed fit in the whole model is inside this folder.** That
is the reason for the split line, not volume:

```
rotor  --running fit-->  housing        (0.8mm on radius, has to keep turning)
rotor  --stub socket-->  plate_idle
plates --bolted-------->  housing        (8x M3)
hopper --saddle-------->  housing OD  + post scallops
support--saddle-------->  plate OD       (SAD_R = PLATE_R + 1)
chute  --sits under---->  housing
```

Parts printed in one batch share their systematic error — same machine, same
flow calibration, same spool — so they miss in the *same direction*. Split
across two orders they can miss in opposite directions, which is exactly the
case every clearance in this model was sized against. Keeping the cluster
together doesn't make the fits legal; it makes them comfortable.

Nothing in `order2_structure` fits anything here except through a bolt hole, so
that folder can be printed on a different machine, in a different material, on a
different day, with no consequence.

See `../ORDER.md` for process settings, verification results and fasteners.
