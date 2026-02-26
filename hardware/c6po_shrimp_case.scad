// =============================================================
//  🦐  C6PO Shrimp Case  v2  —  emoji-accurate
//  ESP32-C6 Super Mini (23 × 17.8 mm)
// =============================================================
//  The shrimp lays on its side just like the 🦐 emoji:
//    • C-curved body (dorsal arc = back, concave belly inside)
//    • Long rostrum pointing forward from the head
//    • Segmented carapace with raised ridges
//    • Wide tail fan (telson + uropods) at the rear
//    • Walking legs on the ventral (belly) side
//    • Paired antennae from the forehead
//    • USB-C slot cut through the rostrum/head end
//    • Board cavity open at the flat base (press-fit)
//
//  Print: flat base down, no supports.
//  Open in OpenSCAD → F6 → File › Export as STL
// =============================================================

$fn = 64;

// ── Board (ESP32-C6 Super Mini) ───────────────────────────
BRD_L  = 23.5;   // board length (USB-C end = head = +X)
BRD_W  = 18.3;   // board width
BRD_H  =  7.5;   // board + tallest component height
WALL   =  2.0;   // shell wall
FLOOR  =  1.6;   // base floor

USBC_W =  9.5;   // USB-C slot width
USBC_H =  3.8;   // USB-C slot height
USBC_Z =  2.8;   // slot centre above floor

// ── Derived shell box ─────────────────────────────────────
EXT_L  = BRD_L + WALL * 2;       // 27.5 mm
EXT_W  = BRD_W + WALL * 2;       // 22.3 mm
EXT_H  = BRD_H + FLOOR;          //  9.1 mm

// ── Body curve arc (the C-shape) ─────────────────────────
//   Arc is in the XY plane (top view), centred at [ACX, ACY].
//   OUTER_R = dorsal (back) of shrimp.
//   INNER_R = ventral (belly) / concave side.
//   Arc sweeps ANG1 → ANG2 degrees.
//   Head = high angle (ANG2), Tail = low angle (ANG1).

ACX      =  0;
ACY      =  0;
OUTER_R  = 34;   // back of shrimp
INNER_R  = 13;   // belly of shrimp
ANG1     = 20;   // tail end angle (°)
ANG2     = 140;  // head end angle (°)
N        = 32;   // polygon smoothness

// ── Helper: 2D shrimp crescent outline ───────────────────
//   Outer arc + pointed ends + inner arc = emoji silhouette
module shrimp_2d_body() {
    outer = [for (i = [0 : N])
                let(a = ANG1 + (ANG2 - ANG1) * i / N)
                [ACX + OUTER_R * cos(a), ACY + OUTER_R * sin(a)]];

    inner = [for (i = [0 : N])
                let(a = ANG2 - (ANG2 - ANG1) * i / N)
                [ACX + INNER_R * cos(a), ACY + INNER_R * sin(a)]];

    // Taper the two ends to a point by using the arc midpoints
    // Head tip: between outer & inner at ANG2
    head_tip = [(OUTER_R + INNER_R) / 2 * cos(ANG2),
                (OUTER_R + INNER_R) / 2 * sin(ANG2)];

    // Tail tip: between outer & inner at ANG1
    tail_tip = [(OUTER_R + INNER_R) / 2 * cos(ANG1),
                (OUTER_R + INNER_R) / 2 * sin(ANG1)];

    polygon(concat(
        [outer[0]],        // start at tail outer
        outer,             // sweep outer arc to head
        [head_tip],        // pointed head
        inner,             // inner arc back to tail
        [tail_tip]         // pointed tail
    ));
}

// ── 2D → 3D body shell (slightly domed top using hull) ───
module body_3d() {
    hull() {
        // Base footprint (full width)
        linear_extrude(0.1)
            offset(r = 0)
                shrimp_2d_body();
        // Top face slightly inset and raised — creates dome
        translate([0, 0, EXT_H])
            linear_extrude(0.1)
                offset(r = -2)
                    shrimp_2d_body();
    }
}

// ── Rostrum ───────────────────────────────────────────────
//   Long pointed horn from the head. In the emoji it's the
//   prominent spike pointing forward-upward.
module rostrum() {
    // Base of rostrum at the head end of the body arc
    base_x = (OUTER_R + 2) * cos(ANG2);
    base_y = (OUTER_R + 2) * sin(ANG2);
    tip_x  = base_x + 22 * cos(ANG2);
    tip_y  = base_y + 22 * sin(ANG2);

    hull() {
        translate([base_x, base_y, EXT_H * 0.72])
            sphere(r = 2.8);
        translate([tip_x, tip_y, EXT_H * 0.82])
            sphere(r = 0.6);
    }
}

// ── Tail fan ──────────────────────────────────────────────
//   Telson (central spike) + 2 uropods (side blades).
//   Positioned at the tail end of the arc.
module tail_fan() {
    tx = (OUTER_R - 4) * cos(ANG1);
    ty = (OUTER_R - 4) * sin(ANG1);
    tz = EXT_H * 0.18;
    // Direction: tangent at ANG1, pointing away from body
    tang_a = ANG1 - 90;

    // Telson (central blade)
    hull() {
        translate([tx, ty, tz]) sphere(r = 3.0);
        translate([tx + 12 * cos(tang_a),
                   ty + 12 * sin(tang_a), tz - 2]) sphere(r = 1.0);
    }
    // Left uropod
    hull() {
        translate([tx, ty, tz]) sphere(r = 2.6);
        translate([tx + 9 * cos(tang_a + 22),
                   ty + 9 * sin(tang_a + 22), tz - 1]) sphere(r = 0.9);
    }
    // Right uropod
    hull() {
        translate([tx, ty, tz]) sphere(r = 2.6);
        translate([tx + 9 * cos(tang_a - 22),
                   ty + 9 * sin(tang_a - 22), tz - 1]) sphere(r = 0.9);
    }
}

// ── Carapace segments ─────────────────────────────────────
//   Raised ridges running perpendicular to the body arc —
//   the most recognisable feature of the 🦐 emoji.
//   Cut from the solid body using difference().
module segment_grooves() {
    seg_n = 7;
    for (i = [0 : seg_n - 1]) {
        // Step evenly along the arc
        a = ANG1 + (ANG2 - ANG1) * (i + 0.5) / seg_n;
        // Point on the outer arc
        px = ACX + (OUTER_R - 1) * cos(a);
        py = ACY + (OUTER_R - 1) * sin(a);
        // Normal to arc = radial direction; groove runs perpendicular
        groove_a = a + 90;   // tangent direction
        translate([px, py, -1])
            rotate([0, 0, groove_a])
                // thin wall that cuts across the body
                cube([1.4, (OUTER_R - INNER_R) * 2, EXT_H + 4], center = true);
    }
}

// ── Walking legs ──────────────────────────────────────────
//   3 pairs of small nubs on the ventral (inner/belly) side.
//   Purely decorative — makes it look like the emoji.
module walking_legs() {
    leg_n = 3;
    for (i = [0 : leg_n - 1]) {
        a = ANG1 + (ANG2 - ANG1) * (0.25 + i * 0.2);
        bx = ACX + (INNER_R - 0.5) * cos(a);
        by = ACY + (INNER_R - 0.5) * sin(a);
        bz = 0.5;
        leg_dir = a - 90;   // points away from body into belly
        leg_len = 7 + i * 1;

        // Leg pair: slightly spread left and right of the tangent
        for (spread = [-18, 18]) {
            hull() {
                translate([bx, by, bz]) sphere(r = 1.5);
                translate([bx + leg_len * cos(leg_dir + spread),
                           by + leg_len * sin(leg_dir + spread),
                           bz - 1.0]) sphere(r = 0.6);
            }
        }
    }
}

// ── Antennae ──────────────────────────────────────────────
//   Two long thin feelers from the forehead, like the emoji.
module antennae() {
    hx = (OUTER_R + 1) * cos(ANG2);
    hy = (OUTER_R + 1) * sin(ANG2);
    hz = EXT_H * 0.78;
    // Antennae trail backwards along the dorsal side
    ant_dir = ANG2 + 160;
    for (spread = [-12, 12]) {
        hull() {
            translate([hx, hy, hz]) sphere(r = 1.2);
            translate([hx + 30 * cos(ant_dir + spread),
                       hy + 30 * sin(ant_dir + spread),
                       hz + 3]) sphere(r = 0.5);
        }
    }
}

// ── Board cavity ──────────────────────────────────────────
//   Straight rectangular pocket; board press-fits in from below.
//   Positioned so the USB-C end aligns with the head of the arc.
module board_cavity() {
    // Head is near ANG2; place cavity with USB-C end toward head
    // Centre the cavity roughly in the middle of the crescent body
    arc_mid_a = (ANG1 + ANG2) / 2;
    // Place cavity so its USB-C end (+X) points toward ANG2
    cx = ACX + (OUTER_R + INNER_R) / 2 * cos(arc_mid_a) - (BRD_L / 2) * cos(ANG2);
    cy = ACY + (OUTER_R + INNER_R) / 2 * sin(arc_mid_a) - (BRD_L / 2) * sin(ANG2);

    translate([cx, cy, FLOOR])
        rotate([0, 0, ANG2])        // align long axis with head direction
            cube([BRD_L, BRD_W, BRD_H + 1]);
}

// ── USB-C cutout ──────────────────────────────────────────
//   Punched through the head wall toward the rostrum.
module usbc_hole() {
    // Head face centre
    hx = ACX + (OUTER_R + INNER_R) / 2 * cos(ANG2);
    hy = ACY + (OUTER_R + INNER_R) / 2 * sin(ANG2);
    hz = USBC_Z;
    r  = USBC_H / 2;

    translate([hx, hy, hz])
        rotate([0, 0, ANG2])
            rotate([0, 90, 0])
                hull() {
                    translate([ (USBC_W/2 - r), 0, 0])
                        cylinder(r = r, h = 16, center = true);
                    translate([-(USBC_W/2 - r), 0, 0])
                        cylinder(r = r, h = 16, center = true);
                }
}

// =============================================================
//  ASSEMBLY
// =============================================================

difference() {
    union() {
        body_3d();
        rostrum();
        tail_fan();
        antennae();
        walking_legs();
    }

    board_cavity();
    usbc_hole();
    segment_grooves();
}

// =============================================================
//  NOTES
// =============================================================
//  Top-view shape matches the 🦐 emoji: C-curved crescent,
//  rostrum at head, fan tail at rear, legs on belly side.
//
//  The board cavity is a straight rectangle inside the curved
//  body. If the cavity clips an outer wall (visible as a hole
//  in the side), increase WALL or reduce the cavity offset by
//  editing the cx/cy calculation in board_cavity().
//
//  Adjust OUTER_R / INNER_R to make the body fatter or slimmer.
//  Adjust ANG1 / ANG2 to change how much the body curves.
//
//  Antennae: 1.2 mm radius — print slow (25 mm/s) for the tips,
//  or set sphere() $fn=16 to thicken them slightly.
//
//  Render: F6 in OpenSCAD, then File › Export › Export as STL
