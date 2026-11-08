/* EVE-style printable desk robot, v2 -- body shell and model skeleton.

   Units are millimetres. Every dimension lives in the parameter block so the
   neck and arm parts (eprv-cxces) can be built from the same interface numbers.

   The body is one hollow shell that prints upright without supports. That is
   the reason the interior ceiling is a cone at roughly 50 degrees from the
   horizontal instead of a flat lid: a flat lid this wide would be a bridge far
   outside the overhang budget. The only bridges left are the flat ceiling of
   the weight pocket and the short span over the cable slot.

   The body is a single sampled egg curve, revolved, then squashed in y to get
   the oval cross-section. v1 stacked hull()s of oval discs instead, which is
   what produced the visible horizontal banding.
*/

/* part selects what is rendered. The body, neck and arms are all modelled in
   assembly position; the print_* variants are the same solids turned into their
   print orientations with the bed at z = 0, so the STLs can be regenerated from
   this file alone (see export.sh). */
part = "assembly";   // body | neck | arm_left | arm_right | assembly
                     // | print_body | print_neck | print_arm_left | print_arm_right

$fn = 180;

/* ----------------------------- body ----------------------------- */
body_width  = 40;      // widest point, across x
body_depth  = 30;      // widest point, across y
body_height = 48;
base_width  = 26;      // flat bottom, across x (depth comes out at 19.5)
wall        = 2.4;     // shell thickness along x; the y wall is about 1.8 after the squash

body_oval_scale = body_depth / body_width;   // y squash applied after the revolve
base_radius     = base_width / 2;
top_radius      = 12.4;                      // flat top, before the y squash

/* The egg profile is a quartic lower half and a power-curve shoulder that meet
   at the widest point with the same value, slope *and curvature*. Curvature is
   the one that matters: match only the slope and the surface's bend rate steps
   at the widest ring, which renders as a faint horizontal band even though the
   surface itself is smooth. Each half is smooth on its own, so with the join
   matched the whole profile is C2. */
body_peak_radius     = body_width / 2;
body_widest_z        = 28.0;   // 58% of the height, as in the sheet
profile_shoulder_m   = 0.7;    // how sharply the shoulder rolls into the top flat
profile_base_slope   = 0.709;  // dr/dz at the base: a 35 degree flare

/* Curvature at the widest point, taken from the shoulder curve; the lower
   quartic is then pinned by its value at the base, that curvature, and the base
   slope. */
profile_join_curvature = -2 * profile_shoulder_m * (body_peak_radius - top_radius)
                         / pow(body_height - body_widest_z, 2);
profile_lower_a = -profile_join_curvature * pow(body_widest_z, 2) / 2;
profile_lower_b = 4 * (body_peak_radius - base_radius) - 2 * profile_lower_a
                  - profile_base_slope * body_widest_z;
profile_lower_c = profile_base_slope * body_widest_z + profile_lower_a
                  - 3 * (body_peak_radius - base_radius);

/* Clamped to the body height so the fractional powers never see a negative
   base, which would quietly turn every caller undef. */
function profile_radius(z) =
  let (zc = min(body_height, max(0, z)))
  zc <= body_widest_z
    ? let (u = 1 - zc / body_widest_z)
      body_peak_radius - profile_lower_a * pow(u, 2)
                       - profile_lower_b * pow(u, 3)
                       - profile_lower_c * pow(u, 4)
    : let (t = (zc - body_widest_z) / (body_height - body_widest_z))
      top_radius + (body_peak_radius - top_radius) * pow(1 - pow(t, 2), profile_shoulder_m);

/* ---------------------- interior cavity ---------------------- */
floor_thickness    = 8.5;   // base slab above the weight pocket
cavity_ceiling_z   = 36.0;  // inner wall turns into the sloped ceiling here
ceiling_slope      = 1.05;  // z rise per unit radius: 44 degrees from vertical
bore_bottom_z      = cavity_ceiling_z;

/* ------------------ weight pocket (underside) ------------------ */
/* Largest circle that fits inside the flat base in its narrow direction (y),
   leaving a printable rim. The body widens above z=0, so this rim is the
   smallest one anywhere around the pocket. */
weight_pocket_rim      = 1.25;
weight_pocket_diameter = 2 * (base_radius * body_oval_scale - weight_pocket_rim);
weight_pocket_depth    = 6.0;

/* --------------------- back cable slot ---------------------- */
/* 14 x 9 with fully rounded ends: a 14 x 9 stadium, cut through the back wall
   above the base slab. */
slot_width  = 14.0;
slot_height = 9.0;
slot_z      = 14.0;
slot_radius = slot_height / 2;

/* -------------------- chest ring groove --------------------- */
/* Shallow annular groove on the front, centred left to right. The sheet puts the
   lit ring to the viewer's right, but the owner wanted it centred. Cut with one
   torus whose axis is the
   surface normal at the ring centre, which is also the plane that best fits the
   ring, so the depth stays within about half a millimetre of nominal. */
groove_x_center    = 0;
groove_z_center    = 36.5;
groove_ring_radius = 4.5;    // centre line of the ring
groove_width       = 1.7;
groove_depth       = 0.7;
/* A torus whose tube sat flush with the surface would barely mark this face: at
   the ring's 4.5 mm radius the body has already curved about 0.5 mm away from
   the tangent plane, so the tube has to start inside the surface for the ring
   to come out about 0.7 deep. Width sets the tube radius from that sink. */
groove_sink        = 0.2;    // tube centre, inside the surface along the normal
groove_tube_radius = sqrt(pow(groove_width / 2, 2) + pow(groove_sink, 2));

/* ------------------- shared interface: neck ------------------- */
/* Dimensions the neck (eprv-cxces) has to match. The chain is fixed by the
   USB-C plug: it has to travel up through the neck, so the neck bore is 14 and
   the spigot around that bore needs a wall of its own. That is why the socket
   is 17 rather than the 14.6 that a 14 spigot alone would need, which in turn
   sets the smallest top flat the body can have: 18.6 deep across y, and no
   smaller, or the flat would have no rim around the socket bore. */
neck_bore_diameter   = 14.0;      // clear path for the USB-C plug
neck_spigot_diameter = 16.4;      // 14 bore + 1.2 wall
neck_socket_diameter = neck_spigot_diameter + 0.6;   // 0.3 clearance per side
neck_socket_radius   = neck_socket_diameter / 2;
neck_socket_depth    = 10.0;      // nominal; the bore is open into the cavity
body_top_z           = body_height;
/* The neck's visible band is an extrusion of this ellipse: the body owns the
   outline, the neck must match it exactly. */
body_top_radius_x    = top_radius;
body_top_radius_y    = top_radius * body_oval_scale;
neck_collar_seat_z   = body_top_z;

/* Neck stackup. The neck is a straight band on the body's top flat with the
   spigot hanging under it. It prints upside down, so neither piece needs a
   flare to keep its overhangs in budget. */
neck_band_height = 5.0;   // visible black band, outline = the body top flat
neck_band_top_z  = body_top_z + neck_band_height;   // 53; the screen sits here

screen_width    = 46;
screen_height   = 46;
screen_depth    = 23;
/* The screen's USB-C port is not centred front to back: it sits about 40% of
   the depth back from the front face (measured by the owner). The bore cannot
   move forward to meet it (the spigot wall is only 1.2 mm and the band is 18.6
   mm deep), so the screen is offset back instead, putting the port straight
   over the centred bore. Front is -y. */
screen_port_from_front = 0.40;   // fraction of screen_depth
screen_y_offset = screen_depth * (0.5 - screen_port_from_front);   // +2.3, toward the back
screen_bottom_z = neck_band_top_z;
screen_top_z    = screen_bottom_z + screen_height;

/* ------------------ shared interface: arms ------------------ */
/* The joint: a stub on the body, a blind hole in the arm's flat back. The arm
   slides straight on along the back plane's normal, so the stub sits on the
   same axis as the hole. The body's wall stays solid where the stub is.

   The stub is a teardrop, not a cylinder: a circle with 45 degree flanks down
   to a point, so its underside prints at 45 degrees instead of overhanging.
   The hole takes the same shape with a little clearance, which also stops the
   arm from spinning on the stub. */
arm_stub_diameter  = 4.0;
arm_hole_clearance = 0.2;  // radial clearance between the stub and the hole
arm_hole_depth     = 3.0;  // blind, from the boss face into the shell
arm_stub_tip_gap   = 0.4;  // stub stops short of the hole's floor
arm_peg_z          = 26.0; // stub axis height
/* The shell alone is too thin for the blind hole, so the arm grows a boss on its
   inner face: a disc that reaches towards the body and stops 0.2 mm short of it,
   so it is invisible from outside and gives a glue face. The hole is drilled
   through the boss and into the shell. */
arm_boss_diameter  = 9.0;
arm_boss_face_x    = max([for (i = [0 : 20])
                            profile_radius(arm_peg_z - 5 + i * 0.5)]) + 0.2;

/* ----------------------------- neck -----------------------------
   Two solids: the spigot that plugs into the body's socket, and the band that
   sits on the body's top flat. The band's top is a bare flat annulus, so the
   screen rests on it located only by the USB-C plug in the bore.

   Modelled in assembly position; the print orientation is upside down (band
   top on the bed, spigot up) and is applied at export. Nothing overhangs in
   that orientation.
*/
neck_spigot_length = 9.0;    // into the socket, which is open into the cavity
neck_spigot_bottom_z = body_top_z - neck_spigot_length;   // 39

/* ----------------------------- arms -----------------------------
   A long, thick crescent, as on the sheet: it hugs the body at the shoulder and
   opens away toward a pointed tip, with its top tucked toward the back and its
   tip toward the front-bottom.

   The inner face is the body's own surface offset by a gap that grows from 0.4
   at the shoulder to 3 at the tip, and the outer face is that offset another
   4.5 out. So the arm is a shell of constant thickness that follows the body -
   no flat back, no epaulet.
*/
arm_tip_z         = 8.6;    // pointed tip, about 18% of the body height
arm_top_z         = 43.2;   // rounded top, about 90%
arm_wide_z        = 26.0;   // widest point of the leaf
arm_half_width    = 11.5;   // half width at the widest, from the arm's centreline
arm_centre_top    = 2.5;    // leaf centreline at the top: toward the back
arm_centre_tip    = -3.0;   // and at the tip: toward the front
arm_gap_shoulder  = 0.4;    // clearance to the body at the shoulder
arm_gap_tip       = 3.0;    // and at the tip, where the arm opens away
arm_thickness     = 3.0;    // thickest part of the petal, at the middle of the leaf
arm_bump_power    = 0.6;    // how fast it thins towards the tip and the top
arm_loft_x0       = 13.0;   // leaf outline is full size here ...
arm_loft_x1       = 28.0;   // ... and scaled down here, so the rim is a rolled edge
arm_loft_scale    = 0.75;
arm_leaf_steps    = 100;
arm_profile_steps = 72;

arm_groove_inset    = 1.2;   // groove centreline, inside the leaf edge
arm_groove_width    = 1.6;
arm_groove_depth    = 0.8;
arm_groove_below_z  = 30.0;  // the light line only runs along the lower edge

function arm_centre(z) =
  arm_centre_tip + (arm_centre_top - arm_centre_tip)
                   * (z - arm_tip_z) / (arm_top_z - arm_tip_z);

function arm_gap(z) =
  arm_gap_shoulder + (arm_gap_tip - arm_gap_shoulder)
                     * (arm_top_z - z) / (arm_top_z - arm_tip_z);

function arm_inner_radius(z) = profile_radius(z) + arm_gap(z);
/* The petal is thickest at the middle of the leaf and thins to nothing at its
   top and tip, so the front view is a crescent rather than a slab. */
function arm_bump(z) =
  pow(sin(180 * (z - arm_tip_z) / (arm_top_z - arm_tip_z)), arm_bump_power);
function arm_outer_radius(z) = arm_inner_radius(z) + arm_thickness * arm_bump(z);

/* The leaf: a point at the tip, widest near the middle, rounded at the top. */
function arm_leaf_half_width(z) =
  let (u = (z - arm_wide_z) / (arm_top_z - arm_wide_z))
  /* Floored so the polygon does not close on itself at the tip and the top,
     which would leave a zero-width edge and degenerate facets. */
  max(0.35,
      z <= arm_wide_z
        ? arm_half_width * pow(sin(90 * (z - arm_tip_z) / (arm_wide_z - arm_tip_z)), 0.62)
        : arm_half_width * sqrt(max(0, 1 - u * u)));

arm_stub_base_x = profile_radius(arm_peg_z);
arm_stub_length = arm_boss_face_x - arm_stub_base_x + arm_hole_depth - arm_stub_tip_gap;

/* --------------------- sampled geometry --------------------- */

/* Cosine density in z: ring spacing tightens at the base flare and at the
   shoulder where the surface turns over, and relaxes across the flank where the
   radius barely changes. */
profile_samples = 640;

body_profile = [
  for (i = [0 : profile_samples])
    let (z = body_height * (1 - cos(180 * i / profile_samples)) / 2)
      [profile_radius(z), z]
];



cavity_wall_r    = profile_radius(cavity_ceiling_z) - wall;
/* The ceiling cone runs a little past the bore radius instead of stopping
   exactly on it: apex on the bore wall leaves CGAL with two zero-area faces at
   the tangent point, and the overshoot is inside the bore cut anyway. */
cone_apex_r      = neck_socket_radius - 0.5;
cone_top_z       = cavity_ceiling_z + (cavity_wall_r - cone_apex_r) * ceiling_slope;

body_outline = concat([[0, 0]], body_profile, [[0, body_height]]);

cavity_wall = [
  for (p = body_profile)
    if (p[1] >= floor_thickness && p[1] <= cavity_ceiling_z) [p[0] - wall, p[1]]
];

cavity_outline = concat(
  [[0, floor_thickness]],
  cavity_wall,
  [[cone_apex_r, cone_top_z], [0, cone_top_z]]);

/* ------------------------- body modules ------------------------- */

module revolve_outline(points) {
  scale([1, body_oval_scale, 1])
    rotate_extrude(angle = 360, convexity = 10)
      polygon(points);
}

module body_outer() {
  revolve_outline(body_outline);
}

module body_cavity() {
  revolve_outline(cavity_outline);
}

/* One torus, not a chain of cutters: a chain of spheres left a visibly faceted
   groove. The torus' axis is the surface normal at the ring centre, which is
   also the best-fit plane normal there, so the groove stays 0.7 deep all the
   way round. */
function front_surface_y(x, z) =
  let (r = profile_radius(z))
  -body_oval_scale * sqrt(max(0.0001, r * r - x * x));

function front_surface_normal(x, z) =
  let (r = profile_radius(z),
       dr = (profile_radius(z + 0.2) - profile_radius(z - 0.2)) / 0.4,
       y = front_surface_y(x, z),
       v = [x, y / (body_oval_scale * body_oval_scale), -r * dr])
  v / norm(v);

module chest_groove_cut() {
  let (surface = [groove_x_center, front_surface_y(groove_x_center, groove_z_center),
                  groove_z_center],
       normal = front_surface_normal(groove_x_center, groove_z_center))
    translate(surface - normal * groove_sink)
      rotate([0, acos(normal[2]), atan2(normal[1], normal[0])])
        rotate_extrude(angle = 360, convexity = 10, $fn = 180)
          translate([groove_ring_radius, 0])
            circle(r = groove_tube_radius, $fn = 32);
}

module cable_slot_cut() {
  hull()
    for (x = [-(slot_width / 2 - slot_radius), slot_width / 2 - slot_radius])
      translate([x, 5, slot_z])
        rotate([-90, 0, 0])
          cylinder(r = slot_radius, h = 18);
}

/* Stub on the body's side, on the arm's back-plane normal. Built per side with
   a rotation rather than a mirror: a mirrored solid has inverted faces and CGAL
   silently ignores it in a boolean. */
/* Cross-section in a frame where +x is "down": the circle's upper half, then
   the two 45 degree flanks meeting at a point at (r, 0). */
function teardrop_2d(dia, steps = 36) =
  concat(
    [for (i = [0 : steps])
       let (a = 90 + 180 * i / steps)
         [cos(a) * dia / 2, sin(a) * dia / 2]],
    [[dia / 2, 0]]);

module arm_stub_right() {
  translate([arm_stub_base_x - arm_stub_dir[0], 0, arm_peg_z - arm_stub_dir[2]])
    rotate([0, 90 - atan(arm_back_tilt), 0])
      linear_extrude(height = arm_stub_length + 1)
        polygon(teardrop_2d(arm_stub_diameter));
}

/* Half a turn about z, so the left stub is the right one rotated, not mirrored,
   and its teardrop still points down. */
module arm_stub(side) {
  if (side > 0) arm_stub_right();
  else rotate([0, 0, 180]) arm_stub_right();
}

module body() {
  difference() {
    union() {
      body_outer();
      for (side = [-1, 1]) arm_stub(side);
    }

    body_cavity();

    // Neck spigot bore, open into the cavity.
    translate([0, 0, bore_bottom_z - 0.01])
      cylinder(r = neck_socket_radius, h = body_height - bore_bottom_z + 2);

    // Weight pocket in the underside, ceiling is a small bridge by design.
    translate([0, 0, -0.01])
      cylinder(d = weight_pocket_diameter, h = weight_pocket_depth + 0.01);

    cable_slot_cut();
    chest_groove_cut();
  }
}

/* ---------------------- assembly preview ---------------------- */

module dummy_screen() {
  // Never exported: only here so the body/head proportions can be judged.
  color([0.07, 0.08, 0.10])
    translate([-screen_width / 2, screen_y_offset - screen_depth / 2, screen_bottom_z])
      hull()
        for (x = [2, screen_width - 2])
          for (y = [2, screen_depth - 2])
            for (z = [2, screen_height - 2])
              translate([x, y, z]) sphere(r = 2, $fn = 16);
}

module assembly() {
  color([0.93, 0.93, 0.91]) {
    body();
    arm_right();
    arm_left();
  }
  color([0.10, 0.10, 0.11]) neck();
  dummy_screen();
}

/* ---------------------------- neck ---------------------------- */

/* The band is an extrusion of the body's top flat, so the body's silhouette
   carries straight on into the black neck without a lip. */
module neck_band() {
  translate([0, 0, body_top_z])
    linear_extrude(height = neck_band_height)
      scale([1, body_oval_scale]) circle(r = body_top_radius_x, $fn = $fn);
}

module neck_spigot() {
  translate([0, 0, neck_spigot_bottom_z])
    cylinder(d = neck_spigot_diameter,
             h = body_top_z - neck_spigot_bottom_z + 0.01);
}

module neck() {
  difference() {
    union() {
      neck_band();
      neck_spigot();
    }
    /* Cable bore: through the spigot and the band, open at the band's top so
       the plug comes out flush under the screen and the screen's port can sit
       over it. Runs a millimetre past each end so neither cut ends on a face
       of the solid, which would leave coplanar faces in the boolean. */
    translate([0, 0, neck_spigot_bottom_z - 1])
      cylinder(d = neck_bore_diameter,
               h = neck_band_top_z - neck_spigot_bottom_z + 2);
  }
}

/* ---------------------------- arms ---------------------------- */

arm_leaf = concat(
  [for (i = [0 : arm_leaf_steps])
     let (z = arm_tip_z + (arm_top_z - arm_tip_z) * i / arm_leaf_steps)
       [arm_centre(z) + arm_leaf_half_width(z), z]],
  [for (i = [arm_leaf_steps : -1 : 0])
     let (z = arm_tip_z + (arm_top_z - arm_tip_z) * i / arm_leaf_steps)
       [arm_centre(z) - arm_leaf_half_width(z), z]]);

/* Leaf outline in the y-z plane, lofted outward so it shrinks as x grows: the
   cut face becomes a rolled edge instead of a slab. The scale is about the
   leaf's own centroid so the outline stays where it is. */
arm_leaf_centre_z = (arm_tip_z + arm_top_z) / 2 + 0.17;   // off a sample, so the
                                                          // loft has no degenerate seam
arm_leaf_centre_y = arm_centre(arm_leaf_centre_z);

module arm_leaf_prism() {
  translate([arm_loft_x0, 0, 0])
    rotate([0, 90, 0])
      translate([-arm_leaf_centre_z, arm_leaf_centre_y, 0])
        linear_extrude(height = arm_loft_x1 - arm_loft_x0, scale = arm_loft_scale)
          translate([arm_leaf_centre_z, -arm_leaf_centre_y, 0])
            polygon([for (p = arm_leaf) [-p[1], p[0]]]);
}

/* Inner and outer faces: the body's own profile offset by the gap (and by the
   gap plus the shell thickness), revolved with the body's oval squash. */
function arm_profile(use_outer) =
  [for (i = [0 : arm_profile_steps])
     let (z = arm_tip_z + (arm_top_z - arm_tip_z) * i / arm_profile_steps)
       [use_outer ? arm_outer_radius(z) : arm_inner_radius(z), z]];

module arm_inner_solid() {
  revolve_outline(concat([[0, arm_tip_z]], arm_profile(false),
                         [[0, arm_top_z]]));
}

module arm_outer_solid() {
  revolve_outline(concat([[0, arm_tip_z]], arm_profile(true),
                         [[0, arm_top_z]]));
}

/* Inner and outer surfaces, for placing the groove cutters, their normals, and
   the print-orientation bed shift. */
function arm_inner_x(y, z) =
  let (r = arm_inner_radius(z))
  sqrt(max(0.0001, r * r - pow(y / body_oval_scale, 2)));

function arm_outer_x(y, z) =
  let (r = arm_outer_radius(z))
  sqrt(max(0.0001, r * r - pow(y / body_oval_scale, 2)));

function arm_outer_normal(y, z) =
  let (r = arm_outer_radius(z),
       dr = (arm_outer_radius(z + 0.2) - arm_outer_radius(z - 0.2)) / 0.4,
       x = arm_outer_x(y, z),
       v = [x, y / (body_oval_scale * body_oval_scale), -r * dr])
  v / norm(v);

function arm_groove_point(i) =
  let (p = arm_leaf[i], a = arm_leaf[i - 1], b = arm_leaf[i + 1],
       t = [b[0] - a[0], b[1] - a[1]],
       m = norm(t),
       n0 = [-t[1] / m, t[0] / m],
       centre = [arm_leaf_centre_y, arm_leaf_centre_z],
       inward = n0[0] * (centre[0] - p[0]) + n0[1] * (centre[1] - p[1]) > 0
                ? n0 : [-n0[0], -n0[1]])
  [p[0] + arm_groove_inset * inward[0], p[1] + arm_groove_inset * inward[1]];

/* Light-line groove: short cutters square to the outer face, run in by `depth`.
   The depth is capped at a fraction of the shell so it cannot punch through. */
module arm_groove_cut() {
  for (i = [1 : len(arm_leaf) - 2])
    if (arm_leaf[i][1] < arm_groove_below_z)
      let (p = arm_groove_point(i),
           n = arm_outer_normal(p[0], p[1]),
           depth = min(arm_groove_depth, 0.55 * arm_thickness * arm_bump(p[1])))
        translate([arm_outer_x(p[0], p[1]), p[0], p[1]] + n * (arm_groove_width / 2))
          rotate([0, acos(-n[2]), atan2(-n[1], -n[0])])
            cylinder(r = arm_groove_width / 2,
                     h = arm_groove_width / 2 + depth, $fn = 32);
}

/* Teardrop cross-section: a circle with 45 degree flanks down to a point, so
   the stub's underside prints at 45 degrees instead of overhanging. */
function teardrop_2d(dia, steps = 36) =
  concat(
    [for (i = [0 : steps])
       let (a = 90 + 180 * i / steps)
         [cos(a) * dia / 2, sin(a) * dia / 2]],
    [[dia / 2, 0]]);

module arm_stub_right() {
  translate([arm_stub_base_x - 1, 0, arm_peg_z])
    rotate([0, 90, 0])
      linear_extrude(height = arm_stub_length + 1)
        polygon(teardrop_2d(arm_stub_diameter));
}

/* Half a turn about z, so the left stub is the right one rotated, not mirrored,
   and its teardrop still points down. */
module arm_stub(side) {
  if (side > 0) arm_stub_right();
  else rotate([0, 0, 180]) arm_stub_right();
}

/* Local thickening on the arm's inner face, around the blind hole. It runs from
   the boss face (0.2 mm clear of the body) out to the arm's own inner surface,
   so it is invisible once the arm is on. */
module arm_boss() {
  intersection() {
    translate([arm_boss_face_x, 0, arm_peg_z])
      rotate([0, 90, 0])
        linear_extrude(height = 20)
          circle(d = arm_boss_diameter, $fn = 64);
    /* Runs a little past the arm's inner face so the union overlaps instead of
       meeting on a coincident surface. */
    revolve_outline(concat(
      [[0, arm_tip_z]],
      [for (i = [0 : arm_profile_steps])
         let (z = arm_tip_z + (arm_top_z - arm_tip_z) * i / arm_profile_steps)
           [arm_inner_radius(z) + 0.6, z]],
      [[0, arm_top_z]]));
  }
}

/* Blind hole for the body's stub: same teardrop, grown by the clearance, drilled
   from the boss face through the boss and into the shell. */
module arm_stub_hole() {
  translate([arm_boss_face_x - 0.5, 0, arm_peg_z])
    rotate([0, 90, 0])
      linear_extrude(height = arm_hole_depth + 0.51)
        offset(delta = arm_hole_clearance)
          polygon(teardrop_2d(arm_stub_diameter));
}

module arm_right() {
  difference() {
    union() {
      intersection() {
        difference() {
          arm_outer_solid();
          arm_inner_solid();
        }
        arm_leaf_prism();
      }
      arm_boss();
    }
    arm_stub_hole();
    arm_groove_cut();
  }
}

/* The left arm is the right arm rotated half a turn about z. That is a proper
   rotation, so no reflected (inside-out) solid ever reaches a boolean - a
   mirrored cutter silently fails in CGAL. */
module arm_left() {
  rotate([0, 0, 180]) arm_right();
}

/* ------------------------ print orientations ------------------------
   Body: upright, exactly as modelled.
   Neck: upside down, band top on the bed. The flip about x then drops the
   band's top face (z = neck_band_top_z) onto z = 0, leaving the spigot standing
   up. No overhangs and no bridges that way.
   Arms: on the inner face, tipped a little past flat, with the shift below
   putting the lowest point of that face on z = 0. The inner face is curved, so
   this orientation needs supports under it; it was chosen as the least support
   of the orientations tried.
*/
/* The arm is printed on its inner face. Of the rotations tried, tipping it 15
   degrees past flat gave the least support area (about 410 mm2 against 470 flat
   on its back), and it rests on an edge instead of a point. The shift is taken
   from the lowest point of the inner face, which lies on the leaf's outline. */
print_arm_tilt = -105;

print_arm_shift = -4.39982;   // measured: the lowest point of the inner face in this
                            // orientation, from the exported mesh

module print_body()  { body(); }
module print_neck()  { translate([0, 0, neck_band_top_z]) rotate([180, 0, 0]) neck(); }

module print_arm_right() {
  translate([0, 0, print_arm_shift])
    rotate([0, print_arm_tilt, 0])
      arm_right();
}

/* The left arm is the right one turned half a turn about z, so its inner face
   points the other way and it takes the opposite tilt. The bed shift is the
   same by symmetry. */
module print_arm_left() {
  translate([0, 0, print_arm_shift])
    rotate([0, -print_arm_tilt, 0])
      arm_left();
}

if (part == "body") body();
else if (part == "neck") neck();
else if (part == "arm_right") arm_right();
else if (part == "arm_left") arm_left();
else if (part == "assembly") assembly();
else if (part == "print_body") print_body();
else if (part == "print_neck") print_neck();
else if (part == "print_arm_left") print_arm_left();
else if (part == "print_arm_right") print_arm_right();
