#include "faces.h"

#include <assert.h>
#include <math.h>

// The faces are drawn from the shapes in the user's design notes, which give
// the geometry in the SVG coordinates of the 480x480 frame. LVGL 9.5 has no
// ellipse or Bezier primitive, so ellipses become pills and each quadratic
// curve becomes the circular arc through the curve's midpoint.
//
// Colours are already mixed against the black background, because the screen
// is black and a group opacity would brighten wherever two translucent strokes
// cross. Only the listening waves keep a per-shape opacity, because they fade
// in and out.
#define FACE_COLOUR_EYE 0xDCDCFFu
#define FACE_COLOUR_ACCENT 0x5252E6u
#define FACE_COLOUR_ALERT 0xF72A61u
// #DCDCFF at 55% over black and #F72A61 at 45% over black.
#define FACE_COLOUR_MUTED 0x79798Cu
#define FACE_COLOUR_CHEEK 0x6F132Cu
#define FACE_COLOUR_WHITE 0xEFEFFFu
#define FACE_COLOUR_HIGHLIGHT 0xFFFFFFu
#define FACE_COLOUR_BLACK 0x000000u
#define FACE_COLOUR_BUBBLE 0x2A2540u
#define FACE_COLOUR_SCROLL_TRACK 0x3A3456u
#define FACE_COLOUR_SCROLL_THUMB 0x9A98C0u
#define FACE_COLOUR_THINKING 0x8A38F5u
#define FACE_COLOUR_USER_BORDER 0x3A3456u

#define FACE_OPAQUE 255

// The design's blink keyframe. The design uses 0.08 for the idle face and 0.1
// for the muted face; one value for both keeps a blink looking the same.
#define FACE_BLINK_SCALE 0.08f

#define FACE_PI 3.14159265358979323846f

typedef struct {
    uint16_t time_ms;
    float value;
} face_keyframe_t;

// Every animation repeats with its first and last keyframe equal, so the jump
// back to zero is invisible and the period wraps cleanly.
static const face_keyframe_t breathe_keyframes[] = {
    {0, 0.0f},
    {2500, -4.0f},
    {5000, 0.0f},
};

static const face_keyframe_t tilt_keyframes[] = {
    {0, -4.0f},
    {1500, -7.0f},
    {3000, -4.0f},
};

static const face_keyframe_t wave_keyframes[] = {
    {0, 0.0f},
    {480, 0.9f},
    {1600, 0.0f},
};

static const face_keyframe_t bob_keyframes[] = {
    {0, 0.0f},
    {900, -5.0f},
    {1800, 0.0f},
};

static const face_keyframe_t talk_keyframes[] = {
    {0, 0.3f},
    {450, 1.0f},
    {900, 0.3f},
};

static const face_keyframe_t look_keyframes[] = {
    {0, 0.0f},
    {2400, 0.0f},
    {3000, -10.0f},
    {3600, -10.0f},
    {4200, 10.0f},
    {4800, 10.0f},
    {6000, 0.0f},
};

// The design closes the eyes over 90 ms and opens them over 160 ms. The
// easing is the same per-segment smoothstep as the other animations, so the
// lid accelerates out of and into the open and closed positions.
static const face_keyframe_t blink_keyframes[] = {
    {0, 1.0f},
    {90, FACE_BLINK_SCALE},
    {FACE_BLINK_DURATION_MILLISECONDS, 1.0f},
};

// The thinking dots bounce up and dim at the ends of each 1.2 s cycle. The
// last keyframe is flat, so a dot waits for its turn while the others bounce.
static const face_keyframe_t thinking_dot_y_keyframes[] = {
    {0, 0.0f},
    {360, -6.0f},
    {720, 0.0f},
    {1200, 0.0f},
};

static const face_keyframe_t thinking_dot_opacity_keyframes[] = {
    {0, 0.5f},
    {360, 1.0f},
    {720, 0.5f},
    {1200, 0.5f},
};

// The mini thinking eyes look up and to the right, then drift left and down.
static const face_keyframe_t thinking_gaze_x_keyframes[] = {
    {0, 6.0f},
    {2000, -3.0f},
    {4000, 6.0f},
};

static const face_keyframe_t thinking_gaze_y_keyframes[] = {
    {0, -8.0f},
    {2000, -10.0f},
    {4000, -8.0f},
};

// Smoothstep for every segment is not exactly the design's ease-in-out and
// ease-out, but the difference is below a pixel at these amplitudes, and it
// avoids a second easing function.
static float evaluate_keyframes(const face_keyframe_t *frames, int count, float time_ms)
{
    if (time_ms <= (float)frames[0].time_ms) {
        return frames[0].value;
    }
    for (int index = 0; index < count - 1; index++) {
        const float start = (float)frames[index].time_ms;
        const float end = (float)frames[index + 1].time_ms;
        if (time_ms <= end) {
            float progress = (time_ms - start) / (end - start);
            progress = progress * progress * (3.0f - 2.0f * progress);
            return frames[index].value + (frames[index + 1].value - frames[index].value) * progress;
        }
    }
    return frames[count - 1].value;
}

// Before an animation's delay ends it shows its first keyframe, which is how
// the CSS delay behaves. The elapsed time comes from the face's first frame,
// so every animation restarts when the face does.
static float animation_value(const face_keyframe_t *frames, int count, uint32_t elapsed_ms, uint32_t delay_ms,
                             uint32_t period_ms)
{
    if (elapsed_ms < delay_ms) {
        return frames[0].value;
    }
    const float time_ms = (float)((elapsed_ms - delay_ms) % period_ms);
    return evaluate_keyframes(frames, count, time_ms);
}

static uint8_t opacity_from_fraction(float fraction)
{
    return (uint8_t)lroundf(fraction * 255.0f);
}

static int add_filled_rectangle(face_shape_t *shapes, int count, int16_t x, int16_t y, int16_t width,
                                int16_t height, int16_t radius, uint32_t rgb, uint8_t opacity)
{
    assert(count < FACE_MAX_SHAPES);
    shapes[count] = (face_shape_t){
        .x1 = x,
        .y1 = y,
        .x2 = (int16_t)(x + width - 1),
        .y2 = (int16_t)(y + height - 1),
        .radius = radius,
        .kind = FACE_SHAPE_FILLED_RECT,
        .red = (uint8_t)(rgb >> 16),
        .green = (uint8_t)(rgb >> 8),
        .blue = (uint8_t)rgb,
        .opacity = opacity,
    };
    return count + 1;
}

// An outline whose rectangle is already the outer edge, with selectable
// sides. The user bubble uses it directly and its tail uses a bottom-right
// only piece.
static int add_outline_outer(face_shape_t *shapes, int count, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                             int16_t radius, int16_t stroke_width, uint8_t border_sides, uint32_t rgb,
                             uint8_t opacity)
{
    assert(count < FACE_MAX_SHAPES);
    shapes[count] = (face_shape_t){
        .x1 = x1,
        .y1 = y1,
        .x2 = x2,
        .y2 = y2,
        .radius = radius,
        .kind = FACE_SHAPE_OUTLINE_RECT,
        .red = (uint8_t)(rgb >> 16),
        .green = (uint8_t)(rgb >> 8),
        .blue = (uint8_t)rgb,
        .opacity = opacity,
        .stroke_width = (uint8_t)stroke_width,
        .border_sides = border_sides,
    };
    return count + 1;
}

// The design's outlines are SVG strokes centred on the path, while LVGL draws
// a border inside its rectangle. Callers pass the path's bounding box and the
// stroke width, and this expands it by half the stroke so the visible outer
// edge lands where the SVG's does.
static int add_outline_rectangle(face_shape_t *shapes, int count, int16_t x, int16_t y, int16_t width,
                                 int16_t height, int16_t radius, int16_t stroke_width, uint32_t rgb,
                                 uint8_t opacity)
{
    const int16_t half_stroke = stroke_width / 2;
    return add_outline_outer(shapes, count, (int16_t)(x - half_stroke), (int16_t)(y - half_stroke),
                             (int16_t)(x + width - 1 + half_stroke),
                             (int16_t)(y + height - 1 + half_stroke), (int16_t)(radius + half_stroke),
                             stroke_width, FACE_BORDER_FULL, rgb, opacity);
}

// A pill is a rounded rectangle whose radius is half its shorter side. An SVG
// ellipse of the same bounding box becomes one of these.
static int add_pill(face_shape_t *shapes, int count, int16_t x, int16_t y, int16_t width, int16_t height,
                    uint32_t rgb, uint8_t opacity)
{
    const int16_t radius = (width < height ? width : height) / 2;
    return add_filled_rectangle(shapes, count, x, y, width, height, radius, rgb, opacity);
}

static int add_line(face_shape_t *shapes, int count, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                    int16_t stroke_width, uint32_t rgb, uint8_t opacity)
{
    assert(count < FACE_MAX_SHAPES);
    shapes[count] = (face_shape_t){
        .x1 = x1,
        .y1 = y1,
        .x2 = x2,
        .y2 = y2,
        .kind = FACE_SHAPE_LINE,
        .red = (uint8_t)(rgb >> 16),
        .green = (uint8_t)(rgb >> 8),
        .blue = (uint8_t)rgb,
        .opacity = opacity,
        .stroke_width = (uint8_t)stroke_width,
    };
    return count + 1;
}

// radius is the centreline here; display.c adds half the stroke when it turns
// this into an lv_draw_arc, whose radius is the outer edge.
static int add_arc(face_shape_t *shapes, int count, int16_t centre_x, int16_t centre_y, int16_t radius,
                   int16_t start_angle, int16_t end_angle, int16_t stroke_width, uint32_t rgb, uint8_t opacity)
{
    assert(count < FACE_MAX_SHAPES);
    shapes[count] = (face_shape_t){
        .x1 = centre_x,
        .y1 = centre_y,
        .radius = radius,
        .start_angle = start_angle,
        .end_angle = end_angle,
        .kind = FACE_SHAPE_ARC,
        .red = (uint8_t)(rgb >> 16),
        .green = (uint8_t)(rgb >> 8),
        .blue = (uint8_t)rgb,
        .opacity = opacity,
        .stroke_width = (uint8_t)stroke_width,
    };
    return count + 1;
}

static bool circle_through_points(float x0, float y0, float x1, float y1, float x2, float y2, float *centre_x,
                                  float *centre_y, float *radius)
{
    const float determinant = 2.0f * (x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1));
    if (fabsf(determinant) < 0.5f) {
        return false;
    }
    const float square0 = x0 * x0 + y0 * y0;
    const float square1 = x1 * x1 + y1 * y1;
    const float square2 = x2 * x2 + y2 * y2;
    *centre_x = (square0 * (y1 - y2) + square1 * (y2 - y0) + square2 * (y0 - y1)) / determinant;
    *centre_y = (square0 * (x2 - x1) + square1 * (x0 - x2) + square2 * (x1 - x0)) / determinant;
    *radius = hypotf(x0 - *centre_x, y0 - *centre_y);
    return true;
}

static float normalise_degrees(float degrees)
{
    while (degrees < 0.0f) {
        degrees += 360.0f;
    }
    while (degrees >= 360.0f) {
        degrees -= 360.0f;
    }
    return degrees;
}

// The SVG path "M x0 y0 Q control_x control_y x1 y1" becomes the circular arc
// through (x0,y0), the curve's midpoint and (x1,y1). LVGL's arcs run clockwise
// from start_angle to end_angle, so the angles are chosen so that the midpoint
// lies on that run.
static int add_quadratic_arc(face_shape_t *shapes, int count, int16_t x0, int16_t y0, int16_t control_x,
                             int16_t control_y, int16_t x1, int16_t y1, int16_t stroke_width, uint32_t rgb,
                             uint8_t opacity)
{
    const float mid_x = (x0 + 2.0f * control_x + x1) / 4.0f;
    const float mid_y = (y0 + 2.0f * control_y + y1) / 4.0f;

    float centre_x;
    float centre_y;
    float radius;
    if (!circle_through_points(x0, y0, mid_x, mid_y, x1, y1, &centre_x, &centre_y, &radius)) {
        return add_line(shapes, count, x0, y0, x1, y1, stroke_width, rgb, opacity);
    }

    const float angle0 = atan2f(y0 - centre_y, x0 - centre_x) * 180.0f / FACE_PI;
    const float angle_mid = atan2f(mid_y - centre_y, mid_x - centre_x) * 180.0f / FACE_PI;
    const float angle1 = atan2f(y1 - centre_y, x1 - centre_x) * 180.0f / FACE_PI;

    const float to_mid = normalise_degrees(angle_mid - angle0);
    const float to_end = normalise_degrees(angle1 - angle0);
    float start_angle;
    float end_angle;
    if (to_mid < to_end) {
        start_angle = angle0;
        end_angle = angle0 + to_end;
    } else {
        start_angle = angle1;
        end_angle = angle1 + normalise_degrees(angle0 - angle1);
    }
    // LVGL's angle mask clamps negative angles to zero instead of wrapping
    // them, so both ends must be in 0..359 even when the arc crosses 3
    // o'clock (the happy eyes do).
    start_angle = normalise_degrees(start_angle);
    end_angle = normalise_degrees(end_angle);

    return add_arc(shapes, count, (int16_t)lroundf(centre_x), (int16_t)lroundf(centre_y),
                   (int16_t)lroundf(radius), (int16_t)lroundf(start_angle), (int16_t)lroundf(end_angle),
                   stroke_width, rgb, opacity);
}

static void rotate_point(float *x, float *y, float centre_x, float centre_y, float degrees)
{
    const float radians = degrees * FACE_PI / 180.0f;
    const float cosine = cosf(radians);
    const float sine = sinf(radians);
    const float offset_x = *x - centre_x;
    const float offset_y = *y - centre_y;
    *x = centre_x + offset_x * cosine - offset_y * sine;
    *y = centre_y + offset_x * sine + offset_y * cosine;
}

// The design rotates the listening eyes and mouth as a group. Rotating an
// LVGL rectangle would need an off-screen layer, so this moves each shape's
// centre the way the rotation would and leaves the shape upright.
static int add_rotated_pill(face_shape_t *shapes, int count, float centre_x, float centre_y, int16_t width,
                            int16_t height, float rotation_centre_x, float rotation_centre_y, float degrees,
                            uint32_t rgb)
{
    rotate_point(&centre_x, &centre_y, rotation_centre_x, rotation_centre_y, degrees);
    const int16_t x = (int16_t)lroundf(centre_x - width / 2.0f);
    const int16_t y = (int16_t)lroundf(centre_y - height / 2.0f);
    return add_pill(shapes, count, x, y, width, height, rgb, FACE_OPAQUE);
}

static void translate_shapes_vertically(face_shape_t *shapes, int count, float offset)
{
    for (int index = 0; index < count; index++) {
        shapes[index].y1 = (int16_t)lroundf(shapes[index].y1 + offset);
        // An arc stores its centre in y1 and leaves y2 unused.
        if (shapes[index].kind != FACE_SHAPE_ARC) {
            shapes[index].y2 = (int16_t)lroundf(shapes[index].y2 + offset);
        }
    }
}

// Blink squashes the eye about its own centre. The rounded height never drops
// below one pixel, so a one-pixel row still repaints as a closed eye. A scale
// of 1 leaves the eye exactly where it was, so a face that is not blinking
// produces the same shapes every frame.
static int add_eye(face_shape_t *shapes, int count, int16_t x, int16_t y, int16_t width, int16_t height,
                   uint32_t rgb, float blink_scale)
{
    if (blink_scale >= 1.0f) {
        return add_pill(shapes, count, x, y, width, height, rgb, FACE_OPAQUE);
    }

    const int16_t squashed_height = (int16_t)lroundf(height * blink_scale);
    const int16_t centre_y = (int16_t)lroundf(y + height / 2.0f);
    const int16_t squashed_y = (int16_t)lroundf(centre_y - (squashed_height < 1 ? 1 : squashed_height) / 2.0f);
    return add_pill(shapes, count, x, squashed_y, width, squashed_height < 1 ? 1 : squashed_height, rgb,
                    FACE_OPAQUE);
}

static int build_idle(face_shape_t *shapes, uint32_t elapsed_ms, float blink)
{
    int count = 0;
    const float breathe = animation_value(breathe_keyframes, 3, elapsed_ms, 0, 5000);

    count = add_eye(shapes, count, 140, 150, 70, 130, FACE_COLOUR_EYE, blink);
    count = add_eye(shapes, count, 270, 150, 70, 130, FACE_COLOUR_EYE, blink);
    count = add_quadratic_arc(shapes, count, 215, 320, 240, 342, 265, 320, 10, FACE_COLOUR_EYE, FACE_OPAQUE);

    translate_shapes_vertically(shapes, count, breathe);
    return count;
}

static int build_listening(face_shape_t *shapes, uint32_t elapsed_ms)
{
    int count = 0;

    // Each level of the sound wave is the same shape on both sides, so the
    // right-hand arcs are the left-hand ones mirrored about x = 240.
    static const int16_t wave_paths[3][6] = {
        {95, 200, 78, 230, 95, 260},
        {70, 180, 45, 230, 70, 280},
        {45, 160, 12, 230, 45, 300},
    };
    static const uint32_t wave_delays[3] = {0, 400, 800};

    for (int level = 0; level < 3; level++) {
        const uint8_t opacity =
            opacity_from_fraction(animation_value(wave_keyframes, 3, elapsed_ms, wave_delays[level], 1600));
        const int16_t *path = wave_paths[level];
        count = add_quadratic_arc(shapes, count, path[0], path[1], path[2], path[3], path[4], path[5], 8,
                                  FACE_COLOUR_ACCENT, opacity);
        count = add_quadratic_arc(shapes, count, (int16_t)(480 - path[0]), path[1], (int16_t)(480 - path[2]),
                                  path[3], (int16_t)(480 - path[4]), path[5], 8, FACE_COLOUR_ACCENT, opacity);
    }

    const float tilt = animation_value(tilt_keyframes, 3, elapsed_ms, 0, 3000);
    count = add_rotated_pill(shapes, count, 175, 210, 80, 150, 240, 240, tilt, FACE_COLOUR_EYE);
    count = add_rotated_pill(shapes, count, 305, 210, 80, 150, 240, 240, tilt, FACE_COLOUR_EYE);
    count = add_rotated_pill(shapes, count, 240, 328, 24, 18, 240, 240, tilt, FACE_COLOUR_EYE);
    return count;
}

static int build_speaking(face_shape_t *shapes, uint32_t elapsed_ms)
{
    int count = 0;
    const float bob = animation_value(bob_keyframes, 3, elapsed_ms, 0, 1800);
    const int16_t eye_y = (int16_t)lroundf(150 + bob);

    count = add_pill(shapes, count, 140, eye_y, 70, 120, FACE_COLOUR_EYE, FACE_OPAQUE);
    count = add_pill(shapes, count, 270, eye_y, 70, 120, FACE_COLOUR_EYE, FACE_OPAQUE);

    // The bars scale about their own centres, so the height changes but the
    // middle row of 328 stays put.
    static const uint32_t bar_delays[5] = {0, 150, 300, 450, 600};
    for (int bar = 0; bar < 5; bar++) {
        const float scale = animation_value(talk_keyframes, 3, elapsed_ms, bar_delays[bar], 900);
        const int16_t height = (int16_t)lroundf(56 * scale);
        const int16_t y = (int16_t)lroundf(300 + (56 - height) / 2.0f);
        count = add_filled_rectangle(shapes, count, (int16_t)(194 + 20 * bar), y, 12, height, 6, FACE_COLOUR_EYE,
                                     FACE_OPAQUE);
    }
    return count;
}

static int build_muted(face_shape_t *shapes, uint32_t elapsed_ms, float blink)
{
    (void)elapsed_ms;
    int count = 0;

    count = add_eye(shapes, count, 140, 190, 70, 80, FACE_COLOUR_MUTED, blink);
    count = add_eye(shapes, count, 270, 190, 70, 80, FACE_COLOUR_MUTED, blink);
    count = add_line(shapes, count, 200, 330, 280, 330, 8, FACE_COLOUR_MUTED, FACE_OPAQUE);
    for (int tooth = 0; tooth < 4; tooth++) {
        const int16_t x = (int16_t)(212 + 20 * tooth);
        count = add_line(shapes, count, x, 320, x, 340, 8, FACE_COLOUR_MUTED, FACE_OPAQUE);
    }

    // The microphone is a stroke centred on its path, so the rounded outline
    // is passed as the path's geometry and add_outline_rectangle() expands it.
    count = add_outline_rectangle(shapes, count, 403, 36, 22, 38, 11, 6, FACE_COLOUR_ALERT, FACE_OPAQUE);
    // The lower half of a circle: 0 degrees is 3 o'clock, 90 is 6 o'clock.
    count = add_arc(shapes, count, 414, 64, 21, 0, 180, 6, FACE_COLOUR_ALERT, FACE_OPAQUE);
    count = add_line(shapes, count, 414, 85, 414, 96, 6, FACE_COLOUR_ALERT, FACE_OPAQUE);
    count = add_line(shapes, count, 392, 34, 436, 98, 6, FACE_COLOUR_ALERT, FACE_OPAQUE);
    return count;
}

static int build_happy_eyes(face_shape_t *shapes, uint32_t elapsed_ms)
{
    (void)elapsed_ms;
    int count = 0;

    // The cheeks are the only filled shapes, and they sit behind the arcs.
    count = add_pill(shapes, count, 92, 265, 72, 40, FACE_COLOUR_CHEEK, FACE_OPAQUE);
    count = add_pill(shapes, count, 316, 265, 72, 40, FACE_COLOUR_CHEEK, FACE_OPAQUE);
    count = add_quadratic_arc(shapes, count, 120, 235, 165, 160, 210, 235, 20, FACE_COLOUR_EYE, FACE_OPAQUE);
    count = add_quadratic_arc(shapes, count, 270, 235, 315, 160, 360, 235, 20, FACE_COLOUR_EYE, FACE_OPAQUE);
    count = add_quadratic_arc(shapes, count, 212, 300, 240, 330, 268, 300, 10, FACE_COLOUR_EYE, FACE_OPAQUE);
    return count;
}

static int build_curious(face_shape_t *shapes, uint32_t elapsed_ms)
{
    int count = 0;

    count = add_pill(shapes, count, 85, 145, 140, 140, FACE_COLOUR_WHITE, FACE_OPAQUE);
    count = add_pill(shapes, count, 255, 145, 140, 140, FACE_COLOUR_WHITE, FACE_OPAQUE);

    // The look group is the irises, pupils and highlights; the whites stay
    // still. Rounding the offset to whole pixels means the group does not
    // repaint while a keyframe holds it at 0 or +/-10.
    const float look_x = animation_value(look_keyframes, 7, elapsed_ms, 0, 6000);
    count = add_pill(shapes, count, (int16_t)lroundf(160 + look_x - 46), 176, 92, 92, FACE_COLOUR_ACCENT,
                     FACE_OPAQUE);
    count = add_pill(shapes, count, (int16_t)lroundf(320 + look_x - 46), 176, 92, 92, FACE_COLOUR_ACCENT,
                     FACE_OPAQUE);
    count = add_pill(shapes, count, (int16_t)lroundf(160 + look_x - 24), 198, 48, 48, FACE_COLOUR_BLACK,
                     FACE_OPAQUE);
    count = add_pill(shapes, count, (int16_t)lroundf(320 + look_x - 24), 198, 48, 48, FACE_COLOUR_BLACK,
                     FACE_OPAQUE);
    count = add_pill(shapes, count, (int16_t)lroundf(174 + look_x - 13), 191, 26, 26, FACE_COLOUR_HIGHLIGHT,
                     FACE_OPAQUE);
    count = add_pill(shapes, count, (int16_t)lroundf(334 + look_x - 13), 191, 26, 26, FACE_COLOUR_HIGHLIGHT,
                     FACE_OPAQUE);
    count = add_pill(shapes, count, (int16_t)lroundf(146 + look_x - 6), 234, 12, 12, FACE_COLOUR_HIGHLIGHT,
                     FACE_OPAQUE);
    count = add_pill(shapes, count, (int16_t)lroundf(306 + look_x - 6), 234, 12, 12, FACE_COLOUR_HIGHLIGHT,
                     FACE_OPAQUE);

    // Two arcs that meet at (240,325). Their round ends overlap there, so the
    // join fills without a seam.
    count = add_quadratic_arc(shapes, count, 208, 325, 224, 345, 240, 325, 9, FACE_COLOUR_EYE, FACE_OPAQUE);
    count = add_quadratic_arc(shapes, count, 240, 325, 256, 345, 272, 325, 9, FACE_COLOUR_EYE, FACE_OPAQUE);
    return count;
}

// The robot's bubble is a radius-24 rectangle with a radius-6 bottom-left
// corner. Painting a 24x24 radius-6 square over the big rectangle's corner is
// how the design gets two radii out of one shape, and both pieces are the same
// opaque colour.
static int add_conversation_bubble(face_shape_t *shapes, int count, int16_t left, int16_t top, int16_t width,
                                   int16_t height)
{
    count = add_filled_rectangle(shapes, count, left, top, width, height, 24, FACE_COLOUR_BUBBLE, FACE_OPAQUE);
    return add_filled_rectangle(shapes, count, left, (int16_t)(top + height - 24), 24, 24, 6,
                                FACE_COLOUR_BUBBLE, FACE_OPAQUE);
}

// The user bubble is outline-only, so the filled-square trick used for the
// robot bubble would paint over the line instead of reshaping it. The big
// bottom-right corner is erased with a black square, then a small border-only
// rounded rectangle (bottom and right sides only) forms the 6 px tail. The
// mini rectangle also draws its bottom-left and top-right corners; two more
// black squares erase those, and two filled straight edges join the tail to
// the remaining big outline. Everything is a rectangle, with no arcs.
static int add_user_bubble(face_shape_t *shapes, int count, int32_t width, int32_t height, int16_t radius)
{
    const int16_t right = 452;
    const int16_t top = 28;
    const int16_t left = (int16_t)(right - width);
    const int16_t bottom = (int16_t)(top + height);

    count = add_outline_outer(shapes, count, left, top, right, bottom, radius, 2, FACE_BORDER_FULL,
                              FACE_COLOUR_USER_BORDER, FACE_OPAQUE);

    count = add_filled_rectangle(shapes, count, (int16_t)(right - radius), (int16_t)(bottom - radius), radius,
                                 radius, 0, FACE_COLOUR_BLACK, FACE_OPAQUE);
    count = add_outline_outer(shapes, count, (int16_t)(right - 12), (int16_t)(bottom - 12), right, bottom, 6, 2,
                              FACE_BORDER_RIGHT | FACE_BORDER_BOTTOM, FACE_COLOUR_USER_BORDER, FACE_OPAQUE);

    count = add_filled_rectangle(shapes, count, (int16_t)(right - 12), (int16_t)(bottom - 6), 6, 6, 0,
                                 FACE_COLOUR_BLACK, FACE_OPAQUE);
    count = add_filled_rectangle(shapes, count, (int16_t)(right - 6), (int16_t)(bottom - 12), 6, 6, 0,
                                 FACE_COLOUR_BLACK, FACE_OPAQUE);

    const int16_t straight_length = (int16_t)(radius - 6 + 1);
    count = add_filled_rectangle(shapes, count, (int16_t)(right - radius), (int16_t)(bottom - 1),
                                 straight_length, 2, 0, FACE_COLOUR_USER_BORDER, FACE_OPAQUE);
    return add_filled_rectangle(shapes, count, (int16_t)(right - 1), (int16_t)(bottom - radius), 2,
                                straight_length, 0, FACE_COLOUR_USER_BORDER, FACE_OPAQUE);
}

// The scrollbar is the part of the conversation screen the face renderer can
// draw from measurements alone. It is absent when the text fits, and the
// thumb uses the same scroll offset as the reply label so the two cannot drift
// apart.
static int32_t conversation_reply_shift(const face_context_t *context)
{
    return context->user_bubble_height > 0 ? context->user_bubble_height + 12 : 0;
}

static int add_conversation_scrollbar(face_shape_t *shapes, int count, int32_t track_y,
                                      const face_context_t *context)
{
    const int32_t viewport_height = context->bubble_height - 12;
    const int32_t content_height = context->text_height + 40;
    const int32_t scroll_distance = content_height - viewport_height;
    if (scroll_distance <= 0) {
        return count;
    }

    const int32_t track_height = context->bubble_height - 48;
    int32_t thumb_height = track_height * viewport_height / content_height;
    if (thumb_height < 1) {
        thumb_height = 1;
    }
    const int32_t thumb_y = track_y + (track_height - thumb_height) * context->scroll_offset / scroll_distance;

    count = add_filled_rectangle(shapes, count, 436, (int16_t)track_y, 4, (int16_t)track_height, 2,
                                 FACE_COLOUR_SCROLL_TRACK, FACE_OPAQUE);
    return add_filled_rectangle(shapes, count, 436, (int16_t)thumb_y, 4, (int16_t)thumb_height, 2,
                                FACE_COLOUR_SCROLL_THUMB, FACE_OPAQUE);
}

static int build_conversation_thinking(face_shape_t *shapes, uint32_t elapsed_ms,
                                        const face_context_t *context)
{
    const int16_t user_height = (int16_t)context->user_bubble_height;
    const int16_t dots_shift = user_height > 0 ? (int16_t)(user_height + 14) : 0;

    int count = 0;
    if (user_height > 0) {
        count = add_user_bubble(shapes, count, context->user_bubble_width, user_height, 22);
    }
    count = add_conversation_bubble(shapes, count, 28, (int16_t)(28 + dots_shift), 120, 60);

    for (int dot = 0; dot < 3; dot++) {
        const uint32_t delay = (uint32_t)dot * 200;
        const float bounce = animation_value(thinking_dot_y_keyframes, 4, elapsed_ms, delay, 1200);
        const float opacity = animation_value(thinking_dot_opacity_keyframes, 4, elapsed_ms, delay, 1200);
        const int16_t centre_x = (int16_t)(70 + 18 * dot);
        const int16_t centre_y = (int16_t)lroundf(60 + dots_shift + bounce);
        count = add_pill(shapes, count, (int16_t)(centre_x - 6), (int16_t)(centre_y - 6), 12, 12,
                         FACE_COLOUR_THINKING, opacity_from_fraction(opacity));
    }

    // The gaze group moves the two eye pills; the brow and mouth stay put.
    const float gaze_x = animation_value(thinking_gaze_x_keyframes, 3, elapsed_ms, 0, 4000);
    const float gaze_y = animation_value(thinking_gaze_y_keyframes, 3, elapsed_ms, 0, 4000);
    count = add_pill(shapes, count, (int16_t)lroundf(52 + gaze_x), (int16_t)lroundf(368 + gaze_y), 34, 50,
                     FACE_COLOUR_EYE, FACE_OPAQUE);
    count = add_pill(shapes, count, (int16_t)lroundf(98 + gaze_x), (int16_t)lroundf(360 + gaze_y), 34, 66,
                     FACE_COLOUR_EYE, FACE_OPAQUE);
    count = add_line(shapes, count, 50, 356, 86, 362, 5, FACE_COLOUR_EYE, FACE_OPAQUE);
    return add_line(shapes, count, 82, 444, 102, 444, 5, FACE_COLOUR_EYE, FACE_OPAQUE);
}

static int build_conversation_speaking(face_shape_t *shapes, uint32_t elapsed_ms, const face_context_t *context)
{
    const int32_t reply_shift = conversation_reply_shift(context);

    int count = 0;
    if (context->user_bubble_height > 0) {
        count = add_user_bubble(shapes, count, context->user_bubble_width, context->user_bubble_height, 18);
    }
    // The display caps the bubble at 262 px, so the drawn coordinate is safe
    // to narrow even though the context keeps it 32-bit.
    count = add_conversation_bubble(shapes, count, 28, (int16_t)(28 + reply_shift), 424,
                                    (int16_t)context->bubble_height);
    count = add_conversation_scrollbar(shapes, count, 52 + reply_shift, context);

    count = add_pill(shapes, count, 52, 360, 34, 62, FACE_COLOUR_EYE, FACE_OPAQUE);
    count = add_pill(shapes, count, 98, 360, 34, 62, FACE_COLOUR_EYE, FACE_OPAQUE);

    for (int bar = 0; bar < 3; bar++) {
        const float scale = animation_value(talk_keyframes, 3, elapsed_ms, (uint32_t)bar * 150, 900);
        const int16_t height = (int16_t)lroundf(22 * scale);
        const int16_t y = (int16_t)lroundf(432 + (22 - height) / 2.0f);
        count = add_filled_rectangle(shapes, count, (int16_t)(79 + 10 * bar), y, 7, height, 3,
                                     FACE_COLOUR_EYE, FACE_OPAQUE);
    }
    return count;
}

static int build_conversation_idle(face_shape_t *shapes, const face_context_t *context)
{
    const int32_t reply_shift = conversation_reply_shift(context);

    int count = 0;
    if (context->user_bubble_height > 0) {
        count = add_user_bubble(shapes, count, context->user_bubble_width, context->user_bubble_height, 18);
    }
    count = add_conversation_bubble(shapes, count, 28, (int16_t)(28 + reply_shift), 424,
                                    (int16_t)context->bubble_height);
    count = add_conversation_scrollbar(shapes, count, 52 + reply_shift, context);
    count = add_pill(shapes, count, 52, 386, 30, 56, FACE_COLOUR_EYE, FACE_OPAQUE);
    return add_pill(shapes, count, 94, 386, 30, 56, FACE_COLOUR_EYE, FACE_OPAQUE);
}

int faces_build(face_id_t face, uint32_t elapsed_ms, const face_context_t *context, face_shape_t *shapes)
{
    const float blink = evaluate_keyframes(blink_keyframes, 3, (float)context->blink_elapsed_ms);

    switch (face) {
    case FACE_IDLE_DEFAULT:
        return build_idle(shapes, elapsed_ms, blink);
    case FACE_LISTENING:
        return build_listening(shapes, elapsed_ms);
    case FACE_SPEAKING:
        return build_speaking(shapes, elapsed_ms);
    case FACE_MUTED:
        return build_muted(shapes, elapsed_ms, blink);
    case FACE_IDLE_HAPPY_EYES:
        return build_happy_eyes(shapes, elapsed_ms);
    case FACE_IDLE_CURIOUS:
        return build_curious(shapes, elapsed_ms);
    case FACE_CONVERSATION_THINKING:
        return build_conversation_thinking(shapes, elapsed_ms, context);
    case FACE_CONVERSATION_SPEAKING:
        return build_conversation_speaking(shapes, elapsed_ms, context);
    case FACE_CONVERSATION_IDLE:
        return build_conversation_idle(shapes, context);
    case FACE_COUNT:
        break;
    }
    return 0;
}
