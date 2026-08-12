// ============================================================================
// VelaPaw - paddle-rotor feeder  (contest 2026, team 043)
//
//   hopper  ->  rotor housing  ->  chute  ->  bowl
//
// The rotor is a vaned wheel turning inside a round housing under the hopper.
// Each vane pocket holds a FIXED VOLUME, so the dose depends on how many
// pockets the servo indexes -- not on how long a gate is held open, and not on
// how full the hopper is. That is the whole point: a flap dispenses more when
// the hopper is heavy, which would quietly break the per-pet portion control
// the product is built on.
//
// Driven by a 28BYJ-48 STEPPER (via a ULN2003 board), NOT a servo. A stepper
// rotates CONTINUOUSLY in one direction, so pockets ratchet inlet(top) ->
// outlet(bottom) without ever reversing. Each pocket = 60 degrees = one sixth
// of a turn = STEP_PER_POCKET half-steps in the firmware. So per-pocket dosing
// is exact:  grams = pockets x GRAMS_PER_POCKET  (calibrate by weighing one
// pocket's drop). Keep POCKETS here == ROTOR_POCKETS in the board file.
// (History: a 180-deg servo was tried first, but with the inlet/outlet 180 deg
//  apart its oscillation has zero net rotation and can't carry a pocket across
//  -- it dead-heads. The stepper's continuous spin is what makes the rotary
//  valve actually work and the portion-control claim literally true.)
//
// PRINTING -- JLCPCB MJF / SLS, PA12 nylon
// 12 printable parts in the dropdown, but you ORDER 11 FILES / 12 PIECES:
// board_cradle and board_cradle_stand are ALTERNATIVES (the stand contains the
// cradle -- order one, never both), and the A-frame support is the one part with
// quantity TWO. The ready-to-upload set with the quantities in the filenames is
// in stl_jlcpcb/, with ORDER.md beside it; regenerate it from this file rather
// than editing those STLs.
// Leave PRINT_POSE on and each part exports already rotated and sitting on z=0;
// the PRINT POSES block at the bottom of the file has the measured bed sizes and
// the reasoning behind each orientation. Nothing here exceeds 220 x 220 x 250.
//
// MJF was chosen over FDM for three reasons, in order of how much they matter:
//   1. IT IS A POWDER BED, SO THERE ARE NO SUPPORTS. Three parts (chute,
//      pedestal, board_cradle_stand) carry deliberate overhangs that no rotation
//      puts on a bed -- their open centres are the whole point of the design.
//      On FDM each needs support material printed into and then broken out of
//      the exact surfaces that have to stay clean. On MJF the problem does not
//      exist. The "supports?" column in PRINT POSES is FDM advice, kept because
//      it is still true if anyone reprints this on a filament machine.
//   2. TOLERANCE. +/-0.3mm instead of FDM's +/-0.5. The board pocket is 95.11mm
//      across and has to receive a purchased case; at +/-0.5 the clearance
//      needed to guarantee it seats is more than the locating wall can give.
//   3. PA12 NYLON FLEXES INSTEAD OF SNAPPING, which is what the four corner
//      snap clamps on the board mount actually want. They are 2.4mm columns
//      sprung sideways every time the board comes out; in PLA that is a fatigue
//      crack waiting to happen.
// The cost is that every fit between two printed parts needs 0.6mm per side --
// see FIT near the top of the parameters. That is why the running clearances in
// this file are looser than they would be for a filament print.
//
// Two things to hand the vendor:
//   * Orientation is ADVISORY. JLC nest parts in the powder bed themselves, so
//     the poses below are for your own inspection, not a build instruction.
//   * No part has an internal void, so nothing traps un-sintered powder --
//     every one comes back from CGAL as a single closed volume. The only deep
//     blind holes are the four 1.7 x 12 pilots down the cradle posts, and on the
//     -C those are vestigial anyway (see BM_SCREW_D).
//
//   FEEDER MECHANISM
//   part="rotor"         - flat, hub up. No supports.
//   part="housing"       - bore vertical. No supports (34mm bridges over the
//                          inlet/outlet, which any printer handles).
//   part="plate_stepper" - flat. No supports. (28BYJ-48 mount; plate_servo is
//                          the old MG90S mount, kept only for reference.)
//   part="plate_idle"    - flat, boss up. No supports.
//   part="hopper"        - MOUTH DOWN. No supports. It is FLIPPED from the way
//                          it is modelled: the drum saddle eats the whole neck
//                          face, so neck-down there is literally nothing to
//                          start on. See PRINT POSES.
//
//   BOARD MOUNT (print one or the other, not both -- the stand contains the
//   cradle). Holds the ESP32-S3 screen-UP with the camera looking down.
//   part="board_cradle"  - flat, clamps up. No supports (45deg lead-in ramps).
//   part="board_cradle_stand" - the cradle riding a 30-degree wedge, flat feet
//                          down. SUPPORTS under the tray. THIS is the mount the
//                          finished appliance uses (it drops onto the stand's
//                          pedestal); also works as a stand-alone desk stand.
//
//   STAND -- five printed pieces that bolt together. part="stand" is a PREVIEW
//   of them assembled and is 164 x 268 x 172: do not try to print it.
//   part="base_front"    - floor down. No supports. Bowl end of the base plate.
//   part="base_back"     - floor down. No supports. Column end. Bolts to
//                          base_front at BASE_SEAM_Y with 2x M3x22 socket cap.
//   part="support"       - PRINT TWO. Lies flat on its face, 90 x 167 x 16.
//                          No supports.
//   part="chute"         - pads down. SUPPORTS under the trough.
//   part="pedestal"      - posts up. SUPPORTS under the rails and tie bar.
//   The three towers then bolt DOWN through the base plate, 2x M3x12 countersunk
//   per foot -- 6 feet, 12 screws. Join the two base tiles FIRST: the splice
//   bosses sit inside the tray and a fitted tower is in the way.
//
//   part="all"           - preview of the assembly (do NOT print this).
//
// AFTER PRINTING - calibrate once:
//   Fill the hopper, dispense a single pocket, weigh what lands in the bowl.
//   Put that number in GRAMS_PER_POCKET in the board file. Done -- portions are
//   then correct for the rest of the machine's life, at any hopper level.
// ============================================================================

/* [What to show] */
// pick a part to print, or "all" to preview the assembly
part = "product"; // [product:full appliance + stand + camera aim (do NOT print), all:feeder mechanism only, rotor, housing, plate_stepper, plate_idle, hopper, board_cradle, board_cradle_stand, stand:assembled stand preview (do NOT print), base_front, base_back, support:print TWO, chute, pedestal]

// export printable parts already rotated into their slicing orientation and
// sitting on z=0. Turn OFF to see a part where it lives in the machine instead.
// See the PRINT POSES block at the bottom of the file. Previews always ignore it.
PRINT_POSE = true;   // [true, false]

/* [Hidden] */

// ---- PROCESS: JLCPCB MJF / SLS, PA12 nylon ---------------------------------
// FIT is the per-side gap on every place where one PRINTED part has to go into
// or turn inside another printed part. Its size is NOT about the parts touching
// in the powder bed -- they are printed as separate solids and never touch. It
// is about TOLERANCE: MJF holds +/-0.3mm, and on a fit BOTH halves can miss in
// the direction that closes the gap. A nominal 0.3mm clearance is therefore a
// 0.3mm INTERFERENCE at the corner of the tolerance box, and the parts simply
// do not go together. 0.6 per side is the smallest number that still assembles
// when everything goes wrong at once, and is what JLCPCB publishes for MJF/SLS.
//
// Every fit below is written against FIT so they cannot drift apart. Changing
// process is then one line: FDM/SLA want 0.5, SLM/binder-jet 1.0.
//
// THIS MUST STAY ABOVE THE GEOMETRY BLOCK. OpenSCAD does not hoist top-level
// assignments -- a variable read before the line that sets it is undef, and it
// only WARNS. Defined below ROTOR_R, this silently exported a rotor with an
// undefined radius.
//
// TWO fits deliberately do NOT use it, and both are noted where they occur:
//   * the D-shaft socket -- it grips a PURCHASED steel shaft, and only the
//     printed half varies. 0.6 per side there would be 1.2mm of backlash in the
//     one joint that transmits torque.
//   * fastener clearance holes -- 0.2 per side is standard and an M3 still
//     passes a 3.6 hole that came out 0.3 small. Not a fit between two forms.
FIT = 0.6;

// ---- geometry --------------------------------------------------------------
POCKETS      = 6;      // vanes; MUST match ROTOR_POCKETS in esp32s3_st7789.c
BORE_R       = 29;     // housing inner radius
ROTOR_R      = BORE_R - (FIT + 0.2);   // 28.2 -- 0.8mm running clearance.
                       // FIT alone (0.6) is the assembly minimum, and this is a
                       // journal that has to keep TURNING at the worst corner of
                       // the tolerance box, not just go together once, so it gets
                       // 0.2 more. Costs ~2% of pocket volume, which is free:
                       // GRAMS_PER_POCKET is calibrated by weighing, not by this
                       // number. Still far below kibble size, so nothing leaks.
HUB_R        = 8;      // rotor hub radius
WIDTH        = 38;     // rotor axial width  -> pocket volume
VANE_T       = 3;      // vane thickness
WALL         = 3;      // housing wall
CLEAR        = 0.4;    // general print clearance (flow gaps, reliefs -- NOT fits)

// Pocket volume is bounded by the ROTOR radius (not the bore) and the vanes
// eat into it: ~ pi*(ROTOR_R^2 - HUB_R^2)/POCKETS * WIDTH  -  vane displacement
//   ~= pi*(28.2^2 - 8^2)/6 * 38  -  vane  ~= 12.2 cm^3
// Dry kibble bulk density ~0.35 g/cm^3  ->  ~4.3 g per pocket. This is an
// ESTIMATE -- the weigh-once calibration is what counts (GRAMS_PER_POCKET in the
// board file). Bigger pockets: raise WIDTH (linear).

// ---- 28BYJ-48 STEPPER (the drive motor) ------------------------------------
// All values below are DATASHEET-CONFIRMED (5VDC unit, 64:1, ears 42mm t-t).
STEP_BODY_D      = 28;    // motor body diameter (datasheet 28)
STEP_FLANGE_SPAN = 35;    // the two mount holes, centre-to-centre (ears 42 t-t)
STEP_FLANGE_HOLE = 4.3;   // mount holes (M4 clearance)
STEP_SHAFT_OFFS  = 8;     // shaft axis offset from the flange-hole line
STEP_SHAFT_D     = 5.0;   // round shaft diameter (datasheet 5)
STEP_SHAFT_FLAT  = 3.0;   // shaft width across the flats (datasheet "3 on flats")
STEP_SHAFT_LEN   = 8;     // shaft length from the mounting face (datasheet 8)
STEP_BOSS_D      = 10;    // raised boss around the shaft on the output face

// ---- MG90S (legacy - servo replaced by the stepper, kept for reference) ----
SERVO_BODY_L = 22.8; SERVO_BODY_W = 12.2;
SERVO_TAB_SPAN = 32.2; SERVO_TAB_T = 2.5; SERVO_SCREW_D = 2.2;
SERVO_SCREW_SPAN = 27.8; SERVO_SHAFT_DX = 5.9;

HORN_R       = 9;      // round horn radius (recess in the rotor hub)
HORN_T       = 1.8;    // horn thickness
SPLINE_D     = 5.2;    // splined boss clearance hole

// ---- openings --------------------------------------------------------------
INLET_W      = 34;     // hopper feeds in through the top of the housing
OUTLET_W     = 34;     // kibble falls out of the bottom
MOUNT_SCREW_D = 3.6;   // M3 through-holes, plates -> housing. Clearance, not a
                       // fit, so it does not take FIT -- but see FOOT_SCREW: 3.4
                       // can print to 3.1 on MJF, which fights an M3. Post wall
                       // around it is still (2*POST_R - 3.6)/2 = 2.7mm.
POST_R       = 4.5;

HOPPER_TOP_R = 45;     // funnel mouth
HOPPER_H     = 70;

$fn = $preview ? 40 : 96;   // fast previews, smooth final render

// ============================================================================
// rotor
// ============================================================================
module rotor() {
    difference() {
        union() {
            cylinder(r = HUB_R, h = WIDTH);
            for (i = [0 : POCKETS - 1])
                rotate([0, 0, i * 360 / POCKETS])
                    translate([-VANE_T / 2, 0, 0])
                        cube([VANE_T, ROTOR_R, WIDTH]);
        }

        // 28BYJ-48 D-shaft socket in the driven face (bottom): the rotor
        // presses onto the 5mm double-flatted shaft. A round hole intersected
        // with a slab leaves two flats, so the shaft's flats key the rotor and
        // it can transmit torque without a horn/spline.
        //
        // THIS IS THE ONE FIT THAT DOES NOT GET `FIT`, and it is a deliberate
        // exception. The shaft is a purchased steel part held to a few hundredths,
        // so only the printed half moves -- the tolerance stack is half as bad as
        // a printed-to-printed fit. More to the point, the flats ARE the drive:
        // opening them 0.6 per side would put 1.2mm of backlash in the only joint
        // carrying torque, and a dosing rotor that lashes 1.2mm is a dosing rotor
        // that mis-counts. Opened only from 0.25 to 0.4 on the round (0.2/side)
        // and 0.25 to 0.35 across the flats, which is enough that a nominal print
        // slips on by hand. At the tight end of MJF's +/-0.3 it will still need a
        // 5mm drill spun through by hand and a file on the flats -- budget for
        // that. It is 30 seconds, and it buys a drive that does not rattle.
        translate([0, 0, -0.1])
            intersection() {
                cylinder(d = STEP_SHAFT_D + 0.4, h = STEP_SHAFT_LEN + 0.6);
                cube([STEP_SHAFT_D + 1, STEP_SHAFT_FLAT + 0.35, 2 * (STEP_SHAFT_LEN + 1)],
                     center = true);
            }

        // Stub-axle socket in the idle face (top), over plate_idle's 6mm boss.
        // Printed-to-printed, so it takes FIT: d = 6 + 2*0.6 = 7.2. It used to be
        // 6.3 (0.15/side) chasing "minimal wobble", which on MJF is 0.45mm of
        // INTERFERENCE at the bad corner -- the rotor would not have gone in at
        // all. The slop is affordable here and nowhere else: the rotor is
        // journalled at BOTH ends and the steel D-shaft does the locating, so
        // this end only has to hold the rotor up, not centre it.
        translate([0, 0, WIDTH - 8])
            cylinder(d = 6 + 2 * FIT, h = 8.1);
    }
}

// ============================================================================
// housing: the drum the rotor turns inside.
//
// The bore is a THROUGH-hole -- the two end plates cap it, and the rotor drops
// in from either end. Printed standing (bore vertical) so the bore itself needs
// no support. The inlet (+Y) and outlet (-Y) are cut clean through the wall by
// one slot; their ceilings are ~34mm flat bridges between two anchored walls,
// which any printer handles without support.
// ============================================================================
HOUSING_H = WIDTH + 2 * FIT;  // 39.2 -- 0.6mm end float per face for the rotor.
                              // Axial, but still a printed-to-printed fit and
                              // still a running one: the rotor turns between the
                              // two end plates. At the old +1 the worst-case
                              // stack left 0.2/side and a rotor that could rub.
RIM       = 3;              // solid rim above/below the windows

module housing() {
    difference() {
        union() {
            cylinder(r = BORE_R + WALL, h = HOUSING_H);
            for (a = [45, 135, 225, 315])          // screw posts
                rotate([0, 0, a])
                    translate([BORE_R + WALL - 1, 0, 0])
                        cylinder(r = POST_R, h = HOUSING_H);
        }

        // bore, all the way through
        translate([0, 0, -0.1]) cylinder(r = BORE_R, h = HOUSING_H + 0.2);

        // inlet + outlet in one cut: a slot straight through both walls.
        // (Posts sit on the 45-degree diagonals at |x| ~ 22, clear of this.)
        translate([-INLET_W / 2, -(BORE_R + WALL + 2), RIM])
            cube([INLET_W, 2 * (BORE_R + WALL + 2), HOUSING_H - 2 * RIM]);

        // plate screws
        for (a = [45, 135, 225, 315])
            rotate([0, 0, a])
                translate([BORE_R + WALL - 1, 0, -0.1])
                    cylinder(d = MOUNT_SCREW_D, h = HOUSING_H + 0.2);
    }
}

// ============================================================================
// end plates
//
// PLATE_R was BORE_R + WALL + 2 (r34, O68). The four M3 plate screws sit at
// radius 31 and are 3.4 dia, so their outer edge reached 32.7 -- leaving only
// 1.3mm of plate outboard of each hole. That is one and a half extrusion widths
// of PLA carrying the clamp load of the screw that holds the whole drum shut;
// it splits. (The matching bosses in the HOUSING are fine -- POST_R 4.5 gives
// them 2.8mm.) +4 makes it r36 / O72 and the margin 3.3mm.
// This is not a free change: the A-frame cup that journals the plate has to grow
// with it, hence SAD_R below. Keep the two in step.
// ============================================================================
PLATE_R = BORE_R + WALL + 4;

module plate_blank() {
    difference() {
        cylinder(r = PLATE_R, h = 4);
        for (a = [45, 135, 225, 315])
            rotate([0, 0, a])
                translate([BORE_R + WALL - 1, 0, -0.1])
                    cylinder(d = MOUNT_SCREW_D, h = 4.2);
    }
}

// Servo plate. The servo body drops through the pocket; its flange screws to
// the plate face. The rotor axis sits on the SHAFT, which is offset from the
// body centre -- so the body pocket is shifted by -SERVO_SHAFT_DX to bring the
// shaft onto the plate centre (= the rotor axis).
module plate_servo() {
    difference() {
        plate_blank();

        translate([-SERVO_SHAFT_DX, 0, 0]) {
            // body pocket, through. Clearance split EVENLY on both sides so the
            // pocket stays CENTRED on the shaft (offsetting by half the pocket,
            // not half the body, keeps SHAFT_DX honest).
            translate([-(SERVO_BODY_L + CLEAR) / 2, -(SERVO_BODY_W + CLEAR) / 2, -0.1])
                cube([SERVO_BODY_L + CLEAR, SERVO_BODY_W + CLEAR, 4.2]);
            // flange recess so the servo sits flush (clearance also centred)
            translate([-(SERVO_TAB_SPAN + CLEAR) / 2, -(SERVO_BODY_W + CLEAR) / 2, 4 - SERVO_TAB_T])
                cube([SERVO_TAB_SPAN + CLEAR, SERVO_BODY_W + CLEAR, SERVO_TAB_T + 0.1]);
            // mounting screws
            for (x = [-SERVO_SCREW_SPAN / 2, SERVO_SCREW_SPAN / 2])
                translate([x, 0, -0.1])
                    cylinder(d = SERVO_SCREW_D, h = 4.2);
        }
    }
}

// 28BYJ-48 stepper mount (THIS is the drive plate now, not plate_servo). The
// motor bolts to the back through its two 35mm flange holes; its shaft is on
// the plate CENTRE (= the rotor axis) and pokes through the central bore into
// the rotor's D-socket. The flange holes sit STEP_SHAFT_OFFS from the shaft.
module plate_stepper() {
    difference() {
        plate_blank();
        // boss + shaft clearance, central = rotor axis
        translate([0, 0, -0.1]) cylinder(d = STEP_BOSS_D + 1, h = 4.2);
        // two flange mounting holes (M4), 35mm apart, offset from the shaft line
        for (x = [-STEP_FLANGE_SPAN / 2, STEP_FLANGE_SPAN / 2])
            translate([x, STEP_SHAFT_OFFS, -0.1])
                cylinder(d = STEP_FLANGE_HOLE, h = 4.2);
    }
}

// Idle plate: a plain stub axle for the far end of the rotor.
module plate_idle() {
    union() {
        plate_blank();
        translate([0, 0, 4]) cylinder(d = 6, h = 7);
    }
}

// ============================================================================
// hopper: a funnel with a square neck that SADDLES onto the round drum, so it
// seats on the curve instead of hovering above it. Sits over the inlet.
// ============================================================================
NECK_H = 12;
// ---- HOP_DROP: how far the hopper is SUNK onto the drum ---------------------
// It used to be placed with its neck's bottom face exactly at the top of the
// drum (world z = BORE_R + WALL = 32). But the saddle cylinder is centred on the
// drum axis with radius 32.3, so its crown only reached z=32.3 -- the cut bit
// just 0.3mm deep and only out to x=+/-4.2 (measured off the exported mesh).
// The "saddle" was therefore LINE CONTACT along the crown of the drum: the
// funnel hovered 4.9mm off the housing at the inlet edges (x=+/-17) and 7.0mm
// at the neck corners (x=+/-20). It could rock, and nothing located it.
// Sinking it 7mm puts the neck's bottom plane level with the drum's surface at
// x=20 (sqrt(32^2-20^2) = 24.98), so the groove now runs the full width of the
// neck -- 7.3mm deep at the centre, tapering to nothing at the corners -- and
// the funnel beds down on the curve. 4.7mm of neck is left above the groove.
// NOTE this must be applied in TWO places that have to agree: the placement in
// assembly() and the saddle-cylinder centre below.
HOP_DROP = 7;

module hopper() {
    difference() {
        union() {
            linear_extrude(NECK_H)
                square([INLET_W + 2 * WALL, INLET_W + 2 * WALL], center = true);
            translate([0, 0, NECK_H])
                cylinder(r1 = INLET_W / 2 + WALL, r2 = HOPPER_TOP_R + WALL, h = HOPPER_H);
        }
        // throat
        translate([0, 0, -0.1])
            linear_extrude(NECK_H + 0.2)
                square([INLET_W - CLEAR, INLET_W - CLEAR], center = true);
        translate([0, 0, NECK_H - 0.01])
            cylinder(r1 = INLET_W / 2 - CLEAR, r2 = HOPPER_TOP_R, h = HOPPER_H + 0.1);
        // saddle: subtract the drum's outer curve. The drum axis runs along Y in
        // the assembly, so the groove must run along Y (rotate about X, not Y).
        // The axis sits HOP_DROP above where it would be if the neck just rested
        // on the drum's crown -- keep this in step with the placement in
        // assembly(), or the funnel will float again.
        translate([0, 0, -(BORE_R + WALL - HOP_DROP)])
            rotate([90, 0, 0])
                // + FIT, not the old + 0.3: the saddle wraps a printed drum, so
                // both halves carry MJF's +/-0.3 and 0.3 nominal is a possible
                // interference. The groove just gets 0.3mm deeper (crown 7.6 not
                // 7.3) -- HOP_DROP is untouched, the neck still beds on the curve
                // across its full width, and 4.4mm of neck is left above it. The
                // funnel can now shift 0.6mm on the drum; the post scallops below
                // are what actually locate it, so that does not matter.
                cylinder(r = BORE_R + WALL + FIT, h = 300, center = true);
        // Relief for the housing's two UPPER screw posts. The saddle above only
        // clears the drum's 32mm barrel, but the posts stand PROUD of it: r=4.5
        // centred at radius 31 on the 45deg diagonals, so they bulge to 35.5.
        // Once the hopper is sunk by HOP_DROP the neck's outer bottom corners run
        // into them (CGAL: a 0.63mm-deep sliver at x=+/-20, z=25.35..25.99) and
        // the funnel would rock on two posts instead of bedding on the curve.
        // These cuts take a ~1.9mm-wide scallop off the bottom outer edge of the
        // neck -- they stop short of the throat wall at x=17, so nothing thins --
        // and they double as anti-rotation pockets.
        for (a = [45, 135])
            translate([cos(a) * (BORE_R + WALL - 1), 0,
                       -(BORE_R + WALL - HOP_DROP) + sin(a) * (BORE_R + WALL - 1)])
                rotate([90, 0, 0])
                    // + FIT: these scallops are the hopper's anti-rotation
                    // location, i.e. a real printed-to-printed fit, not just a
                    // relief. Widening 0.4 -> 0.6 takes the scallop from ~1.9 to
                    // ~2.1mm and still stops well short of the throat wall at
                    // x=17, so nothing thins.
                    cylinder(r = POST_R + FIT, h = 300, center = true);
    }
}

// ============================================================================
// assembly preview -- shown in the ORIENTATION IT RUNS IN (drum axis
// horizontal, inlet up, outlet down), which is NOT how the parts are printed.
// ============================================================================
module assembly() {
    // drum + rotor + plates, rotated so the inlet (+Y local) points up (+Z)
    rotate([-90, 0, 0]) {
        color("SlateGray") housing();
        color("Goldenrod") translate([0, 0, 0.5]) rotor();
        color("IndianRed") translate([0, 0, -4]) plate_stepper();
        // idle plate flipped so its stub axle points INTO the drum
        color("SteelBlue") translate([0, 0, HOUSING_H + 4]) rotate([180, 0, 0]) plate_idle();
    }
    // hopper sits on top of the drum, CENTRED over the rotor/inlet (drum spans
    // world y = 0..HOUSING_H, so its centre is HOUSING_H/2, not y=0).
    color("Silver") translate([0, HOUSING_H / 2, BORE_R + WALL - HOP_DROP]) hopper();
}

// ============================================================================
// board_cradle: holds the Waveshare ESP32-S3-Touch-LCD-3.5-C so the SCREEN
// faces UP (owner reads it) and the CAMERA (dead-centre of the back) looks
// straight DOWN through a window at the bowl.
//
// The board rests SCREEN-UP on four posts. The camera peers down through
// CAM_WIN in the base; the bottom GPIO header clears through HDR_SLOT, and
// that slot is also where the servo signal wire reaches GPIO43.
//
// ---- CHECKED AGAINST THE WAVESHARE OUTLINE DRAWINGS ------------------------
// Waveshare ships TWO SKUs and they are NOT the same part:
//   30733  ESP32-S3-Touch-LCD-3.5    bare board, 92.44 x 61.00 x 11.50, R6.00,
//                                    four REAL M2.00 mounting holes
//   30934  ESP32-S3-Touch-LCD-3.5-C  in a case + OV5640, 95.11 x 63.67 x 14.10,
//                                    R7.30, and NO mounting holes (see below)
// The BOM part is the -C, and the outline numbers below are the -C's -- they
// check out exactly against the drawing, so BM_W/BM_H/BM_CORNER are confirmed,
// not assumed. Likewise the hole/foot pattern: the drawing gives 72.00 (X) and
// 48.50 (Y), and both are CENTRED (95.11-72.00 = 23.11 -> 11.56 each side, which
// the drawing states; 63.67-48.50 = 15.17 -> 7.59 each side, ditto). So
// +/-36.00, +/-24.25 is right. The camera being dead-centre is confirmed the
// same way: the drawing's 36.00 is exactly half of 72.00 and its 24.25 exactly
// half of 48.50, and both leaders point at the lens.
//
// ---- THE ONE THAT BITES: the -C HAS NO MOUNTING HOLES ----------------------
// This cradle used to say the board "is fixed with four M2.5 screws dropped
// through the corner mounting holes from the top". On the -C there is nothing
// to drop a screw through. What sits at those four positions is a smooth,
// domed, blind FOOT -- confirmed on the outline drawing and on two product
// photos of the cased unit. The bare board's holes are underneath the case.
// (They are M2.00, by the way, not M2.5 -- the bare-board drawing labels them.)
// So the four posts are SUPPORT PADS, nothing more: they carry the board on its
// four feet and the pocket walls locate it in X/Y, but as it stands NOTHING
// holds the board DOWN. See BM_SCREW_D. Retention is unresolved -- decide it
// before printing the cradle.
// ============================================================================
BM_W        = 95.11;   // board width           ) all four CONFIRMED against the
BM_H        = 63.67;   // board height          ) Waveshare -C outline drawing;
BM_CORNER   = 7.30;    // board corner radius   ) do not "correct" them to the
                       //                       ) bare board's 92.44/61.00/R6.00
BM_HOLE_X   = 36.00;   // foot/hole offset from centre, X (72.00 / 2)  CONFIRMED
BM_HOLE_Y   = 24.25;   // foot/hole offset from centre, Y (48.50 / 2)  CONFIRMED
BM_SCREW_D  = 1.7;     // self-tap pilot down each post. Was 2.2 "M2.5" -- both
                       // halves of that were wrong. The mounting holes are
                       // M2.00 (labelled on the bare-board drawing), and 2.2 is
                       // the CLEARANCE diameter for M2, so a screw would have
                       // spun freely in the post and gripped nothing. A self-tap
                       // pilot wants roughly (major dia - pitch) = 2.0 - 0.4 =
                       // 1.6; 1.7 is the usual practical value in PLA, tight
                       // enough to hold thread and loose enough not to split an
                       // 8mm post.
                       // NOTE this only means anything if you fit the BARE board
                       // (SKU 30733). On the -C the posts are blind support pads
                       // under the case's rubber feet and these pilots do
                       // nothing -- harmless, but they are not retention.
BM_POST_R   = 4;       // post outer radius
BM_STANDOFF = 12;      // board sits this high. NOT for the camera barrel -- that
                       // is recessed in the case and needs nothing. It buys room
                       // for the GPIO header's mating connector and the servo
                       // lead, which need ~10mm to plug in without straining.
                       // Be aware it fights BM_CAM_WIN: every mm of standoff
                       // widens the camera's cone at the base by 2*tan(FOV/2) mm,
                       // ~1.45mm per mm at 72deg. If the window ever has to grow
                       // past ~24 the answer is to lower this, not to keep
                       // cutting away the base.
BM_BASE_T   = 3;       // base plate thickness
BM_WALL     = 2.8;     // nominal locating wall around the board. The pocket's
                       // BM_FIT clearance eats half of it, so what actually gets
                       // printed is BM_WALL - BM_FIT/2 = 1.8mm.
                       // 2.8 not 2.4: opening BM_FIT to 1.0 per side would have
                       // taken the printed wall to 1.4mm, and at FDM's +/-0.5 that
                       // can arrive as 0.9 -- under JLCPCB's 1.2mm FDM minimum.
                       // Growing the nominal keeps the printed wall at 1.8 (1.3 at
                       // the bad corner). It costs 0.8mm on the part's outside
                       // envelope, which nothing is competing for.
BM_WALL_H   = 14;      // locating wall height. This USED to be 5, which made the
                       // pocket purely decorative: the board rests on the posts at
                       // z = BM_BASE_T + BM_STANDOFF = 15, but walls of height 5
                       // topped out at z=8 -- the board floated 7mm ABOVE them and
                       // was held by the four screws alone, free to rack sideways
                       // while you push a USB cable in. 14 puts the wall tops at
                       // z=17, i.e. 2mm up the edge of a seated board, so it is
                       // actually captured on all four sides. It does not shadow
                       // the screen: the 80x44 display sits well inside the 95x64
                       // outline that the wall traces.
BM_FIT      = 2.0;     // 2.0 total across each axis, 1.0 per side.
                       //
                       // DELIBERATELY NOT `2 * FIT`, and this is the one fit in
                       // the file that is decoupled from it. FIT is sized for MJF
                       // (+/-0.3). This part is printed FDM, where JLCPCB's
                       // tolerance is +/-0.5 -- and this is the largest feature in
                       // the model, 95.11mm across, so it sees the full swing. At
                       // 0.6 per side an FDM pocket printed to the tight end of
                       // that leaves 0.1mm, which is a pocket you have to force a
                       // 4000-yen board into. 1.0 per side leaves 0.5mm at the
                       // same bad corner and simply drops in.
                       //
                       // It is worth the slop BECAUSE OF WHAT FAILS. Everything
                       // else in this machine can be persuaded -- reamed, filed,
                       // shimmed. If this pocket comes out tight the display does
                       // not seat, and the demo has no screen. Trading 0.4mm of
                       // location for that is not a close call; the four corner
                       // clamps are what actually hold the board anyway, and
                       // BM_LIP_W below is re-sized so they still do.
// ---- RETENTION: four corner snap clamps ------------------------------------
// The -C has no mounting holes (see the header comment), so something has to
// hook OVER the board. The obvious move is to run the pocket walls up the full
// 14.10mm of the case and put lips along the long edges -- but that buries every
// port. The case carries USB-C on a short edge and PWR/RST/BOOT plus the TF slot
// along a long edge, and the -C drawing does not dimension any of them well
// enough to cut windows I would trust. Get one wrong and the board is unpowered
// or the SD card is unreachable, in a part that takes hours to print.
// So the walls STAY at BM_WALL_H (they only come 2mm up the side of the case,
// below everything) and the lips go on four towers at the CORNERS instead. The
// corners are the one place the drawing proves is clear: the case puts its own
// feet there, 11.56mm in from the short edges and 7.59mm from the long ones, so
// that real estate is structural, not connectors. Every edge stays fully open.
// Each tower is an isolated column ~2.4mm thick and ~29mm tall, so it flexes
// easily; the board presses straight down, the four ramps splay them, and it
// snaps home. To remove, spring two diagonal clamps outward and lift.
// CONFIRMED ON THE PHYSICAL BOARD (2026-07-24): the BM_CLAMP reach along each
// edge really is empty. This was the one value inferred from the drawing rather
// than measured on the part; it now checks out, so the corner scheme stands.
BM_BOARD_T  = 14.10;   // -C case thickness (drawing, confirmed)
BM_LIP_W    = 2.8;     // how far each lip reaches inward from the POCKET WALL --
                       // which is not the same as how far it reaches over the
                       // BOARD, and that distinction is what forces this to track
                       // BM_FIT. The lip starts at the pocket wall, so every mm
                       // the pocket opens is a mm the lip retreats:
                       //     grip = BM_LIP_W - BM_FIT/2
                       // Opening BM_FIT to 2.0 for FDM would have left 1.0mm of
                       // grip nominal and 0.0 on the far side once the board slid
                       // to one end of its own clearance -- a clamp that lets go.
                       // 2.8 restores the designed numbers: 1.8mm nominal, 0.8mm
                       // worst-case. That is the "re-sized so they still hold"
                       // half of the BM_FIT decision above; the two move together.
                       //
                       // Still lands on bezel, which is the constraint that caps
                       // it: the glass is inset 10.83mm in X and 7.34mm in Y, so
                       // the lip edge stops ~9.0/5.5mm short nominal, ~8.0/4.5mm
                       // with the board pushed fully to one side. Nowhere near it.
BM_LIP_T    = 1.6;     // thickness of the flat retaining face under the lip.
                       // 1.6 not 1.2: this is the face that takes the whole
                       // pull-out load, and 1.2 is exactly JLCPCB's FDM minimum
                       // wall -- zero margin, on the thinnest feature in the
                       // model. On a powder bed 1.2 was fine; printed in filament
                       // a minimum-wall retaining face is two perimeters and a
                       // guess. 1.6 buys a real one for 0.4mm of height.
BM_CLAMP    = 14;      // how far each clamp reaches along the edges from the
                       // outer corner (~11.9mm measured on the board itself)
BM_TOP_Z    = BM_BASE_T + BM_STANDOFF + BM_BOARD_T;   // top face of a seated board
BM_CAM_WIN  = 22;      // camera window dia (centre). NOT sized by the lens barrel
                       // -- that was the old mistake. Measured off the -C outline
                       // drawing (calibrated on the 72.00 boss pitch, two
                       // independent scale checks agreeing to 0.55%), the barrel
                       // showing through the case is only ~6.9mm, so a 14mm hole
                       // clears it with room to spare and still throttles the
                       // camera, because what actually has to fit through the
                       // hole is the FIELD OF VIEW CONE, not the glass.
                       // The lens sits at z = BM_BASE_T + BM_STANDOFF = 15 and the
                       // binding aperture is the base's BOTTOM face at z = 0, so
                       // the throw is 15mm and the cone there is
                       //     dia = 2 * 15 * tan(FOV_diagonal / 2)
                       // = 19.1mm at 65deg, 21.8mm at 72deg. 14mm would have
                       // vignetted the corners off every frame -- and the pet
                       // detector would have been quietly cropped without any
                       // obvious symptom. 22 covers the usual 5MP OV5640 lens up
                       // to ~72deg diagonal.
                       // Waveshare do NOT publish the FOV of the bundled module,
                       // so this is sized for the common case, not measured: if
                       // the module turns out to be a WIDE-ANGLE OV5640 (some are
                       // 120deg+, which would want 52mm and is not survivable at
                       // this standoff), the fix is to cut BM_STANDOFF, not to
                       // keep opening this hole. Re-run the formula if you swap
                       // the camera.
BM_HDR_W    = 44;      // GPIO header slot width. Was 40, guessed as "2x14 @2.54
                       // ~36mm + margin" -- but the header is 2x16, not 2x14:
                       // the page calls it a 32PIN header and the -C's own back
                       // label prints the pinout as two columns of sixteen. A
                       // 2x16 body at 2.54 pitch is 16*2.54 = 40.64mm, and the
                       // opening measured off the drawing is ~41.9mm, so the old
                       // 40 was NARROWER than the connector it was meant to clear.
BM_HDR_D    = 9;       // GPIO header slot depth (opening measures ~6.8mm; 9 gives
                       // ~1.1mm each side for print tolerance and cable dressing)
BM_HDR_OFF  = -24.2;   // header slot centre, offset along local -Y from the camera
                       // hole. Sign matters: the cradle is tilted by BM_TILT about
                       // X and then turned 180 about Z, and a Z-turn does NOT touch
                       // the Z component -- so local +Y ends up pointing (0,-.866,
                       // +0.5), i.e. UP in world Z. A positive offset therefore put
                       // the slot ABOVE the camera hole (wrong: crumbs off the
                       // header ledge drop straight onto the lens, and the servo
                       // wire has to climb). Negative puts it BELOW the hole.
                       // The MAGNITUDE is no longer a guess either: on the drawing
                       // the header opening centres 24.2mm below the case centre
                       // (and is centred in X to within 0.2mm), so -22 had the slot
                       // 2.2mm high -- enough to catch the connector's shoulder on
                       // one edge. Seating the board the other way up so the header
                       // lands on the -Y side is deliberate; the screen is rotated
                       // in software.
BM_TILT     = 30;      // board_cradle_stand: forward tilt (deg) so the camera
                       // looks down-AND-out at the pet's approach, not straight
                       // down at the top of its head. 0 = flat.

module bm_rrect(w, h, th) {          // rounded-rect prism, centred
    linear_extrude(th)
        offset(r = BM_CORNER) offset(r = -BM_CORNER)
            square([w, h], center = true);
}

// One snap clamp per corner. Each is built by taking the FULL-height wall ring,
// the FULL lip ring and the FULL lead-in ramp, then intersecting all three with a
// BM_CLAMP box pinned to one corner -- so the shapes stay concentric with the
// pocket (no trigonometry around the corner radius) and only a corner's worth
// survives. The lip's flat face lands at BM_TOP_Z, i.e. exactly on top of a
// seated case; the ramp above it rises at 45deg, which is both the lead-in that
// splays the tower during insertion and, printed Z-up, a self-supporting slope.
module bm_corner_clamps() {
    OW = BM_W + 2*BM_WALL;   OH = BM_H + 2*BM_WALL;   // outer
    PW = BM_W + BM_FIT;      PH = BM_H + BM_FIT;      // pocket
    for (sx = [-1, 1], sy = [-1, 1])
        intersection() {
            // ONE solid prism with the board's space cut out of it -- not three
            // stacked rings unioned together. Stacking them only made the tower,
            // the lip and the ramp TOUCH along shared faces, and CGAL counted the
            // result as nine separate volumes: the clamps would have sliced off
            // the tray as loose shells. Cutting a single prism keeps each clamp
            // one solid, and its z 0..BM_BASE_T footing overlaps the tray's base
            // plate by real volume, which is what fuses it to the tray.
            difference() {
                bm_rrect(OW, OH, BM_TOP_Z + BM_LIP_T + BM_LIP_W);
                union() {
                    // the seated board's space
                    translate([0, 0, BM_BASE_T])
                        bm_rrect(PW, PH, BM_TOP_Z - BM_BASE_T + 0.01);
                    // opening under the lip -- what is left here IS the lip
                    translate([0, 0, BM_TOP_Z])
                        bm_rrect(PW - 2*BM_LIP_W, PH - 2*BM_LIP_W, BM_LIP_T + 0.01);
                    // 45deg lead-in, flaring back out to the pocket wall
                    translate([0, 0, BM_TOP_Z + BM_LIP_T])
                        hull() {
                            bm_rrect(PW - 2*BM_LIP_W, PH - 2*BM_LIP_W, 0.01);
                            translate([0, 0, BM_LIP_W - 0.01]) bm_rrect(PW, PH, 0.01);
                        }
                }
            }
            translate([sx > 0 ? OW/2 - BM_CLAMP : -OW/2,
                       sy > 0 ? OH/2 - BM_CLAMP : -OH/2, -1])
                cube([BM_CLAMP, BM_CLAMP, BM_TOP_Z + BM_LIP_T + BM_LIP_W + 2]);
        }
}

module board_cradle() {
    difference() {
        union() {
            // tray = outer box MINUS inner pocket (one manifold solid: floor + walls)
            difference() {
                bm_rrect(BM_W + 2*BM_WALL, BM_H + 2*BM_WALL, BM_BASE_T + BM_WALL_H);
                translate([0, 0, BM_BASE_T])
                    bm_rrect(BM_W + BM_FIT, BM_H + BM_FIT, BM_WALL_H + 1);   // pocket the board drops into
            }
            // four support pads under the case's feet (NOT screw posts on the -C)
            for (sx = [-1, 1], sy = [-1, 1])
                translate([sx * BM_HOLE_X, sy * BM_HOLE_Y, 0])
                    cylinder(r = BM_POST_R, h = BM_BASE_T + BM_STANDOFF);
            bm_corner_clamps();
        }
        // pilot holes down the posts
        for (sx = [-1, 1], sy = [-1, 1])
            translate([sx * BM_HOLE_X, sy * BM_HOLE_Y, BM_BASE_T])
                cylinder(d = BM_SCREW_D, h = BM_STANDOFF + 0.1);
        // camera window, centre
        translate([0, 0, -0.1]) cylinder(d = BM_CAM_WIN, h = BM_BASE_T + 0.2);
        // GPIO header slot (also the servo-wire exit). Offset by BM_HDR_OFF, which
        // is NEGATIVE so the slot lands BELOW the camera hole once the cradle is
        // tilted and turned into its mounted orientation -- see BM_HDR_OFF.
        translate([-BM_HDR_W/2, BM_HDR_OFF - BM_HDR_D/2, -0.1])
            cube([BM_HDR_W, BM_HDR_D, BM_BASE_T + 0.2]);
    }
}

// board_cradle_stand: the cradle tilted forward BM_TILT degrees on two OPEN side
// legs. The centre stays open so the camera's (now angled) line of sight is
// clear -- a solid wedge would occlude it. Sits flat, ready to place beside the
// bowl with the screen facing the owner and the camera facing the approach.
module bm_leg(ow_x, oh, lift) {
    translate([ow_x, 0, 0]) hull() {
        // tilted underside bar (matches the cradle's side rail once lifted)
        translate([0, 0, lift]) rotate([BM_TILT, 0, 0])
            translate([-BM_WALL/2, -oh/2, 0]) cube([BM_WALL, oh, 0.1]);
        // ground bar
        translate([-BM_WALL/2, -oh/2, 0]) cube([BM_WALL, oh, 0.1]);
    }
}
// show=false -> printable (bare cradle on the wedge). show=true -> preview with
// the board + screen + camera seated in the cradle (used by product()).
module board_cradle_stand(show = false) {
    OW = BM_W + 2*BM_WALL;
    OH = BM_H + 2*BM_WALL;
    lift = OH * sin(BM_TILT);
    difference() {
        union() {
            translate([0, 0, lift]) rotate([BM_TILT, 0, 0])
                if (show) screen_holder(); else board_cradle();
            bm_leg( (OW/2 - BM_WALL/2), OH, lift);
            bm_leg(-(OW/2 - BM_WALL/2), OH, lift);
        }
        // trim to a flat bottom at z = 0
        translate([0, 0, -100]) cube([500, 500, 200], center = true);
    }
}
BM_LIFT = (BM_H + 2*BM_WALL) * sin(BM_TILT);   // how high the wedge raises the cradle

// ============================================================================
// STAND: an A-frame that cradles the feeder's two end-plate discs (holding the
// drum up, outlet pointing down), a base the BOWL sits on under the outlet, and
// an integrated CAMERA POST carrying the board cradle. Built in the assembly()
// frame: drum axis along +Y (ends ~y=0 and y=39), drum centre at z=0, outlet at
// the bottom (~z=-32). `part="product"` previews it all together.
// ============================================================================
BOWL_D       = 130;   // standard cat bowl diameter
BOWL_DEP     = 50;    // bowl depth
DROP_GAP     = 40;    // clearance from the outlet down to the bowl rim. Raised
                      // from 22 to 40 when the bowl moved 50mm forward: a longer
                      // bowl run means a longer CHUTE, and the chute can only stay
                      // at ~19deg if it has more vertical room. See the joint
                      // solve at the "back" layout aim block below.
BOWL_CY      = -122;  // bowl centre, IN FRONT of the feeder (clear of the column).
                      // Moved 50mm further forward (and 18mm down via DROP_GAP)
                      // together with WEDGE_Y and PET_FACE_Y/Z, to open a real gap
                      // between the screen and the funnel. Shifting the wedge, the
                      // bowl and the pet by the SAME delta leaves the camera->pet
                      // vector untouched, so the aim needs no re-solving -- only
                      // PED_TOP follows the 18mm drop.
SAD_R        = PLATE_R + 1;  // cradle-notch radius: the end plate (PLATE_R) plus
                      // 1mm running clearance. This is a printed-to-printed fit
                      // and it is left at 1 rather than moved to FIT because 1 is
                      // ALREADY over it -- worst case it prints down to 0.4 and
                      // the plate still drops in. Nothing is gained by loosening
                      // a cradle that is meant to locate the drum.
                      // TIED to PLATE_R rather than typed
                      // as a number, because it went stale once already -- the
                      // plate grew from r34 to r36 to give its screws real meat,
                      // and a hard-coded 35 would have clamped the plate.
                      // Widening the cup also fixes something that was quietly
                      // wrong before: the drum is assembled AXIALLY (the drop-in
                      // throat is 1.44*SAD_R, sized for the hopper NECK, not for
                      // the Ohm64 drum), which means the housing has to pass through
                      // this cup -- and the housing's screw posts bulge to r35.5,
                      // 0.5mm PROUD of the old r35 cup. At r37 they pass freely.
SAD_T        = 8;     // cradle support thickness (along the drum axis)
SUPP_W       = 84;    // support width at the base (X)
STAND_BASE_T = 5;
STAND_BASE_Z = -(BOWL_DEP + DROP_GAP + 32);   // top of the base plate (~ -104)
SUPP_Y1      = -2;    // support at the stepper-plate end
SUPP_Y2      = 41;    // support at the idle-plate end
BASE_FRONT_EXT = 18;  // extra platform length in FRONT of the bowl (-Y)

// ---- back-camera aim -------------------------------------------------------
// The camera is on the BACK of the board, so with the screen facing UP it stares
// straight DOWN -- at the top of the pet's head. We hold the board on the arm
// over the bowl and TILT it forward so the camera's optical axis points
// down-AND-forward, landing on the pet's FACE where it lowers in to eat.
//
// Rather than hand-guess the angle, we aim the board at a TARGET POINT (the
// pet's face) and COMPUTE the tilt -- move the target and the camera re-aims.
// LAYOUT decides which side the person stands on. Screen and camera are on
// OPPOSITE faces of one board, so the owner and the pet are ALWAYS on opposite
// sides -- these two options just choose which:
//   "back"  : camera hangs over the bowl, screen faces BEHIND the funnel (owner
//             reads it from the back of the appliance).
//   "front" : the display rides a FRONT post facing the owner who stands at the
//             front; the camera (board back) looks BACK-and-down at the bowl.
LAYOUT = "back"; // [back:screen behind / camera over bowl, front:screen faces owner up front]

// ---- "back" layout aim (WEDGE STAND over the bowl) -------------------------
// The board rides the same printable 30-degree wedge stand as the desk part
// (board_cradle_stand), lifted on a short pedestal that rises from the base.
// The pedestal gives the HEIGHT; the wedge gives the 30-degree TILT; together
// the camera on the board's underside looks down-and-forward onto the pet.
PET_FACE_Y   = -142;  // where the pet's face is as it eats (over the bowl, -Y)
PET_FACE_Z   = -52;   // pet-face height (down over the food, off the front rim)
WEDGE_Y      = -104;  // wedge-stand centre, cantilevered forward over the bowl
PED_TOP      = -20;   // pedestal platform height the wedge sits on (102mm of post
                      // above the base plate at STAND_BASE_Z)
//
// ---- WHY THE BOARD IS 50mm FURTHER FORWARD AND 18mm LOWER THAN THE AIM ALONE
// ---- WOULD REQUIRE: the SCREEN has to be readable past the FUNNEL.
// The hopper mouth is r=48 (outer) centred at Y=+19.5, so its rim OVERHANGS
// forward to Y=-28.5 at z=114. In the previous placement the screen's back-top
// corner sat at (0,-28.5,54.5) -- exactly UNDER that rim. Every sightline from
// an owner standing behind the appliance clipped the funnel before it reached
// the screen. Two constraints then pull against each other:
//
//   screen clearance : the sightline down the screen's own normal (60deg from
//                      horizontal) must pass OVER the rim ->  1.732*D - d > 59.5
//   chute slope      : a bowl D further forward makes the chute run D longer,
//                      and it only holds ~19deg without ploughing the near rim
//                      if the bowl also drops d  ->  d >= 0.344*D
//
// Substituting gives 1.388*D > 59.5, i.e. D >= 43mm. D=50 / d=18 is used for
// margin: screen check 1.732*50 - 18 = 68.6 > 59.5, chute check 18 > 17.2.
// The screen's back-top corner now lands at (0,-78.5,36.5); the nearest point of
// the funnel is its rim at (0,-28.5,114) -- 92mm away, and 78mm of clear
// horizontal air to the funnel neck. DO NOT pull this back toward the drum.
// With BM_TILT=30, a wedge at (0,WEDGE_Y,PED_TOP) puts its camera near
// (0,WEDGE_Y,PED_TOP+BM_LIFT) aimed almost exactly onto (0,PET_FACE_Y,PET_FACE_Z).
// WHY IT SITS LOW AND FORWARD (do not "tidy" this by raising it): the wedge's
// tilt is fixed at 30 deg, so the camera must lie on ONE line back up the sight
// ray. Raising it also drags it BACKWARD -- straight into the drum and the
// hopper flare, which the board's back-top corner fouls. (At PED_TOP=6 that
// corner grazed the hopper by 0.57mm at Y~0, z~42: a real interference, caught
// by intersecting the placed wedge with assembly()+bowl in CGAL.) The clean
// solution is therefore low + forward, and it still lands the sightline on the
// pet at (0,-80,-34). The pet's head passes BETWEEN the rails (X +/-48, head
// only +/-26), so the low rails do not block it.
// (legacy back-layout single-board params, kept for the front layout / reference)
CAM_MOUNT_Y  = -68;
CAM_MOUNT_Z  =  62;
CAM_TILT     = atan2(PET_FACE_Y - CAM_MOUNT_Y, CAM_MOUNT_Z - PET_FACE_Z);

// ---- "front" layout aim (display on a front post, camera looks BACK) --------
CAM_F_Y      = -128;  // front display post, out ahead of the bowl
CAM_F_Z      =  34;   // display height (eye-line for someone standing at the front)
PETF_Y       = -76;   // pet face over the bowl, seen from the front post
PETF_Z       = -30;
// Camera on the BACK of the board looks back (+Y) and down: rotate([+θ,0,0]).
CAM_F_TILT   = atan2(CAM_F_Y - PETF_Y, CAM_F_Z - PETF_Z);

// active camera/pet points for the chosen layout (drive the preview aids)
CAM_ACT = (LAYOUT == "front") ? [0, CAM_F_Y, CAM_F_Z] : [0, WEDGE_Y, PED_TOP + BM_LIFT];
PET_ACT = (LAYOUT == "front") ? [0, PETF_Y, PETF_Z]   : [0, PET_FACE_Y, PET_FACE_Z];

module bowl() {                         // ~13 cm dish, in FRONT on the base
    translate([0, BOWL_CY, STAND_BASE_Z])
        difference() {
            cylinder(d1 = 0.72 * BOWL_D, d2 = BOWL_D, h = BOWL_DEP, $fn = 96);
            translate([0, 0, 4])
                cylinder(d1 = 0.72 * BOWL_D - 6, d2 = BOWL_D - 6, h = BOWL_DEP, $fn = 96);
        }
}

// ============================================================================
// SPLIT FOR PRINTING
// ----------------------------------------------------------------------------
// stand() as a single solid measures 164 x 268 x 172 against a 220 x 220 x 250
// bed. Rotation does not save it: a 164 x 268 rectangle turned 45deg needs a
// 305mm square envelope, and standing it on edge puts the 268 into Z, over the
// 250 height. So it has to be cut.
//
// Measured on their own, the four things stand() unions together are:
//     base plate      164 x 268 x  11    <-- the ONLY one that does not fit
//     A-frames (x2)    90 x  51 x 167        fits
//     pedestal        108 x  80 x 102        fits
//     chute            36 x 109 x  96.5      fits
//
// So this is two separate jobs, not one "split into three":
//   1. cut the BASE PLATE in two across Y and bolt the halves back together;
//   2. stop fusing the three towers into the plate -- give each a bolt-down
//      flange so it prints alone and screws on.
//
// ---- 1. where the plate is cut ---------------------------------------------
// The seam is a plain BUTT joint at BASE_SEAM_Y, carried by two splice bosses
// that straddle it with an M3 running along Y through each. A half-lap was
// tried first and thrown away: the plate floor is only 5mm, so each half of the
// lap would have been a 2.5mm shelf -- and the perimeter rim sits on top of it,
// which would have left the back tile's rim cantilevered on 2.5mm of PLA. Two
// 10mm-tall bosses in the open middle of the tray are both stronger and simpler.
//
// BASE_SEAM_Y = -32 is not arbitrary. Everything between the bowl and the column
// has to be missed, and the constraint is the SPLICE BOSSES (which reach 15mm
// FORWARD of the seam), not the cut line itself:
//     bowl locating ring   rearmost point Y = -51   ->  4mm clear of the boss
//     pedestal foot flange rear edge      Y = -50   ->  3mm clear of the boss
//     front A-frame flange front edge     Y = -15   ->  2mm clear of the boss
// The ring is the mean one: its rear arc sweeps back through Y -51..-70 across
// almost the whole width, so ANY seam far enough forward to be near the bowl
// puts a boss inside it. Do not move the seam forward to "balance" the two
// tiles -- 176mm and 92mm both fit, and balance buys nothing.
BASE_Y0      = BOWL_CY - 68 - BASE_FRONT_EXT;  // plate front edge (-208)
BASE_Y1      = 60;                             // plate back edge
BASE_SEAM_Y  = -32;    // the cut. front tile 176mm long, back tile 92mm
SPLICE_X     = 50;     // splice boss offset from centre (X)
SPLICE_W     = 24;     // splice boss width  (X)
SPLICE_L     = 15;     // splice boss reach EACH SIDE of the seam (Y)
SPLICE_H     = 10;     // splice boss height. 10 not 6 (the rim height) so the
                       // bolt has 2.5mm under it and 5mm over it; at rim height
                       // a 3.4mm hole would have left 1.3mm walls.
SPLICE_Z     = STAND_BASE_Z + SPLICE_H/2;      // bolt axis
// Bolt: M3 x 22 socket cap -- 4mm of head in the counterbore, 11mm of clearance
// through the rest of the front boss, 11mm of thread into the back boss's 13mm
// pilot. Do not go to 25: 11 + 14 would bottom out in that pilot.

// ---- 2. how the towers bolt down -------------------------------------------
// Each tower leg gets a FLANGE: the bottom FOOT_T of the leg spread out into a
// pair of screw ears. It sits ON the plate's floor, so no tower moves -- the
// flange only adds material beside a leg that already reached this low. Screws
// go UP from under the plate, countersunk so the plate still sits flat.
// Countersunk, not socket cap, deliberately: a 6mm counterbore 3mm deep in a
// 5mm floor leaves 2mm; a 90deg csk sinks 1.4mm and leaves 3.6mm.
FOOT_T       = 9;      // flange thickness. Deep enough for 7mm of M3 thread.
                       // Two screws per foot, so the screws themselves stop it
                       // rotating -- no separate dowel or spigot is needed.
                       // (Footprint and screw spread are per-tower; see FEET_ALL.)
                       // 9 not 8: the pilot has to stay 7 deep (an M3x12 through
                       // a 5mm floor protrudes exactly 7, and a shallower hole
                       // would bottom the screw out before it pulled tight), so
                       // at FOOT_T=8 the blind end was capped by just 1.1mm --
                       // the sort of thing JLCPCB's DFM check flags (it is under
                       // their 1.2mm FDM minimum, and only barely over the 1.0mm
                       // MJF one, which +/-0.3 would then eat). 9 caps it at
                       // 2.0mm, which survives the tolerance either way. The
                       // extra millimetre is free: the flange only has to clear
                       // the plate's 6mm rim, and the nearest thing above any of
                       // the six is the chute floor, 70mm up.
FOOT_FIT     = FIT;    // per-side clearance of the plate's relief pocket. Both
                       // halves printed, so it takes FIT like any other. It is
                       // the least critical of them -- the pocket is a relief in
                       // the open tray floor, not a locating feature, and the
                       // screws do the positioning -- but there is no reason for
                       // it to disagree with the rest of the file.
FOOT_SCREW   = 3.6;    // M3 clearance through the plate. 3.6 not 3.4: this is a
                       // clearance hole, not a fit, so it does not take FIT (0.6
                       // per side would be a 4.2 hole slopping around an M3). But
                       // 3.4 minus MJF's 0.3 is 3.1 against a 3.0 screw, which is
                       // a hole you have to persuade the screw through. 3.6 gives
                       // 0.3 per side nominal and still clears at the bad corner.
FOOT_PILOT   = 2.5;    // M3 self-tap pilot in the flange. Unchanged, but know
                       // what it is on nylon: +/-0.3 means 2.2..2.8 in practice.
                       // PA12 is tough and takes a thread far better than PLA, so
                       // 2.2 will cut rather than split; 2.8 is the weak end. If a
                       // foot ever strips, the fix is an M3 heat-set insert -- do
                       // not open the hole chasing it.
FOOT_CSK     = 6.4;    // 90deg countersink major dia under the plate. Sinks
                       // (6.4-3.6)/2 = 1.4mm into the 5mm floor, leaving 3.6.

// ONE list of feet, shared by the plate and by the towers, so the holes and the
// flanges can never drift apart. Each entry is
//     [cx, cy, width(X), length(Y), screw dx, screw dy]
// The three towers need three different footprints -- a single global flange
// size was tried and it was wrong twice over:
//
//  * THE A-FRAME IS ONE FOOT, NOT TWO. Its "two legs" only become two 6mm above
//    the plate: the arch cut starts at STAND_BASE_Z + 6, so the bottom is a
//    solid bar the full SUPP_W (X -42..42, 8 deep in Y). Two small flanges at
//    X +/-36 left the middle 57mm of that bar resting straight on the floor,
//    coplanar -- which CGAL merges, so the "separate" support exported fused to
//    the plate. The tell was a connected-component count on the stand preview
//    coming back 2 instead of 6. The foot is therefore the whole bar.
//  * THE CHUTE'S BACK LEGS SAT INSIDE THE BACK A-FRAME. They were at Y 35..39
//    and that bar spans Y 37..45, a real volumetric overlap -- invisible while
//    both were fused into the plate, fatal once they are separate prints. The
//    legs move forward into the clear span between the two A-frames (Y 6..33
//    once both pads are placed) and their tops now follow the trough's slope.
//
// Resulting Y ranges, which is what has to be checked whenever any of these
// move:  ped pad -76..-50 | splice bosses -47..-17 | front pad -6..10 |
//        chute pad 13..29 | back pad 37..53.  Smallest gap 3mm (front pad to
//        chute pad); the back pad ends 7mm short of the plate's back edge.
SUPP_FOOT_L  = 16;     // A-frame pad depth (Y); the bar itself is 8
SUPP_FOOT_X  = 34;     // A-frame screw spread from centre (X)
// The pad is NOT centred on the frame, and that is a print decision, not a
// structural one. The frame is SAD_T thick and centred on yc; a pad centred on
// yc too would stand 4mm proud on BOTH faces, so laying the A-frame down to
// print it would leave the frame floating 4mm off the bed -- and upright it is
// a 167mm tower on a 16mm-deep footprint. Offsetting the pad by SAD_T/2 makes
// its -Y face coplanar with the frame's, so the whole part lies flat on that
// one plane (see the PRINT POSES block near the bottom). Nothing else cares:
// the pad lives inside the plate's tray pocket, which is already open there, so
// the only thing that moves with it is its own pair of screw holes.
SUPP_FOOT_DY = SAD_T / 2;
CHUTE_LEG_Y  = 17;     // chute leg front edge
CHUTE_LEG_L  = 8;      // chute leg depth (Y)
FOOT_SUP     = [ for (yc = [SUPP_Y1, SUPP_Y2])
                     [0, yc + SUPP_FOOT_DY, SUPP_W, SUPP_FOOT_L, SUPP_FOOT_X, 0] ];
FOOT_CHU     = [ for (sx = [-16, 16]) [sx, CHUTE_LEG_Y + CHUTE_LEG_L/2, 14, 16, 0, 5] ];
// Only the "back" layout is split. mount_front() keeps its old fused foot --
// the front layout is legacy (the whole camera aim solve is done for "back")
// and splitting it would put screw holes under the bowl.
FOOT_PED     = (LAYOUT == "front") ? [] : [ for (sx = [-48, 48]) [sx, -63, 14, 26, 0, 9] ];
FEET_ALL     = concat(FOOT_SUP, FOOT_CHU, FOOT_PED);

// bolt-down flange grown at the base of a tower leg (part of the TOWER)
module tower_foot(f) {
    difference() {
        translate([f[0] - f[2]/2, f[1] - f[3]/2, STAND_BASE_Z]) cube([f[2], f[3], FOOT_T]);
        for (s = [-1, 1])
            translate([f[0] + s*f[4], f[1] + s*f[5], STAND_BASE_Z - 0.1])
                // The hole starts 0.1 BELOW the flange (an epsilon, so the two
                // faces are not coplanar), so it needs 7 + 0.1 of length to end
                // up exactly 7 deep measured from the seating face -- which is
                // what an M3x12 through a 5mm floor actually needs. Cap = 2.0.
                cylinder(d = FOOT_PILOT, h = (FOOT_T - 2) + 0.1, $fn = 24);
    }
}
// relief pocket in the PLATE so a flange seats flat on the floor -- and, just as
// important, so the flange never ends up COPLANAR with the floor, which reads as
// one solid. Over most of the tray it cuts only the 0.01mm sliver; it earns its
// keep where a flange lands on the bowl ring -- the pedestal feet at X +/-48 sit
// on the ring's rear arc, so the ring is notched for them. That is fine: the
// flange bolted into the notch locates the bowl there instead.
module foot_pocket(f) {
    translate([f[0] - f[2]/2 - FOOT_FIT, f[1] - f[3]/2 - FOOT_FIT, STAND_BASE_Z - 0.01])
        cube([f[2] + 2*FOOT_FIT, f[3] + 2*FOOT_FIT, 12]);
}
// the matching countersunk clearance pair through the plate floor
module foot_holes(f) {
    for (s = [-1, 1])
        translate([f[0] + s*f[4], f[1] + s*f[5], 0]) {
            translate([0, 0, STAND_BASE_Z - STAND_BASE_T - 1])
                cylinder(d = FOOT_SCREW, h = STAND_BASE_T + 4, $fn = 24);
            translate([0, 0, STAND_BASE_Z - STAND_BASE_T - 0.01])
                cylinder(d1 = FOOT_CSK, d2 = FOOT_SCREW,
                         h = (FOOT_CSK - FOOT_SCREW) / 2, $fn = 24);
        }
}

// cylinder lying along +Y (rotate([-90,0,0]) maps the +Z axis onto +Y)
module y_cyl(x, z, y0, y1, d) {
    translate([x, y0, z]) rotate([-90, 0, 0]) cylinder(d = d, h = y1 - y0, $fn = 24);
}
module splice_boss(front) {
    for (s = [-1, 1])
        translate([s * SPLICE_X - SPLICE_W/2,
                   front ? BASE_SEAM_Y - SPLICE_L : BASE_SEAM_Y, STAND_BASE_Z])
            cube([SPLICE_W, SPLICE_L, SPLICE_H]);
}
module splice_bolt_voids() {
    for (s = [-1, 1]) {
        y_cyl(s*SPLICE_X, SPLICE_Z, BASE_SEAM_Y - SPLICE_L - 0.1,
              BASE_SEAM_Y - SPLICE_L + 4, 6.5);                      // cap head
        y_cyl(s*SPLICE_X, SPLICE_Z, BASE_SEAM_Y - SPLICE_L + 3.9,
              BASE_SEAM_Y + 0.1, FOOT_SCREW);                        // clearance
        y_cyl(s*SPLICE_X, SPLICE_Z, BASE_SEAM_Y - 0.1,
              BASE_SEAM_Y + SPLICE_L - 2, FOOT_PILOT);               // pilot
    }
}

// The whole base plate before it is cut -- floor + perimeter rim + bowl ring +
// splice bosses, with every tower's relief pocket and screw pair already taken
// out. base_front()/base_back() just intersect this with a half-space, so the
// two tiles can never disagree about a hole.
module base_plate_solid() {
    difference() {
        union() {
            difference() {
                translate([-82, BASE_Y0, STAND_BASE_Z - STAND_BASE_T])
                    cube([164, BASE_Y1 - BASE_Y0, STAND_BASE_T + 6]);
                // tray pocket. Cut at exactly STAND_BASE_Z (it used to start
                // 0.5mm higher, which left the floor top at -103.5 while every
                // tower's foot started at -104 -- harmless while they were
                // fused, a 0.5mm rock once the towers bolt on).
                translate([-76, BASE_Y0 + 6, STAND_BASE_Z])
                    cube([152, (BASE_Y1 - 6) - (BASE_Y0 + 6), 10]);
            }
            translate([0, BOWL_CY, STAND_BASE_Z])       // bowl locating ring
                difference() {
                    cylinder(d = BOWL_D + 12, h = 5, $fn = 96);
                    translate([0, 0, -0.1]) cylinder(d = BOWL_D + 2, h = 6, $fn = 96);
                }
            splice_boss(true);
            splice_boss(false);
        }
        for (p = FEET_ALL) { foot_pocket(p); foot_holes(p); }
        splice_bolt_voids();
    }
}
module base_half(front) {
    intersection() {
        base_plate_solid();
        translate([-100, front ? BASE_Y0 - 10 : BASE_SEAM_Y,
                   STAND_BASE_Z - STAND_BASE_T - 10])
            cube([200, front ? BASE_SEAM_Y - (BASE_Y0 - 10)
                             : (BASE_Y1 + 10) - BASE_SEAM_Y, 40]);
    }
}
module base_front() { base_half(true);  }   // 164 x 176 x 15, bowl end
module base_back()  { base_half(false); }   // 164 x  92 x 15, column end

// A-frame support: a top cup that the round end plate drops into, on two legs
// that splay to the base with an OPEN centre -- so nothing walls off the bowl,
// and the chute passes forward through the front one.
// Printed as part="support" (render it at yc=0 and print TWO -- the two are the
// same part, only translated).
module stand_support(yc) {
    tower_foot([0, yc + SUPP_FOOT_DY, SUPP_W, SUPP_FOOT_L, SUPP_FOOT_X, 0]);
    translate([0, yc + SAD_T / 2, 0])
    rotate([90, 0, 0])
    linear_extrude(SAD_T)
    difference() {
        polygon([[-SUPP_W/2, STAND_BASE_Z], [SUPP_W/2, STAND_BASE_Z],
                 [SAD_R + 8, SAD_R + 8], [-(SAD_R + 8), SAD_R + 8]]);
        circle(r = SAD_R, $fn = 72);                                   // the cup
        translate([-0.72 * SAD_R, 0]) square([1.44 * SAD_R, SAD_R + 40]); // drop-in slot
        // arch: open the lower centre, leaving two legs
        translate([-30, STAND_BASE_Z + 6]) square([60, (SAD_R + 6) - (STAND_BASE_Z + 6)]);
    }
}

// chute: catches food dropping from the outlet and slides it FORWARD, out
// through the front A-frame, and drops it near the BOWL CENTRE (so kibble does
// not bounce off the near rim). A back lip closes the gap to the outlet mouth.
// The delivery end is TIED TO THE BOWL (BOWL_CY + 52 lands it 13mm inside the
// near rim). Each time the bowl moves forward the run gets longer and the slope
// would flatten -- too shallow, kibble sits -- so the front end drops to match.
// With the bowl at -122 the run is 104mm and the floor drops from z=-34 to -70:
// 36mm, 19.1deg. The binding check is NOT the far end but where the chute crosses
// the bowl's NEAR RIM (Y=-57, rim top z=-72): the floor is at z=-66.2 there, 5.8mm
// clear. The front end can sit as low as -70 because it is 52mm off the bowl
// centre, where the dish's inner wall is way down at z=-95 -- the rim is nowhere
// near it. This drop is only affordable because DROP_GAP went 22 -> 40.
// ---- CLEARING THE DRUM (this cost a rebuild -- do not undo it) --------------
// The chute used to be tucked UP into the outlet: floor top at z=-31, rails 15mm
// tall all the way to the back. Both fouled the feeder, confirmed by CGAL
// (chute INTERSECT housing = 36 x 29 x 9.3mm at Y 9.96..39; chute INTERSECT rotor
// = a 3 x 1.5 x 1.4mm bite at Y 37..38.5). Three separate causes:
//   1. the trough is 36mm wide (X +/-18) but the outlet slot is only 34 (X +/-17),
//      so the outer 1mm strip each side rode into the drum's bottom wall;
//   2. 15mm rails reached z=-18, straight through the wall (which at x~17..18
//      spans z -27..-23.5) and on into the bore;
//   3. the back lip sat at Y 37..39.5, but the slot STOPS at Y=36 -- it was under
//      solid rim, and clipped a rotor vane.
// The fix keeps the trough full width and instead puts the whole floor BELOW the
// drum: the drum's lowest point is z=-32 (x=0), so a floor topping out at -34 is
// clear at every x, and 2mm is far too small for kibble to escape sideways.
// The rails then taper -- short at the back where the drum curves down over them,
// full height once they are forward of it. The binding point is a rail's INNER
// top corner at (x=16, z=-29.5): radius 33.56 vs the drum's 32, so 1.56mm clear.
// The separate back lip is GONE. It was first moved behind Y=39.5, "past the end
// of the drum where it can rise freely" -- which was wrong, because the drum is
// not the widest thing on that axis: the END PLATES are PLATE_R (r36 now, r34 at
// the time) and the idle one occupies Y 39..43. The lip and the back rails drove
// straight into it, and the floor top at z=-34 came out exactly TANGENT to the
// plate's rim -- so watch this seam if PLATE_R ever grows again. (That is
// what a housing-only clearance probe cannot see, and why the tell was CGAL
// reporting "Simple: no" on the assembled preview rather than any intersection
// test coming back non-empty -- a tangency has zero volume.)
// No lip is needed anyway: now that the floor runs BELOW the drum rather than
// tucked up inside the outlet, the drum's own bottom wall closes the back of the
// trough. Anything that bounces rearward hits that curve and drops back onto the
// floor. So the trough simply STOPS at Y=38 -- 2mm past the outlet's back edge,
// 1mm short of the idle plate.
CHUTE_FY = BOWL_CY + 52;   // chute delivery point (front end), follows the bowl
CHUTE_FZ = -72;            // front-end height: sets the slope (19.6deg)
CHUTE_BZ = -36;            // back-end floor underside; +2 for the floor -> top -34.
                           // The drum's lowest point is z=-32, so this is the 2mm
                           // vertical gap the whole clearance argument rests on. It
                           // was -35 (1mm) at first, which CGAL passed but is too
                           // mean for two SEPARATELY PRINTED parts that only meet
                           // through the A-frame -- the tolerance stack over a
                           // 270mm assembly eats it. 2mm is still far too small for
                           // kibble to squeeze out sideways, and the trough is
                           // wider (36) than the outlet (34) anyway.
CHUTE_RB = 6.5;            // rail height at the BACK -- limited by the drum's curve
// underside of the trough floor at a given Y (the floor is 2mm thick, so the
// top surface is this + 2). Used to slope the legs' tops to match.
function chute_uz(y) = CHUTE_BZ + (36 - y) / (36 - CHUTE_FY) * (CHUTE_FZ - CHUTE_BZ);
module chute() {
    color("Silver") {
        // Sloped trough FLOOR. The back edge runs to Y=38 so it underlies the
        // WHOLE outlet (world-Y 3..36) and 2mm past it -- the old back at Y=22 let
        // the rear third of every pocket-dump spill behind the chute. It must not
        // go further: the idle END PLATE (r=34) starts at Y=39.
        hull() {
            translate([-18, 36, CHUTE_BZ]) cube([36, 2, 2]);         // back, under the outlet
            translate([-18, CHUTE_FY, CHUTE_FZ]) cube([36, 2, 2]);   // front, launches into the bowl
        }
        for (sx = [-18, 16])                                 // side rails (trough walls)
            hull() {
                translate([sx, 36, CHUTE_BZ]) cube([2, 2, CHUTE_RB]);
                translate([sx, CHUTE_FY, CHUTE_FZ]) cube([2, 2, 15]);
            }
        // Two legs down to the base plate. They used to be 4x4 posts at Y 35..39
        // starting 4mm BELOW the floor top, buried in the plate -- which put
        // them inside the BACK A-frame's bottom bar (Y 37..45). Harmless while
        // both were fused into the plate; a hard collision once each is its own
        // print. They now stand in the clear span between the two A-frames, and
        // because the trough is sloping there the leg top has to slope with it
        // (a flat top would either gap under the floor or push a bump up into
        // the trough).
        for (sx = [-18, 14])
            hull() {
                translate([sx, CHUTE_LEG_Y, STAND_BASE_Z]) cube([4, CHUTE_LEG_L, 1]);
                translate([sx, CHUTE_LEG_Y, chute_uz(CHUTE_LEG_Y) - 1])
                    cube([4, 0.5, 1.5]);
                translate([sx, CHUTE_LEG_Y + CHUTE_LEG_L - 0.5,
                           chute_uz(CHUTE_LEG_Y + CHUTE_LEG_L) - 1]) cube([4, 0.5, 1.5]);
            }
        for (f = FOOT_CHU) tower_foot(f);
    }
}

// PREVIEW ONLY -- this is the assembled stand, five printed parts screwed
// together, and it is 164 x 268 x 172 so it cannot be printed as one piece.
// The printable parts are base_front, base_back, support (x2), chute, pedestal.
// The two base tiles are drawn in different tans so the seam is visible.
module stand() {
    color("Tan")       base_front();
    color("BurlyWood") base_back();
    color("Tan") {
        stand_support(SUPP_Y1);
        stand_support(SUPP_Y2);
    }
    chute();
    if (LAYOUT == "front") mount_front(); else mount_back();
}

// "back" layout: a short PEDESTAL that carries the wedge stand. Two posts rise
// from the base plate BESIDE the bowl -- they stand at X = +/-48, out where the
// round dish has already curved away, so they can be much further FORWARD than
// the bowl's centreline rim would allow -- then two side rails cantilever
// forward over the bowl at PED_TOP. The wedge stand's flat feet (at X = +/-48.8)
// land on those rails; the CENTRE is left open so the camera's angled line of
// sight -- and the GPIO/servo wire out of the header slot -- drop clear.
module mount_back() {   // printable pedestal only (the wedge stand is a separate part)
    PX   = 48;   // rail/post offset from centre (X) -- directly under the wedge feet
    PW   = 12;   // rail/post width
    PY0  = -68;  // post front face. The posts stand at X = +/-48 (X 42..54), and
    PY1  = -58;  // at X=42 the bowl's outer cone only reaches back to Y=-72.4, so
                 // -68 clears the dish by 4.4mm. Standing them THIS far forward
                 // (rather than back by the A-frame) shortens the forward
                 // cantilever to 80mm -- less than the 94mm of the previous,
                 // validated design, even though the wedge moved 50mm forward.
                 // They merge into the bowl's locating ring at the bottom.
    RY0  = WEDGE_Y - 34;   // rail front end = the wedge's front foot
    color("DimGray")
        for (s = [-1, 1]) {
            px = s * PX;
            // post up from the base plate, standing behind the bowl
            translate([px - PW/2, PY0, STAND_BASE_Z]) cube([PW, PY1 - PY0, PED_TOP - 4 - STAND_BASE_Z]);
            // bolt-down flange. Its footprint lands on the bowl ring's rear arc,
            // so base_plate_solid() notches the ring to let it seat -- see
            // foot_pocket().
            tower_foot([px, (PY0 + PY1) / 2, 14, 26, 0, 9]);
            // rail cantilevering forward over the bowl; runs BACK over the post
            // top (to PY1) so it is a lap joint, not a weak butt joint
            translate([px - PW/2, RY0, PED_TOP - 4]) cube([PW, PY1 - RY0, 4]);
        }
    // LOW TIE BAR between the two posts. Needed the moment the pedestal became
    // its own printed part: the posts have nothing else joining them (they used
    // to meet only through the base plate), so without this it exports as TWO
    // loose pieces -- each an 80mm post on two M3 screws 18mm apart, carrying a
    // 80mm forward cantilever. CGAL says it plainly: volumes=3, i.e. 2 solids.
    // It has to stay LOW and to the REAR, and the DISH is what pins it. The bowl
    // is a cone opening upward from r=46.8 at the plate to r=65 at its rim, so
    // its rear edge at X=0 walks backward as you go up:
    //     z = -115  ->  Y = -73.7        z = -105  ->  Y = -69.0
    //     z =  -97  ->  Y = -65.9        z =  -90  ->  Y = -63.6
    // The tie's front face is at Y=-66, so anything above z=-96.7 cuts into the
    // dish. It lives at STAND_BASE_Z+7 .. +17 (z -115..-105): 2mm over the bowl
    // locating ring below it, 3mm behind the dish above it. (Written against
    // STAND_BASE_Z, not as a raw number -- an earlier draft hardcoded z=-97 by
    // misreading STAND_BASE_Z as -104 when it is -122, which put the bar
    // straight through the bowl. CGAL will not catch that: two solids that
    // overlap in a union just merge.)
    // The other neighbours are not close: the chute floor is 34mm over the tie
    // where it crosses Y=-66, and the camera sightline runs Y -104..-142, well
    // forward. Do NOT tie the posts at the TOP -- the open centre up there is
    // what lets the camera see past and the servo lead drop through.
    // The dish clearance is only 3mm and the bowl here is a ~13cm stand-in, so
    // check it against the real dish before printing.
    color("DimGray")
        translate([-(PX + PW/2), -66, STAND_BASE_Z + 7])
            cube([2*(PX + PW/2), 12, 10]);
}

// "front" layout: a stout post rises from the extended front platform and carries
// the display facing the owner; the board's back camera looks BACK-and-down at
// the bowl. The post is wide (not a thin arm) so it is rigid.
module mount_front() {  // printable front-post frame only (no board/electronics)
    color("DimGray") {
        // wide front post from the base up to the display
        translate([-16, CAM_F_Y - 4, STAND_BASE_Z])
            cube([32, 12, CAM_F_Z - STAND_BASE_Z]);
        // foot spreading onto the platform for stability
        translate([-24, CAM_F_Y - 10, STAND_BASE_Z]) cube([48, 26, 8]);
    }
}

// The seated board cradle, placed + tilted over the bowl for the chosen layout.
// PREVIEW ONLY (screen/camera mock-up) -- printed as part="board_cradle", flat.
module screen_holder_placed() {
    if (LAYOUT == "front")
        translate([0, CAM_F_Y, CAM_F_Z]) rotate([CAM_F_TILT, 0, 0]) screen_holder();
    else
        translate([0, CAM_MOUNT_Y, CAM_MOUNT_Z]) rotate([CAM_TILT, 0, 0]) screen_holder();
}

// The holder shown over the bowl is the REAL printable cradle (board_cradle:
// posts, centred camera window, GPIO/servo slot) with the board seated in it --
// so the preview matches what actually gets printed, the "board cradle stand"
// part. We drive it with the arm's CAM_TILT (which already aims the camera at the
// pet's face); the cradle's own BM_TILT is only for standing it alone on a desk,
// so we do NOT stack it here. Screen faces up toward the owner (+Z/+Y), camera
// looks down the window toward the pet (-Z/-Y).
module screen_holder() {
    color("DimGray") board_cradle();
    // board seated on the standoffs
    translate([0, 0, BM_BASE_T + BM_STANDOFF]) {
        color("Gainsboro")      translate([-BM_W/2, -BM_H/2, 0]) cube([BM_W, BM_H, 6]);  // board
        color([0.08,0.08,0.09]) translate([-40, -22, 6]) cube([80, 44, 1.2]);            // SCREEN (top, owner side)
    }
    // camera barrel on the BACK of the board, pointing down the window at the pet
    color([0.85,0.15,0.15])
        translate([0, 0, BM_BASE_T + BM_STANDOFF - 3]) cylinder(d = 10, h = 3, $fn = 28);
}

// concept-only representation of the display + camera head, drawn FLAT:
// SCREEN on the top face (+Z, the owner looks down at it), CAMERA on the bottom
// face (-Z, looks down at the bowl). They are on OPPOSITE faces because that is
// how the Waveshare board is built -- which is exactly why the mount is
// "screen-up / camera-down". The caller positions + tilts it over the bowl.
module screen_head() {
    color("Gainsboro")       translate([-49, -33, 0]) cube([98, 66, 6]);      // board
    color([0.08,0.08,0.09])  translate([-40, -26, 6]) cube([80, 52, 1.2]);    // SCREEN (top)
    color([0.85,0.15,0.15])  translate([0, 0, -1.8]) cylinder(d = 11, h = 2, $fn = 28); // CAMERA (bottom -> bowl)
}

// ---- preview aids (only drawn in part="product"; NEVER printed) ------------
// The camera's optical axis, drawn as a red ray from the lens down to the pet's
// face target -- so you can SEE where the camera looks and dial PET_FACE_* /
// CAM_MOUNT_* until the ray lands on the head marker's face.
module camera_sightline() {
    color([1, 0.15, 0.15, 0.9]) hull() {
        translate(CAM_ACT - [0, 0, 6]) sphere(1.4);   // lens
        translate(PET_ACT) sphere(1.4);               // face target
    }
}
// translucent stand-in for the pet's head, sitting at the aim point over the bowl.
module pet_head_marker() {
    color([0.55, 0.55, 0.60, 0.30])
        translate(PET_ACT) sphere(r = 26, $fn = 40);
}

module product() {          // whole appliance preview (do NOT print)
    assembly();
    stand();                // structural stand (frame + base + chute + pedestal)
    // the board: "front" rides a front post; "back" rides the 30-degree wedge
    // stand on the pedestal, rotated so the camera faces the bowl (-Y) and the
    // screen faces the owner at the back (+Y).
    if (LAYOUT == "front")
        screen_holder_placed();
    else
        translate([0, WEDGE_Y, PED_TOP]) rotate([0, 0, 180]) board_cradle_stand(true);
    color("BurlyWood") bowl();
    pet_head_marker();
    camera_sightline();
}

// ============================================================================
// PRINT POSES
//
// Everything above is modelled in the ASSEMBLY frame -- where a part sits in the
// finished machine, not how it is sliced. Those are not the same thing, and for
// two parts the difference is the difference between printable and not:
//
//   * THE HOPPER HAS NO BOTTOM FACE. The saddle groove is a r=32.3 cylinder
//     centred 25mm below the neck's underside, so it eats up to |x| = 20.45 --
//     and the neck is only 40 wide, |x| <= 20. Nothing at all survives at z=0.
//     Printed as modelled it starts on air. Flipped mouth-down it lands on a
//     3mm-wide annulus at r~45, the funnel walls taper INWARD at 21.8deg from
//     vertical (self-supporting, inside and out), and the saddle becomes an
//     open pocket facing the sky.
//   * THE A-FRAME IS A 167mm TOWER ON A 16mm FOOTPRINT. Laid on its face it is
//     90 x 167 with the whole frame on the bed -- but only because the foot pad
//     is offset to sit flush with it; see SUPP_FOOT_DY.
//
// The stand parts also sit 122..127mm BELOW z=0 in the assembly frame, which
// some slicers refuse to load at all. Every pose below therefore ends with a
// drop onto z=0, written against the symbols (STAND_BASE_Z etc.) rather than as
// measured numbers, so it follows if the machine's geometry moves.
//
// X/Y are left where they are -- slicers arrange the plate themselves, and
// hard-coding a centring offset is just one more number that can go stale.
//
// Measured off the exported STLs with PRINT_POSE on. Every one sits on z=0 and
// every one fits a 220 x 220 x 250 bed; the tallest is the pedestal at 102 and
// the longest is the A-frame at 167.
// The "support?" column is FDM ADVICE and is kept only for that case. On MJF
// there are no supports at all -- it is a powder bed -- and JLC nest the parts
// themselves, so on the chosen process these poses are for inspection, not a
// build instruction. They still earn their place: they are what proves each
// part CAN be made on a filament machine if it ever has to be.
//   part                 pose                   bed (mm)            support? (FDM)
//   rotor                as modelled            50.34 x 56.40 x 38  no
//   housing              as modelled, bore up   64 x 64 x 39.20     no (34mm bridges)
//   plate_stepper        as modelled, flat      72 x 72 x 4         no
//   plate_idle           as modelled, stub up   72 x 72 x 11        no
//   hopper               FLIPPED, mouth down    96 x 96 x 80.72     neck corners only
//   board_cradle         as modelled, Z-up     100.71 x 69.27 x 33.50  no (45deg ramps)
//   board_cradle_stand   as modelled, flat cut 100.71 x 81.38 x 80.96  YES, under the tray
//   base_front           floor down             164 x 176 x 15      no
//   base_back            floor down             164 x 92 x 15       no
//   support  (PRINT 2)   LAID ON ITS FACE       90 x 167 x 16       no
//   chute                pads down              46 x 108 x 92.5     YES, under trough
//   pedestal             posts up               110 x 88 x 102      YES, under rails
//
// The three that want support all want it for the same honest reason: each
// carries a deliberately sloped or cantilevered surface, and no rotation puts it
// on the bed without standing some other feature in the air. In all three the
// overhang has clear line of sight down to the plate, so "support on build plate
// only" is enough -- none of them needs support-everywhere.
//   * chute -- it is a bridge on two stilts, and that is the whole difficulty.
//     The trough underside is a 19.6deg plane 104mm long; the legs hang off that
//     same underside at Y 17..25, so putting the floor on the bed would bury
//     them in it. Any pose that rests on the two pads leaves the trough in the
//     air -- as modelled it cantilevers ~87mm forward of the legs, its front tip
//     50mm up. The one alternative, inverting it so the rails' free edges take
//     the bed, trades that for 2mm-wide bed contact under a 92mm part. Not worth
//     it: print it as modelled and support the trough. The support marks land on
//     the underside, where nothing sees them.
//   * pedestal -- the rails cantilever 80mm forward at PED_TOP-4, and the tie
//     bar spans 96mm at z=7..17. Both are unsupported from below. The overhangs
//     ARE the design: the centre has to stay open for the camera's sightline, so
//     the rails have nothing under them by definition. (Flipping it rails-down
//     gives a much better 2 x 12 x 80 bed contact, but then the 96mm tie bar has
//     to bridge at 85mm up between two free-standing posts. Predictable supports
//     beat a hopeful bridge.)
//   * board_cradle_stand -- the tray is tilted BM_TILT=30deg, i.e. 60deg from
//     vertical, over an open centre. Same reason: the opening is the point.
// ============================================================================
module posed(rot = [0, 0, 0], mv = [0, 0, 0]) {
    if (PRINT_POSE) translate(mv) rotate(rot) children();
    else children();
}

if      (part == "rotor")        rotor();
else if (part == "housing")      housing();
else if (part == "plate_stepper") plate_stepper();
else if (part == "plate_servo")  plate_servo();      // legacy, unused
else if (part == "plate_idle")   plate_idle();
// flip mouth-down -- see PRINT POSES above; neck-down there is no bottom face
else if (part == "hopper")       posed([180, 0, 0], [0, 0, NECK_H + HOPPER_H]) hopper();
else if (part == "board_cradle") board_cradle();
else if (part == "board_cradle_stand") board_cradle_stand();   // already cut flat at z=0
else if (part == "stand")        stand();       // preview only -- 268mm, will not fit
// The stand, split for the bed. base_front + base_back bolt together at
// BASE_SEAM_Y with 2x M3x22 socket cap. support/chute/pedestal then bolt DOWN
// through the plate with 2x M3x12 countersunk per foot: 6 feet (one per
// A-frame, two under the chute, two under the pedestal) = 12 screws.
// Assembly order: join the two tiles first, then drop the towers on -- the
// splice bosses are inside the tray and a fitted tower is in the way.
else if (part == "base_front")
    posed([0, 0, 0], [0, 0, STAND_BASE_T - STAND_BASE_Z]) base_front();
else if (part == "base_back")
    posed([0, 0, 0], [0, 0, STAND_BASE_T - STAND_BASE_Z]) base_back();
// PRINT TWO. rotate([90,0,0]) maps the frame's -Y face (which the offset pad is
// flush with) onto the bed; +SAD_T/2 in Z is that face, +SAD_R+8 in Y is the top
// of the cup, which the rotation swings to -Y.
else if (part == "support")
    posed([90, 0, 0], [0, SAD_R + 8, SAD_T / 2]) stand_support(0);
else if (part == "chute")
    posed([0, 0, 0], [0, 0, -STAND_BASE_Z]) chute();
else if (part == "pedestal")
    posed([0, 0, 0], [0, 0, -STAND_BASE_Z])
        { if (LAYOUT == "front") mount_front(); else mount_back(); }
else if (part == "product")      product();
else                             assembly();
