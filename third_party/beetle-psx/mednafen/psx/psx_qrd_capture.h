#ifndef PSX_QRD_CAPTURE_H
#define PSX_QRD_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#define PSX_QRD_MAX_VERTICES 4

typedef struct PsxQrdVertex {
   float x, y, w;
   uint8_t valid_w;
   uint16_t u, v;
   uint32_t color;
   float precise_rgb[3];
} PsxQrdVertex;

typedef struct PsxQrdPrimitive {
   uint8_t kind;
   uint8_t vertex_count;
   PsxQrdVertex vertices[PSX_QRD_MAX_VERTICES];
   uint16_t min_u, min_v, max_u, max_v;
   uint16_t texpage_x, texpage_y, clut_x, clut_y;
   uint8_t texture_blend_mode;
   uint8_t depth_shift;
   int8_t blend_mode;
   uint8_t coordinates_upscaled;
   uint8_t textured, gouraud, dither, mask_test, set_mask, may_be_2d;
} PsxQrdPrimitive;

typedef struct PsxQrdOp {
   uint8_t kind;
   PsxQrdPrimitive primitive;
   uint16_t x, y, width, height;
   uint16_t source_x, source_y;
   uint32_t color;
   uint8_t mask_test, set_mask;
   const uint16_t *payload;
   uint32_t payload_count;
} PsxQrdOp;

typedef struct PsxQrdSceneView {
   uint32_t width, height;
   uint32_t vram_width, vram_height;
   uint8_t upscale_shift;
   uint16_t display_x, display_y, display_width, display_height;
   uint8_t replayable;
   const char *invalid_reason;
   const uint16_t *initial_vram;
   uint32_t initial_vram_count;
   const PsxQrdOp *ops;
   uint32_t op_count;
   uint32_t primitive_count, vram_op_count;
} PsxQrdSceneView;

#ifdef __cplusplus
extern "C" {
#endif

void psx_qrd_scene_set_enabled(bool enabled);
bool psx_qrd_scene_capture_active(void);
void psx_qrd_scene_begin(uint16_t *vram, uint32_t vram_width,
                         uint32_t vram_height, uint8_t upscale_shift);
void psx_qrd_scene_push_triangle(const PsxQrdPrimitive *primitive);
void psx_qrd_scene_push_quad(const PsxQrdPrimitive *primitive);
void psx_qrd_scene_push_line(const PsxQrdPrimitive *primitive);
void psx_qrd_scene_push_sprite(const PsxQrdPrimitive *primitive);
void psx_qrd_scene_push_fill(uint32_t color, uint16_t x, uint16_t y,
                             uint16_t width, uint16_t height);
void psx_qrd_scene_push_copy(uint16_t source_x, uint16_t source_y,
                             uint16_t dest_x, uint16_t dest_y,
                             uint16_t width, uint16_t height,
                             bool mask_test, bool set_mask);
void psx_qrd_scene_push_load_image(uint16_t x, uint16_t y, uint16_t width,
                                   uint16_t height, const uint16_t *vram,
                                   bool mask_test, bool set_mask);
void psx_qrd_scene_push_triangle_rhi(
      float p0x, float p0y, float p0w, float p1x, float p1y, float p1w,
      float p2x, float p2y, float p2w, uint32_t c0, uint32_t c1, uint32_t c2,
      const float *precise_rgb, uint16_t t0x, uint16_t t0y,
      uint16_t t1x, uint16_t t1y, uint16_t t2x, uint16_t t2y,
      uint16_t min_u, uint16_t min_v, uint16_t max_u, uint16_t max_v,
      uint16_t texpage_x, uint16_t texpage_y, uint16_t clut_x, uint16_t clut_y,
      uint8_t texture_blend_mode, uint8_t depth_shift, bool dither,
      int blend_mode, bool mask_test, bool set_mask, bool textured);
void psx_qrd_scene_push_quad_rhi(
      float p0x, float p0y, float p0w, float p1x, float p1y, float p1w,
      float p2x, float p2y, float p2w, float p3x, float p3y, float p3w,
      uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3,
      const float *precise_rgb, uint16_t t0x, uint16_t t0y,
      uint16_t t1x, uint16_t t1y, uint16_t t2x, uint16_t t2y,
      uint16_t t3x, uint16_t t3y, uint16_t min_u, uint16_t min_v,
      uint16_t max_u, uint16_t max_v, uint16_t texpage_x, uint16_t texpage_y,
      uint16_t clut_x, uint16_t clut_y, uint8_t texture_blend_mode,
      uint8_t depth_shift, bool dither, int blend_mode, bool mask_test,
      bool set_mask, bool is_sprite, bool may_be_2d, bool textured);
void psx_qrd_scene_push_line_rhi(
      int16_t p0x, int16_t p0y, int16_t p1x, int16_t p1y,
      uint32_t c0, uint32_t c1, bool dither, int blend_mode,
      bool mask_test, bool set_mask);
void psx_qrd_scene_push_sprite_rhi(
      int32_t x, int32_t y, int32_t w, int32_t h, uint8_t u, uint8_t v,
      uint32_t color, uint32_t clut, uint16_t texpage_x, uint16_t texpage_y,
      bool textured, bool texture_modulate, uint8_t depth_shift,
      int blend_mode, bool dither, bool mask_test, bool set_mask);
void psx_qrd_scene_end(uint32_t width, uint32_t height,
                       uint16_t display_x, uint16_t display_y,
                       uint16_t display_width, uint16_t display_height);
const PsxQrdSceneView *psx_qrd_scene_get(void);
void psx_qrd_scene_reset(void);

#ifdef __cplusplus
}
#endif

#endif
