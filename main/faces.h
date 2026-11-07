#pragma once

#include "lvgl.h"

// One image per assistant state. The values index the generated asset table,
// so they have to stay dense from zero; FACE_COUNT is the table's size.
typedef enum {
    FACE_IDLE_DEFAULT,
    FACE_LISTENING,
    FACE_THINKING,
    FACE_SPEAKING,
    FACE_MUTED,
    FACE_BLINK,
    FACE_IDLE_HAPPY_EYES,
    FACE_IDLE_CURIOUS,
    FACE_COUNT,
} face_id_t;

typedef struct {
    const lv_image_dsc_t *image;
    // Where the cropped image belongs in the 480x480 frame. Drawing it at this
    // offset reproduces the face's position in the source PNG even though the
    // transparent margin around it was dropped to save flash.
    int16_t x;
    int16_t y;
} face_asset_t;

// Returns the image for a face and its position in the frame. The image data
// is const and lives in flash, so nothing is allocated or copied.
const face_asset_t *faces_get(face_id_t id);
