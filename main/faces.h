#pragma once

#include <stdbool.h>
#include <stdint.h>

// The expressions the screen can show. These are no longer images: faces.c
// turns one into a list of primitives, which display.c draws. FACE_COUNT has
// to stay last; nothing indexes a table by these values any more.
typedef enum {
    FACE_IDLE_DEFAULT,
    FACE_LISTENING,
    FACE_SPEAKING,
    FACE_MUTED,
    FACE_IDLE_HAPPY_EYES,
    FACE_IDLE_CURIOUS,
    FACE_CONVERSATION_THINKING,
    FACE_CONVERSATION_SPEAKING,
    FACE_CONVERSATION_IDLE,
    FACE_COUNT,
} face_id_t;

// One drawing primitive in a face. Coordinates are in the 480x480 frame.
//
// The fields mean different things per kind, because a single flat struct is
// easier to compare between animation frames than a tagged union, and the
// comparison is what decides which parts of the screen get repainted:
//   filled rect:  (x1,y1) top left and (x2,y2) bottom right, radius
//   outline rect: as above; the rectangle already includes the stroke, so it
//                 is the outline's outer edge. border_sides selects which
//                 sides are drawn, for the user bubble's tail corner.
//   arc:          (x1,y1) centre, radius is the centreline (not the outer
//                 edge), start_angle and end_angle in LVGL degrees (0 is
//                 3 o'clock, increasing clockwise), stroke_width
//   line:         (x1,y1) to (x2,y2), stroke_width; the ends are round, so
//                 the stroke reaches half its width beyond each point
typedef enum {
    FACE_SHAPE_FILLED_RECT,
    FACE_SHAPE_OUTLINE_RECT,
    FACE_SHAPE_ARC,
    FACE_SHAPE_LINE,
} face_shape_kind_t;

#define FACE_MAX_SHAPES 16

// Which sides a FACE_SHAPE_OUTLINE_RECT draws. The user bubble uses a small
// bottom-right-only outline rect for its tail.
#define FACE_BORDER_LEFT 0x01
#define FACE_BORDER_RIGHT 0x02
#define FACE_BORDER_TOP 0x04
#define FACE_BORDER_BOTTOM 0x08
#define FACE_BORDER_FULL (FACE_BORDER_LEFT | FACE_BORDER_RIGHT | FACE_BORDER_TOP | FACE_BORDER_BOTTOM)

// The eyes close from full height to 0.08 and reopen over this long. The
// renderer needs the total only to know when the animation is over; faces.c
// owns the two segments' timing.
#define FACE_BLINK_DURATION_MILLISECONDS 250

// One full look cycle of the curious face: centre, left, right, centre. The
// renderer holds a curious variety moment for exactly this long, so every
// moment shows the whole cycle and hands back to idle with the eyes centred.
#define FACE_CURIOUS_LOOK_PERIOD_MILLISECONDS 6000

typedef struct {
    int16_t x1;
    int16_t y1;
    int16_t x2;
    int16_t y2;
    int16_t radius;
    int16_t start_angle;
    int16_t end_angle;
    uint8_t kind;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t opacity;
    uint8_t stroke_width;
    uint8_t border_sides;
} face_shape_t;

// Fills shapes with the primitives for one face and returns how many. The
// list is pure: no LVGL objects, no state, and the same arguments always
// produce the same shapes. elapsed_ms is the time since the face appeared, so
// its animations start from their first keyframe on every face change.
// context carries the values the conversation screen measures but the face
// renderer cannot: the blink time, the reply bubble's height and scroll, and
// the user transcript bubble's size. The full-size faces ignore the
// conversation fields. The caller must pass room for FACE_MAX_SHAPES.
typedef struct {
    uint32_t blink_elapsed_ms;
    // 32-bit because a 1024-byte reply full of newlines is about 35,000 px
    // tall, which neither the text height nor the scroll distance fits in
    // 16 bits. Screen coordinates are still clamped before they reach a shape.
    int32_t bubble_height;
    int32_t text_height;
    int32_t scroll_offset;
    // Zero when the transcript is empty or the screen is a full-size layout.
    // In C2 the height is always 44; in C1 it is the measured wrapped bubble.
    int32_t user_bubble_width;
    int32_t user_bubble_height;
} face_context_t;

int faces_build(face_id_t face, uint32_t elapsed_ms, const face_context_t *context, face_shape_t *shapes);
