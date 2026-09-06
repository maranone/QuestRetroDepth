#pragma once
#include <cstdint>

extern "C" {

// Returns a pointer to the main-screen visible-source ownership rows that
// correspond to the frame at `video_data`.
// Values: 0-3 = BG0-BG3, 4 = OBJ, 5 = backdrop, 255 = none.
const uint8_t* snes9x_get_visible_source_for_frame(const void* video_data,
                                                   unsigned* out_stride);

} // extern "C"
