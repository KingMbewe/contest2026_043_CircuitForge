# VelaPaw — Cat Body Condition Score (BCS) labeling guide

We train a 3-class estimator: **under / ideal / over**. This collapses the
standard APOP/Purina 9-point scale:

| 9-point BCS | Our class | What you see |
|---|---|---|
| 1–3 | **under**  | Ribs, spine, hip bones visible; severe waist tuck; no fat over ribs |
| 4–5 | **ideal**  | Slight waist behind ribs; ribs felt with light fat cover; minimal belly |
| 6–9 | **over**   | No waist (oval from above); rounded/sagging belly fat pad; ribs hard to find |

## Folders
```
bcs_dataset/
  ideal/   <- auto-filled from Oxford-IIIT (healthy pet photos)
  under/   <- YOU collect ~200
  over/    <- YOU collect ~200
```

## How many
Aim for **~200 images per class** (balanced). Ideal is already ~190; top it up
toward ~200 if you like. The two you collect are what matter.

## Where to find under / over
- **Over:** search "obese cat", "fat cat", pet-obesity awareness sites/charts.
- **Under:** "underweight cat", "emaciated cat", "thin stray cat", shelter
  intake galleries, rescue before/after photos.

## What makes a GOOD training image
- **Side or standing full-body view** (BCS reads from the side + top). Skip
  face-only or curled-up-ball photos — the body shape must be visible.
- **One cat**, clearly the subject, reasonably in focus.
- Any lighting/background is fine (variety helps).

## Avoid these traps (breed/fur confound BCS)
- **Sphynx** looks "thin" (no fur) but can be any condition — don't auto-label under.
- **Persian / Maine Coon / Ragdoll** look round/big from fluff or frame, not fat
  — judge body shape, not fur.
- **Mix breeds across all three classes** so the model learns *condition*, not breed.
- When unsure between two classes, prefer **ideal** (conservative) or drop the image.

## After collecting
Run the prep script (resize, dedupe, 80/20 train-val split, augmentation), then
the training/export script to produce the INT8 TFLite model. The result is an
*estimate* — the app will show it with an owner-confirm + "check with your vet".
