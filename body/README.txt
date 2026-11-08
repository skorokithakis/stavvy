EVE-style printable desk robot - parts and notes.

FILES
  eve_robot.scad    the whole model. part = "assembly" (default) builds the
                    assembled view; part = "print_body", "print_neck",
                    "print_arm_left", "print_arm_right" gives each printable
                    part in its print orientation, sitting on z = 0.
  export.sh         regenerates stl/*.stl and the three preview PNGs.
  stl/body.stl      body shell
  stl/neck.stl      neck: the spigot and the black band under the screen
  stl/arm_left.stl
  stl/arm_right.stl
  preview_front.png, preview_side.png, preview_back.png   (assembly, with a
                    dummy screen; the dummy is never exported)

PRINTING
  body      white, upright as exported, no supports.
  neck      black, upside down as exported (band top face on the bed, spigot
            standing up and 10 degrees off vertical), no supports.
  arms      white, inner face down as exported. The arm is a curved shell, so it
            needs supports under the inner face - tree supports, around 400 mm2
            each. The other parts print without supports.
  Suggested: 0.4 mm nozzle, 0.2 mm layers, 3 perimeters, 15-20% infill.

ASSEMBLY
  1. Print the neck first and test-fit the screen on the band's top face before
     printing the rest.
  2. Feed the USB-C plug in through the back slot, up through the body and the
     neck's 14 mm bore, and plug it into the screen. The screen then rests on
     the band's top face, located only by the plug.
  3. Glue the neck's spigot into the body's top socket. The neck's band then
     sits on the body's top flat.
  4. Slide each arm onto the 4 mm stub on the side of the body (a blind hole in
     the arm's inner face takes it) and glue it. The stub is a teardrop rather
     than a round pin, so the arm can only go on one way.

TILT
  The neck's band top is not level: it slopes up towards the front by 10
  degrees (neck_tilt in the .scad), pivoting about the line through the bore
  at y = 0, z = 53. The band is about 6.6 mm tall at the front and 3.4 mm at
  the back, and the screen sits on that slope, so it faces slightly up.
  Because the pivot runs over the bore, the band is still 5 mm deep over the
  bore and the screen's port still lands over it.

WEIGHT
  The body has a 17 mm pocket in its underside: a stack of small coins or a
  17 mm steel disc fits.

COLOURS
  Body and arms print white; the neck prints black.

UNTESTED ASSUMPTIONS
  - The USB-C plug's overmould passes the 14 mm bore in the neck.
  - The screen is 46 x 46 x 23 mm with a flat bottom. Its port is centred
    left to right and about 40% of the depth back from the front face. The
    screen sits 2.3 mm toward the back so the port is over the cable bore
    (screen_port_from_front in the .scad).
  - Nothing holds the screen except the plug: it can wobble on the band's
    sloped top and slide off if the robot is tipped.
