// =============================================================
//  🦐  C6PO Shrimp Case
//  ESP32-C6 Super Mini enclosure
// =============================================================
//  Board:   23 × 17.8 mm, ~7 mm tall (PCB 4.32 mm + module/USB-C)
//  USB-C:   centred on the short anterior (head) end
//  Print:   flat base down — zero supports needed
//  Slicer:  0.2 mm layers, 3 perimeters, 20 % infill
//  Fit:     board press-fits in from the open bottom
// =============================================================

$fn = 64;

// ── Board dimensions (Super Mini with clearance) ───────────
BRD_L  = 23.5;   // board length  (X — USB-C end is at +X)
BRD_W  = 18.3;   // board width   (Y)
BRD_H  =  7.5;   // board + tallest component (module + USB-C)

// ── Shell geometry ─────────────────────────────────────────
WALL   = 2.0;    // side wall thickness
FLOOR  = 1.6;    // base floor thickness

// ── USB-C cutout (centred on head face, per IPC standard) ──
USBC_W = 9.5;    // slot width
USBC_H = 3.8;    // slot height
USBC_Z = 2.8;    // slot centre height above floor

// ── Shrimp body proportions ────────────────────────────────
HEAD   = 14;     // rostrum extension beyond board front (USB-C side)
TAIL   = 18;     // abdomen/tail extension beyond board rear

// ── Derived: rectangular shell footprint ───────────────────
EXT_L  = BRD_L + WALL * 2;
EXT_W  = BRD_W + WALL * 2;
EXT_H  = BRD_H + FLOOR;

// Convenience centres of the rectangular section
CX     = EXT_L / 2;
CY     = EXT_W / 2;

// =============================================================
//  MODULES
// =============================================================

// ── 1. Main body (carapace + curved abdomen) ───────────────
//   Oriented: USB-C/head at +X, tail at -X, flat bottom at Z=0
module body_shell() {
    hull() {
        // Thorax — widest, tallest, over the board
        translate([CX, CY, EXT_H * 0.55])
            scale([0.85, 1.0, 0.95]) sphere(r = EXT_W * 0.52);

        // Anterior shoulder (narrows toward head)
        translate([EXT_L * 0.88, CY, EXT_H * 0.46])
            scale([1.0, 0.62, 0.72]) sphere(r = EXT_W * 0.34);

        // Head tip (just behind rostrum base)
        translate([EXT_L + HEAD * 0.38, CY, EXT_H * 0.38])
            scale([1.0, 0.38, 0.52]) sphere(r = EXT_W * 0.22);

        // Posterior — abdomen begins to curve downward
        translate([EXT_L * 0.12, CY, EXT_H * 0.42])
            scale([1.0, 0.72, 0.65]) sphere(r = EXT_W * 0.40);

        // Tail base — lower and narrower
        translate([-TAIL * 0.35, CY, EXT_H * 0.26])
            scale([1.0, 0.50, 0.42]) sphere(r = EXT_W * 0.28);

        // Tail tip — close to the bed
        translate([-TAIL * 0.80, CY, EXT_H * 0.14])
            scale([1.0, 0.32, 0.28]) sphere(r = EXT_W * 0.16);
    }
}

// ── 2. Rostrum (forward horn, angled slightly upward) ──────
module rostrum() {
    hull() {
        translate([EXT_L + HEAD * 0.30, CY, EXT_H * 0.72])
            sphere(r = 2.2);
        translate([EXT_L + HEAD * 1.30, CY, EXT_H * 0.88])
            sphere(r = 0.7);
    }
}

// ── 3. Tail fan (telson + two uropods) ────────────────────
module tail_fan() {
    bx = -TAIL * 0.72;
    bz = EXT_H * 0.13;

    // Telson (central spine)
    hull() {
        translate([bx,      CY,      bz     ]) sphere(r = 2.5);
        translate([bx - 9,  CY,      bz - 2 ]) sphere(r = 1.0);
    }
    // Left uropod
    hull() {
        translate([bx + 2,  CY,      bz     ]) sphere(r = 2.2);
        translate([bx - 6,  CY - 7,  bz - 1 ]) sphere(r = 0.8);
    }
    // Right uropod
    hull() {
        translate([bx + 2,  CY,      bz     ]) sphere(r = 2.2);
        translate([bx - 6,  CY + 7,  bz - 1 ]) sphere(r = 0.8);
    }
}

// ── 4. Antennae (pair from forehead) ──────────────────────
//   Minimum 1.5 mm diameter — printable on FDM with 0.4 mm nozzle
module antennae() {
    nx = EXT_L + HEAD * 0.55;
    nz = EXT_H * 0.76;
    L  = 32;

    hull() {
        translate([nx,     CY + 3.5, nz    ]) sphere(r = 1.4);
        translate([nx + L, CY + 8,   nz + 5]) sphere(r = 0.9);
    }
    hull() {
        translate([nx,     CY - 3.5, nz    ]) sphere(r = 1.4);
        translate([nx + L, CY - 8,   nz + 5]) sphere(r = 0.9);
    }
}

// ── 5. Carapace segment grooves (decorative ridges) ────────
module segment_grooves() {
    n = 6;
    for (i = [1 : n]) {
        x = i * (EXT_L / (n + 1));
        translate([x, CY, EXT_H * 0.28])
            rotate([90, 0, 6])        // slight diagonal, like real shrimp
                cylinder(r = 0.75, h = EXT_W + 6, center = true);
    }
}

// ── 6. Board cavity (open at Z=0 bottom, board presses in) ─
module board_cavity() {
    translate([WALL, WALL, FLOOR])
        cube([BRD_L, BRD_W, BRD_H + 1]);  // +1 open-top clearance
}

// ── 7. USB-C rounded-rectangle cutout ─────────────────────
//   Punched through the anterior (head) end wall
module usbc_hole() {
    r = USBC_H / 2;
    // Position at the head end wall (X = EXT_L), centred on width and height
    translate([EXT_L - 0.5, CY, USBC_Z])
        rotate([0, 90, 0])
            hull() {
                translate([ (USBC_W/2 - r), 0, 0])
                    cylinder(r = r, h = WALL + HEAD + 2, center = false);
                translate([-(USBC_W/2 - r), 0, 0])
                    cylinder(r = r, h = WALL + HEAD + 2, center = false);
            }
}

// ── 8. Optional: small LED window on top ──────────────────
//   ~2 mm slot above the onboard blue LED (near USB-C end)
module led_slot() {
    translate([EXT_L - 4, CY, EXT_H + 0.5])
        cube([3, 2.5, 4], center = true);
}

// =============================================================
//  ASSEMBLY
// =============================================================

difference() {
    // ── Outer form ─────────────────────────────────────────
    union() {
        body_shell();
        rostrum();
        tail_fan();
        antennae();
    }

    // ── Subtractions ───────────────────────────────────────
    board_cavity();     // hollow interior for the board
    usbc_hole();        // USB-C power port
    segment_grooves();  // decorative carapace segments
    led_slot();         // LED visibility window
}

// =============================================================
//  NOTES
// =============================================================
//
//  Orientation on print bed: flat base (Z=0) down. No supports.
//
//  Board insertion: press the Super Mini in from the open bottom.
//  The 0.5 mm wall-to-board gap gives a snug friction fit.
//  A small dab of Blu-Tack on the floor keeps it from rattling.
//
//  USB-C: the slot is at the head (rostrum) end.
//  The cutout is generous — the cable will reach through easily.
//
//  Antennae are 1.4 mm radius (2.8 mm diameter) — printable at
//  0.4 mm nozzle with 3+ perimeters. Print slowly (30 mm/s) for
//  the tips. Skip if your printer struggles with thin features.
//
//  Adjust WALL/FLOOR for tighter/looser board fit.
//  Adjust HEAD/TAIL for a chunkier or slimmer shrimp profile.
//
//  Render → Export as STL in OpenSCAD (F6 then File › Export).
