#include "faces.h"

// The table itself is generated, because its contents are derived from the
// image files. This module exists so callers do not have to know that, and so
// the generated file stays a plain translation of the PNGs.
#include "faces_generated.h"

const face_asset_t *faces_get(face_id_t id)
{
    return &face_generated_assets[id];
}
