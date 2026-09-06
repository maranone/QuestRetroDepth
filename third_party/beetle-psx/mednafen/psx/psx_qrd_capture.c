#include "psx_qrd_capture.h"

#include <stdlib.h>
#include <string.h>

enum {
   PSX_QRD_OP_PRIMITIVE = 0,
   PSX_QRD_OP_FILL = 1,
   PSX_QRD_OP_COPY = 2,
   PSX_QRD_OP_LOAD_IMAGE = 3
};

static bool qrd_enabled;
static PsxQrdSceneView qrd_view;
static PsxQrdOp *qrd_ops;
static uint32_t qrd_ops_count;
static uint32_t qrd_ops_capacity;
static uint16_t *qrd_initial_vram;
static uint32_t qrd_initial_vram_count;
static char qrd_invalid_reason[96];

static void qrd_vertex(PsxQrdVertex *v, float x, float y, float w,
                       uint16_t u, uint16_t tv, uint32_t color,
                       const float *precise_rgb)
{
   v->x = x; v->y = y; v->w = w;
   v->valid_w = (w > 0.0f && w != 1.0f) ? 1 : 0;
   v->u = u; v->v = tv; v->color = color;
   if (precise_rgb)
      memcpy(v->precise_rgb, precise_rgb, sizeof(v->precise_rgb));
   else
   {
      v->precise_rgb[0] = (float)(color & 0xff) / 255.0f;
      v->precise_rgb[1] = (float)((color >> 8) & 0xff) / 255.0f;
      v->precise_rgb[2] = (float)((color >> 16) & 0xff) / 255.0f;
   }
}

static void qrd_init_primitive(PsxQrdPrimitive *p, uint8_t kind,
                               bool textured, bool gouraud, bool dither,
                               int blend_mode, bool mask_test, bool set_mask)
{
   memset(p, 0, sizeof(*p));
   p->kind = kind; p->textured = textured; p->gouraud = gouraud;
   p->dither = dither; p->blend_mode = (int8_t)blend_mode;
   p->mask_test = mask_test; p->set_mask = set_mask;
}

static void qrd_mark_invalid(const char *reason)
{
   qrd_view.replayable = 0;
   if (reason)
   {
      strncpy(qrd_invalid_reason, reason, sizeof(qrd_invalid_reason) - 1);
      qrd_invalid_reason[sizeof(qrd_invalid_reason) - 1] = '\0';
      qrd_view.invalid_reason = qrd_invalid_reason;
   }
}

static PsxQrdOp *qrd_append(void)
{
   PsxQrdOp *op;
   if (!qrd_enabled || !qrd_view.replayable)
      return NULL;
   if (qrd_ops_count == qrd_ops_capacity)
   {
      uint32_t next = qrd_ops_capacity ? qrd_ops_capacity * 2 : 1024;
      PsxQrdOp *grown = (PsxQrdOp *)realloc(qrd_ops, next * sizeof(*qrd_ops));
      if (!grown)
      {
         qrd_mark_invalid("scene command allocation failed");
         return NULL;
      }
      qrd_ops = grown;
      qrd_ops_capacity = next;
   }
   op = &qrd_ops[qrd_ops_count++];
   memset(op, 0, sizeof(*op));
   qrd_view.ops = qrd_ops;
   qrd_view.op_count = qrd_ops_count;
   return op;
}

static void qrd_push_primitive(const PsxQrdPrimitive *primitive, uint8_t kind)
{
   PsxQrdOp *op;
   if (!primitive)
      return;
   op = qrd_append();
   if (!op)
      return;
   op->kind = PSX_QRD_OP_PRIMITIVE;
   op->primitive = *primitive;
   op->primitive.kind = kind;
   qrd_view.primitive_count++;
}

void psx_qrd_scene_set_enabled(bool enabled) { qrd_enabled = enabled; }
bool psx_qrd_scene_capture_active(void) { return qrd_enabled; }

void psx_qrd_scene_begin(uint16_t *vram, uint32_t vram_width,
                         uint32_t vram_height, uint8_t upscale_shift)
{
   uint64_t count;
   psx_qrd_scene_reset();
   if (!qrd_enabled || !vram || !vram_width || !vram_height)
      return;
   count = (uint64_t)vram_width * vram_height;
   if (count > UINT32_MAX)
      return;
   qrd_initial_vram = (uint16_t *)malloc((size_t)count * sizeof(uint16_t));
   if (!qrd_initial_vram)
   {
      qrd_mark_invalid("initial VRAM allocation failed");
      return;
   }
   memcpy(qrd_initial_vram, vram, (size_t)count * sizeof(uint16_t));
   qrd_initial_vram_count = (uint32_t)count;
   qrd_view.vram_width = vram_width;
   qrd_view.vram_height = vram_height;
   qrd_view.upscale_shift = upscale_shift;
   qrd_view.initial_vram = qrd_initial_vram;
   qrd_view.initial_vram_count = qrd_initial_vram_count;
   qrd_view.replayable = 1;
}

void psx_qrd_scene_push_triangle(const PsxQrdPrimitive *p) { qrd_push_primitive(p, 0); }
void psx_qrd_scene_push_quad(const PsxQrdPrimitive *p) { qrd_push_primitive(p, 1); }
void psx_qrd_scene_push_line(const PsxQrdPrimitive *p) { qrd_push_primitive(p, 2); }
void psx_qrd_scene_push_sprite(const PsxQrdPrimitive *p) { qrd_push_primitive(p, 3); }

void psx_qrd_scene_push_triangle_rhi(
      float p0x, float p0y, float p0w, float p1x, float p1y, float p1w,
      float p2x, float p2y, float p2w, uint32_t c0, uint32_t c1, uint32_t c2,
      const float *rgb, uint16_t t0x, uint16_t t0y, uint16_t t1x, uint16_t t1y,
      uint16_t t2x, uint16_t t2y, uint16_t min_u, uint16_t min_v,
      uint16_t max_u, uint16_t max_v, uint16_t page_x, uint16_t page_y,
      uint16_t clut_x, uint16_t clut_y, uint8_t tbm, uint8_t depth_shift,
      bool dither, int blend_mode, bool mask_test, bool set_mask, bool textured)
{
   PsxQrdPrimitive p;
   qrd_init_primitive(&p, 0, textured, rgb != NULL, dither, blend_mode, mask_test, set_mask);
   p.coordinates_upscaled = 1;
   p.vertex_count = 3; p.min_u = min_u; p.min_v = min_v; p.max_u = max_u; p.max_v = max_v;
   p.texpage_x = page_x; p.texpage_y = page_y; p.clut_x = clut_x; p.clut_y = clut_y;
   p.texture_blend_mode = tbm; p.depth_shift = depth_shift;
   qrd_vertex(&p.vertices[0], p0x, p0y, p0w, t0x, t0y, c0, rgb ? rgb : NULL);
   qrd_vertex(&p.vertices[1], p1x, p1y, p1w, t1x, t1y, c1, rgb ? rgb + 3 : NULL);
   qrd_vertex(&p.vertices[2], p2x, p2y, p2w, t2x, t2y, c2, rgb ? rgb + 6 : NULL);
   psx_qrd_scene_push_triangle(&p);
}

void psx_qrd_scene_push_quad_rhi(
      float p0x, float p0y, float p0w, float p1x, float p1y, float p1w,
      float p2x, float p2y, float p2w, float p3x, float p3y, float p3w,
      uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3, const float *rgb,
      uint16_t t0x, uint16_t t0y, uint16_t t1x, uint16_t t1y, uint16_t t2x,
      uint16_t t2y, uint16_t t3x, uint16_t t3y, uint16_t min_u, uint16_t min_v,
      uint16_t max_u, uint16_t max_v, uint16_t page_x, uint16_t page_y,
      uint16_t clut_x, uint16_t clut_y, uint8_t tbm, uint8_t depth_shift,
      bool dither, int blend_mode, bool mask_test, bool set_mask,
      bool is_sprite, bool may_be_2d, bool textured)
{
   PsxQrdPrimitive p;
   qrd_init_primitive(&p, is_sprite ? 3 : 1, textured, rgb != NULL, dither,
                      blend_mode, mask_test, set_mask);
   p.coordinates_upscaled = 1;
   p.vertex_count = 4; p.may_be_2d = may_be_2d; p.min_u = min_u; p.min_v = min_v;
   p.max_u = max_u; p.max_v = max_v; p.texpage_x = page_x; p.texpage_y = page_y;
   p.clut_x = clut_x; p.clut_y = clut_y; p.texture_blend_mode = tbm; p.depth_shift = depth_shift;
   qrd_vertex(&p.vertices[0], p0x, p0y, p0w, t0x, t0y, c0, rgb ? rgb : NULL);
   qrd_vertex(&p.vertices[1], p1x, p1y, p1w, t1x, t1y, c1, rgb ? rgb + 3 : NULL);
   qrd_vertex(&p.vertices[2], p2x, p2y, p2w, t2x, t2y, c2, rgb ? rgb + 6 : NULL);
   qrd_vertex(&p.vertices[3], p3x, p3y, p3w, t3x, t3y, c3, rgb ? rgb + 9 : NULL);
   qrd_push_primitive(&p, is_sprite ? 3 : 1);
}

void psx_qrd_scene_push_line_rhi(
      int16_t p0x, int16_t p0y, int16_t p1x, int16_t p1y,
      uint32_t c0, uint32_t c1, bool dither, int blend_mode,
      bool mask_test, bool set_mask)
{
   PsxQrdPrimitive p;
   qrd_init_primitive(&p, 2, false, c0 != c1, dither, blend_mode, mask_test, set_mask);
   p.vertex_count = 2;
   qrd_vertex(&p.vertices[0], p0x, p0y, 1.0f, 0, 0, c0, NULL);
   qrd_vertex(&p.vertices[1], p1x, p1y, 1.0f, 0, 0, c1, NULL);
   qrd_push_primitive(&p, 2);
}

void psx_qrd_scene_push_sprite_rhi(
      int32_t x, int32_t y, int32_t w, int32_t h, uint8_t u, uint8_t v,
      uint32_t color, uint32_t clut, uint16_t page_x, uint16_t page_y,
      bool textured, bool texture_modulate, uint8_t depth_shift,
      int blend_mode, bool dither, bool mask_test, bool set_mask)
{
   PsxQrdPrimitive p;
   const uint16_t clut_x = (uint16_t)(clut & (0x3f << 4));
   const uint16_t clut_y = (uint16_t)((clut >> 10) & 0x1ff);
   qrd_init_primitive(&p, 3, textured, texture_modulate, dither, blend_mode,
                      mask_test, set_mask);
   p.vertex_count = 4; p.may_be_2d = true; p.depth_shift = depth_shift;
   p.texpage_x = page_x; p.texpage_y = page_y; p.clut_x = clut_x; p.clut_y = clut_y;
   p.min_u = u; p.min_v = v; p.max_u = (uint16_t)(u + w - 1); p.max_v = (uint16_t)(v + h - 1);
   qrd_vertex(&p.vertices[0], (float)x, (float)y, 1.0f, u, v, color, NULL);
   qrd_vertex(&p.vertices[1], (float)(x + w), (float)y, 1.0f, (uint16_t)(u + w), v, color, NULL);
   qrd_vertex(&p.vertices[2], (float)x, (float)(y + h), 1.0f, u, (uint16_t)(v + h), color, NULL);
   qrd_vertex(&p.vertices[3], (float)(x + w), (float)(y + h), 1.0f,
              (uint16_t)(u + w), (uint16_t)(v + h), color, NULL);
   qrd_push_primitive(&p, 3);
}

void psx_qrd_scene_push_fill(uint32_t color, uint16_t x, uint16_t y,
                             uint16_t width, uint16_t height)
{
   PsxQrdOp *op = qrd_append();
   if (!op) return;
   op->kind = PSX_QRD_OP_FILL;
   op->color = color; op->x = x; op->y = y;
   op->width = width; op->height = height;
   qrd_view.vram_op_count++;
}

void psx_qrd_scene_push_copy(uint16_t source_x, uint16_t source_y,
                             uint16_t dest_x, uint16_t dest_y,
                             uint16_t width, uint16_t height,
                             bool mask_test, bool set_mask)
{
   PsxQrdOp *op = qrd_append();
   if (!op) return;
   op->kind = PSX_QRD_OP_COPY;
   op->source_x = source_x; op->source_y = source_y;
   op->x = dest_x; op->y = dest_y;
   op->width = width; op->height = height;
   op->mask_test = mask_test; op->set_mask = set_mask;
   qrd_view.vram_op_count++;
}

void psx_qrd_scene_push_load_image(uint16_t x, uint16_t y, uint16_t width,
                                   uint16_t height, const uint16_t *vram,
                                   bool mask_test, bool set_mask)
{
   PsxQrdOp *op;
   uint64_t count = (uint64_t)width * height;
   if (!vram || count > UINT32_MAX)
   {
      qrd_mark_invalid("invalid image upload");
      return;
   }
   op = qrd_append();
   if (!op) return;
   op->payload = (uint16_t *)malloc((size_t)count * sizeof(uint16_t));
   if (!op->payload)
   {
      qrd_mark_invalid("image upload allocation failed");
      return;
   }
   for (uint32_t row = 0; row < height; ++row)
      for (uint32_t col = 0; col < width; ++col)
         ((uint16_t *)op->payload)[row * width + col] =
            vram[((uint32_t)y + row) % qrd_view.vram_height * qrd_view.vram_width +
                 ((uint32_t)x + col) % qrd_view.vram_width];
   op->kind = PSX_QRD_OP_LOAD_IMAGE;
   op->x = x; op->y = y; op->width = width; op->height = height;
   op->mask_test = mask_test; op->set_mask = set_mask;
   op->payload_count = (uint32_t)count;
   qrd_view.vram_op_count++;
}

void psx_qrd_scene_end(uint32_t width, uint32_t height,
                       uint16_t display_x, uint16_t display_y,
                       uint16_t display_width, uint16_t display_height)
{
   if (!qrd_enabled) return;
   qrd_view.width = width; qrd_view.height = height;
   qrd_view.display_x = display_x; qrd_view.display_y = display_y;
   qrd_view.display_width = display_width; qrd_view.display_height = display_height;
}

const PsxQrdSceneView *psx_qrd_scene_get(void)
{
   return qrd_enabled && qrd_view.replayable ? &qrd_view : NULL;
}

void psx_qrd_scene_reset(void)
{
   uint32_t i;
   for (i = 0; i < qrd_ops_count; ++i)
      free((void *)qrd_ops[i].payload);
   free(qrd_ops); qrd_ops = NULL;
   qrd_ops_count = 0; qrd_ops_capacity = 0;
   free(qrd_initial_vram); qrd_initial_vram = NULL;
   qrd_initial_vram_count = 0;
   memset(&qrd_view, 0, sizeof(qrd_view));
   qrd_view.replayable = 1;
   qrd_invalid_reason[0] = '\0';
}
