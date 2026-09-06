#include "gles_renderer.h"
#include "lightgun_model.h"
#include "presentation_shared.h"
#include <android/log.h>
#include <EGL/egl.h>
#include <sys/system_properties.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <functional>
#include <limits>
// CGLTF_IMPLEMENTATION is defined once, in cgltf_impl.cpp -- this
// translation unit only needs the declarations.
#include "cgltf.h"
// Decodes the controller render model's embedded base-color texture (the
// black top plate / logo / grip pattern live there, not as separate
// geometry) -- this is the only translation unit that needs it, same inline-
// implementation pattern as STB_IMAGE_WRITE_IMPLEMENTATION in
// neogeo_palette_debug.cpp.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "third_party_stb/stb_image.h"

// GL_EXT_disjoint_timer_query — not in the GLES3 headers we include, and the extension isn't
// guaranteed present, so declare just what we use and load it dynamically at init time.
#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif

#define LOG_TAG "QrdGles"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// LayerFrame::side_color_mode -> flat RGB for the box shader's uSideColorRGB.
// 0 (Ori) is never looked up here — callers gate on mode > 0 before using this.
static void side_color_mode_to_rgb(int mode, float& r, float& g, float& b) {
    switch (mode) {
        case 1: r = 0.02f; g = 0.02f; b = 0.02f; break; // Black
        case 2: r = 0.95f; g = 0.95f; b = 0.95f; break; // White
        case 3: r = 0.90f; g = 0.15f; b = 0.15f; break; // Red
        case 4: r = 0.20f; g = 0.85f; b = 0.30f; break; // Green
        case 5: r = 0.20f; g = 0.45f; b = 0.95f; break; // Blue
        default: r = 0.0f;  g = 0.0f;  b = 0.0f;  break;
    }
}

// ---------------------------------------------------------------------------
// GLSL shaders
// ---------------------------------------------------------------------------

static const char* kLayerVS = R"GLSL(#version 310 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;

layout(std430, binding = 0) readonly buffer ObjectBoxes {
    vec4 uBoxRects[];   // (u0, v0, u1, v1) per detected object, UV space
};

layout(std430, binding = 1) readonly buffer ObjectDepths { float uObjectDepths[]; };

uniform mat4  uVP;
uniform float uDepth;
uniform float uQuadW;
uniform float uQuadH;
uniform float uQuadY;
uniform float uRoundness;
uniform float uCopyCount;
uniform float uCopySpan;
uniform float uDepthMap;
uniform float uBboxMode;
uniform float uZBufferDepths;
uniform float uScreenCurve;
uniform float uSubrectEnable;
uniform vec4  uSubrect;
uniform float uInstanceBase;
// Rotate Screen: 0=off, 1=90 degrees, 2=180 degrees, 3=270 degrees. Rotates
// the sampled texture so a natively-portrait arcade board (e.g. 1941) reads
// upright on the quad instead of squished into the normal landscape aspect.
uniform float uRotateMode;
// Table Mode: lays the panel flat (normal pointing straight up) instead of
// the normal vertical/tilted spherical-arc placement, so the viewer looks
// down at it like a cocktail-cabinet screen. uQuadY carries this layer's
// CPU-computed stack height (see gles_renderer.cpp's table_stack_t) instead
// of the usual small vertical nudge.
uniform float uTableMode;
// When set, skips the max(0.01, ...) clamp on d below, allowing genuinely negative depth —
// used only by the Symmetric geometry mode's mirrored copy, which is meant to sit behind the
// viewer's origin rather than collapse to right in front of the eye.
uniform float uAllowBehind;
// Canvas placement (edit mode)
uniform float uCanvasX;   // horizontal translation (metres)
uniform float uCanvasY;   // vertical translation (metres)
uniform float uCanvasAz;  // azimuth arc angle (radians)
// Layer-deck bookshelf yaw (radians): rotates THIS layer in place about its
// own vertical axis, leaving its centre untouched. 0 for every normal draw.
uniform float uLayerYaw;
uniform float uCanvasEl;  // elevation arc angle (radians)
uniform float     uCanvasScale;
uniform int       uHasYDepth;    // 1 = sprite Y-depth active
uniform sampler2D uYDepthTex;    // GL_R8 depth map on texture unit 1
uniform float     uYDepthSpread; // total Z range in metres

out vec2  vUV;
out float vCopyT;
out vec3  vNormal;

void main() {
    float copy_count = max(1.0, uCopyCount);
    // BBox mode packs (boxIndex, copyIndex) into a single instanced draw: all boxes ×
    // all copies for this layer in one glDrawArraysInstanced call.
    int copyIndex = uBboxMode > 0.5 ? (gl_InstanceID % int(copy_count)) : gl_InstanceID;
    int boxIndex  = uBboxMode > 0.5 ? (gl_InstanceID / int(copy_count)) : 0;
    vec4 boxRect  = uBboxMode > 0.5 ? uBoxRects[boxIndex] : vec4(0.0, 0.0, 1.0, 1.0);

    // BBox: reverse instance order so the deepest copy (copyIndex=0) is drawn first
    // and the shallowest last — correct back-to-front order for alpha compositing.
    // Normal: standard front-to-back (closest to viewer last so it composites on top).
    float inst = uBboxMode > 0.5 ? (copy_count - float(copyIndex))
                                 : (float(gl_InstanceID) + uInstanceBase);
    float t = min(inst, copy_count) / copy_count;
    float offset = t * uCopySpan;
    // BBox mode: copies go DEEPER (into the screen) so the front face stays closest to
    // the viewer and the extrusion recedes away, making the object look 3D going inward.
    // Normal mode: copies come toward the viewer (retrodepth pop-out effect).
    float object_depth = (uZBufferDepths > 0.5 && uBboxMode > 0.5) ? uObjectDepths[boxIndex] : uDepth;
    float d = uBboxMode > 0.5 ? max(0.01, object_depth + offset)
             : (uAllowBehind > 0.5 ? (uDepth - offset) : max(0.01, uDepth - offset));

    // Sprite Y-depth: per-vertex Z displacement sampled from the depth texture.
    // dv=0 (top of screen) → farther; dv=1 (bottom) → closer.
    if (uHasYDepth != 0) {
        float dv = texture(uYDepthTex, aUV).r;
        d = max(0.01, d - (dv - 0.5) * uYDepthSpread);
    }

    // scale_x: horizontal-only width factor (bbox wedge expands width, not height).
    // scale: both-axis factor (WholeLayer bulge).
    float scale_x = 1.0;
    float scale   = 1.0;
    if (uBboxMode > 0.5) {
        // Ellipse/cylinder profile: wider ramp-in (0→35%), wide plateau (35%→65%),
        // symmetric ramp-out (65%→100%).
        float wedge = 0.0;
        if (t <= 0.35)      wedge = t / 0.35;
        else if (t < 0.65)  wedge = 1.0;
        else                wedge = clamp((1.0 - t) / 0.35, 0.0, 1.0);
        scale_x = 1.0 + wedge * 0.20;
    } else if (uDepthMap > 0.5 && uRoundness > 0.5) {
        // Tiny bulge only: layers are full-frame quads with large transparent margins,
        // so even small scale changes can visibly drag the sprite inside the frame.
        float wedge = 0.0;
        if (t <= 0.4) {
            wedge = t / 0.4;
        } else if (t < 0.6) {
            wedge = 1.0;
        } else {
            wedge = clamp((1.0 - t) / 0.4, 0.0, 1.0);
        }
        scale = 1.0 - wedge * 0.04; // -4% max at the thickest part of the stack
    }

    float cx = aPos.x * 2.0;
    float curve_offset = uScreenCurve * uQuadW * 0.18 * cx * cx;

    vec3 center, normal, right, up;
    if (uTableMode > 0.5) {
        // Flat tabletop: the quad lies in the XZ plane, its face pointing
        // straight up so a viewer standing above looking down sees the
        // front. uQuadY here is this layer's CPU-computed stack height
        // (table height at the base, rising per-layer toward the ceiling),
        // not the usual small vertical nudge. No screen curve in this mode.
        // Same cabinet placement as the box path's branch -- kept in lockstep
        // so the two paths never disagree about where the scene is.
        // Table/Ceiling share everything but facing, stack direction and
        // height. kTableDistance/kTableHeight/kCeilingHeight MUST stay
        // identical across all three layer programs and the CPU-side surface
        // in openxr_shell.cpp's canvas anchor -- more than one can draw in a
        // frame, and when they disagree the scene renders twice in two places.
        const float kTableDistance = 1.15;  // metres out from the play-space origin
        const float kTableHeight   = -0.62; // Table: metres below eye level
        const float kCeilingHeight =  0.78; // Ceiling: metres above eye level
        bool ceiling_mode = uTableMode > 1.5;
        float surf_y    = ceiling_mode ? kCeilingHeight : kTableHeight;
        float stack_dir = ceiling_mode ? -1.0 : 1.0;
        float t_cos = cos(uCanvasAz);
        float t_sin = sin(uCanvasAz);
        center = vec3( kTableDistance * t_sin + uCanvasX,
                       surf_y + stack_dir * uQuadY + uCanvasY,
                      -kTableDistance * t_cos);
        normal = vec3(0.0, ceiling_mode ? -1.0 : 1.0, 0.0);
        right  = vec3(t_cos, 0.0, t_sin);
        // Flipping `up` with the normal keeps the basis right-handed, so the
        // picture reads correctly when viewed from the other side instead of
        // mirrored.
        up     = ceiling_mode ? vec3(-t_sin, 0.0, t_cos) : vec3(t_sin, 0.0, -t_cos);
        curve_offset = 0.0;
    } else {
        // Spherical arc: canvas centre moves on sphere of radius d and rotates to
        // face the viewer (like a screen on the inside of a sphere).
        float cos_el = cos(uCanvasEl);
        float sin_el = sin(uCanvasEl);
        float cos_az = cos(uCanvasAz);
        float sin_az = sin(uCanvasAz);

        // Canvas centre position on sphere
        center = vec3(d * sin_az * cos_el + uCanvasX,
                      d * sin_el          + uQuadY + uCanvasY,
                     -d * cos_az * cos_el);

        // Outward normal (HMD → canvas centre)
        normal = vec3(sin_az * cos_el, sin_el, -cos_az * cos_el);

        // Tangent axes: right and up in the canvas plane
        right = vec3(cos_az,           0.0,     sin_az);
        up    = vec3(-sin_az * sin_el, cos_el, -cos_az * sin_el);

        // Bookshelf yaw: spin the quad about its own up axis, around its own
        // centre. Only the facing changes -- centre stays put.
        if (uLayerYaw != 0.0) {
            float cyw = cos(uLayerYaw);
            float syw = sin(uLayerYaw);
            vec3 yawed_right = right * cyw - normal * syw;
            normal = right * syw + normal * cyw;
            right  = yawed_right;
        }
    }

    vec4 subrect = uBboxMode > 0.5 ? boxRect : uSubrect;
    float subrectEnable = uBboxMode > 0.5 ? 1.0 : uSubrectEnable;
    float sub_w = mix(1.0, subrect.z - subrect.x, subrectEnable);
    float sub_h = mix(1.0, subrect.w - subrect.y, subrectEnable);
    float sub_cx = mix(0.5, 0.5 * (subrect.x + subrect.z), subrectEnable);
    float sub_cy = mix(0.5, 0.5 * (subrect.y + subrect.w), subrectEnable);
    float center_dx = (sub_cx - 0.5) * uQuadW * uCanvasScale;
    float center_dy = (0.5 - sub_cy) * uQuadH * uCanvasScale;

    float vx = aPos.x * uQuadW * scale_x * scale * uCanvasScale * sub_w;
    float vy = aPos.y * uQuadH *           scale * uCanvasScale * sub_h;

    // Screen curve pushes vertices along the outward normal
    gl_Position = uVP * vec4(center + right * (center_dx + vx) + up * (center_dy + vy) + normal * curve_offset, 1.0);
    vec2 rotUV = aUV;
    if (uRotateMode > 0.5 && uRotateMode < 1.5)      rotUV = vec2(1.0 - aUV.y, aUV.x);       // 90
    else if (uRotateMode > 1.5 && uRotateMode < 2.5) rotUV = vec2(1.0 - aUV.x, 1.0 - aUV.y); // 180
    else if (uRotateMode > 2.5)                      rotUV = vec2(aUV.y, 1.0 - aUV.x);       // 270
    vUV    = mix(rotUV, mix(subrect.xy, subrect.zw, rotUV), subrectEnable);
    vCopyT = t;
    vNormal = normal;
}
)GLSL";

static const char* kImmersiveLayerVS = R"GLSL(#version 310 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;

layout(std430, binding = 0) readonly buffer ObjectBoxes {
    vec4 uBoxRects[];   // (u0, v0, u1, v1) per detected object, UV space
};
layout(std430, binding = 1) readonly buffer ObjectDepths { float uObjectDepths[]; };

uniform mat4  uVP;
uniform float uDepth;
uniform float uQuadW;
uniform float uQuadH;
uniform float uQuadY;
uniform float uRoundness;
uniform float uCopyCount;
uniform float uCopySpan;
uniform float uDepthMap;
uniform float uBboxMode;
uniform float uZBufferDepths;
uniform float uScreenCurve;
uniform float uTiltX;
uniform float uTiltY;
uniform float uSubrectEnable;
uniform vec4  uSubrect;
uniform float uInstanceBase;
uniform float uAllowBehind; // see kLayerVS
uniform float uRotateMode; // see kLayerVS
uniform float uTableMode; // see kLayerVS
uniform float uCanvasX;
uniform float uCanvasY;
uniform float uCanvasAz;
uniform float uCanvasEl;
uniform float uLayerYaw; // see kLayerVS
uniform float uCanvasScale;
uniform int uHasYDepth;
uniform sampler2D uYDepthTex;
uniform float uYDepthSpread;

out vec2  vUV;
out float vCopyT;
out vec3  vNormal;

void main() {
    float copy_count = max(1.0, uCopyCount);
    int copyIndex = uBboxMode > 0.5 ? (gl_InstanceID % int(copy_count)) : gl_InstanceID;
    int boxIndex  = uBboxMode > 0.5 ? (gl_InstanceID / int(copy_count)) : 0;
    vec4 boxRect  = uBboxMode > 0.5 ? uBoxRects[boxIndex] : vec4(0.0, 0.0, 1.0, 1.0);

    float inst = uBboxMode > 0.5 ? (copy_count - float(copyIndex))
                                 : (float(gl_InstanceID) + uInstanceBase);
    float t = min(inst, copy_count) / copy_count;
    float offset = t * uCopySpan;
    float object_depth = (uZBufferDepths > 0.5 && uBboxMode > 0.5) ? uObjectDepths[boxIndex] : uDepth;
    float d = uBboxMode > 0.5 ? max(0.01, object_depth + offset)
             : (uAllowBehind > 0.5 ? (uDepth - offset) : max(0.01, uDepth - offset));
    if (uHasYDepth != 0) {
        float dv = texture(uYDepthTex, aUV).r;
        d = max(0.01, d - (dv - 0.5) * uYDepthSpread);
    }

    float scale_x = 1.0;
    float scale   = 1.0;
    if (uBboxMode > 0.5) {
        float wedge = 0.0;
        if (t <= 0.35)      wedge = t / 0.35;
        else if (t < 0.65)  wedge = 1.0;
        else                wedge = clamp((1.0 - t) / 0.35, 0.0, 1.0);
        scale_x = 1.0 + wedge * 0.20;
    } else if (uDepthMap > 0.5 && uRoundness > 0.5) {
        float wedge = 0.0;
        if (t <= 0.4) {
            wedge = t / 0.4;
        } else if (t < 0.6) {
            wedge = 1.0;
        } else {
            wedge = clamp((1.0 - t) / 0.4, 0.0, 1.0);
        }
        scale = 1.0 - wedge * 0.04;
    }

    vec3 center, normal, right, up, tilted_right, tilted_normal, tilted_up;
    if (uTableMode > 0.5) {
        // Flat tabletop -- see kLayerVS for the full rationale. No tilt/curve
        // in this mode.
        //
        // These constants MUST match kLayerVS and kBoxLayerVS exactly. There
        // are three table branches, one per layer program, and more than one
        // can be drawing in a given frame: when they disagree the same scene
        // is rendered twice in two different places, which reads as the depth
        // being "decoupled" -- a flat copy in one spot and an extruded copy in
        // another.
        // Table/Ceiling share everything but facing, stack direction and
        // height. kTableDistance/kTableHeight/kCeilingHeight MUST stay
        // identical across all three layer programs and the CPU-side surface
        // in openxr_shell.cpp's canvas anchor -- more than one can draw in a
        // frame, and when they disagree the scene renders twice in two places.
        const float kTableDistance = 1.15;  // metres out from the play-space origin
        const float kTableHeight   = -0.62; // Table: metres below eye level
        const float kCeilingHeight =  0.78; // Ceiling: metres above eye level
        bool ceiling_mode = uTableMode > 1.5;
        float surf_y    = ceiling_mode ? kCeilingHeight : kTableHeight;
        float stack_dir = ceiling_mode ? -1.0 : 1.0;
        float t_cos = cos(uCanvasAz);
        float t_sin = sin(uCanvasAz);
        center = vec3( kTableDistance * t_sin + uCanvasX,
                       surf_y + stack_dir * uQuadY + uCanvasY,
                      -kTableDistance * t_cos);
        normal = vec3(0.0, ceiling_mode ? -1.0 : 1.0, 0.0);
        right  = vec3(t_cos, 0.0, t_sin);
        // Flipping `up` with the normal keeps the basis right-handed, so the
        // picture reads correctly when viewed from the other side instead of
        // mirrored.
        up     = ceiling_mode ? vec3(-t_sin, 0.0, t_cos) : vec3(t_sin, 0.0, -t_cos);
        tilted_right = right; tilted_normal = normal; tilted_up = up;
    } else {
        float cos_el = cos(uCanvasEl);
        float sin_el = sin(uCanvasEl);
        float cos_az = cos(uCanvasAz);
        float sin_az = sin(uCanvasAz);

        center = vec3(d * sin_az * cos_el + uCanvasX,
                      d * sin_el          + uQuadY + uCanvasY,
                     -d * cos_az * cos_el);

        normal = vec3(sin_az * cos_el, sin_el, -cos_az * cos_el);
        right  = vec3(cos_az,           0.0,     sin_az);
        up     = vec3(-sin_az * sin_el, cos_el, -cos_az * sin_el);

        // Bookshelf yaw -- see kLayerVS. Applied before tilt so the tilt still
        // reads as a whole-canvas pitch/roll on top of the turned layer.
        if (uLayerYaw != 0.0) {
            float cyw = cos(uLayerYaw);
            float syw = sin(uLayerYaw);
            vec3 yawed_right = right * cyw - normal * syw;
            normal = right * syw + normal * cyw;
            right  = yawed_right;
        }

        float ctx = cos(uTiltX);
        float stx = sin(uTiltX);
        vec3 pitched_up     = up * ctx + normal * stx;
        vec3 pitched_normal = normal * ctx - up * stx;

        float cty = cos(uTiltY);
        float sty = sin(uTiltY);
        tilted_right  = right * cty - pitched_normal * sty;
        tilted_normal = right * sty + pitched_normal * cty;
        tilted_up     = pitched_up;
    }

    vec4 subrect = uBboxMode > 0.5 ? boxRect : uSubrect;
    float subrectEnable = uBboxMode > 0.5 ? 1.0 : uSubrectEnable;
    float sub_w = mix(1.0, subrect.z - subrect.x, subrectEnable);
    float sub_h = mix(1.0, subrect.w - subrect.y, subrectEnable);
    float sub_cx = mix(0.5, 0.5 * (subrect.x + subrect.z), subrectEnable);
    float sub_cy = mix(0.5, 0.5 * (subrect.y + subrect.w), subrectEnable);
    float center_dx = (sub_cx - 0.5) * uQuadW * uCanvasScale;
    float center_dy = (0.5 - sub_cy) * uQuadH * uCanvasScale;

    float vx = aPos.x * uQuadW * scale_x * scale * uCanvasScale * sub_w;
    float vy = aPos.y * uQuadH *           scale * uCanvasScale * sub_h;

    // The strip mesh supplies enough vertices for this depth shift to read as a curve.
    float edge_t = aPos.x * 2.0;
    float curve_offset = uTableMode > 0.5 ? 0.0 : uScreenCurve * uQuadW * 0.18 * edge_t * edge_t;

    gl_Position = uVP * vec4(center + tilted_right * (center_dx + vx) + tilted_up * (center_dy + vy) + tilted_normal * curve_offset, 1.0);
    vec2 rotUV = aUV;
    if (uRotateMode > 0.5 && uRotateMode < 1.5)      rotUV = vec2(1.0 - aUV.y, aUV.x);       // 90
    else if (uRotateMode > 1.5 && uRotateMode < 2.5) rotUV = vec2(1.0 - aUV.x, 1.0 - aUV.y); // 180
    else if (uRotateMode > 2.5)                      rotUV = vec2(aUV.y, 1.0 - aUV.x);       // 270
    vUV    = mix(rotUV, mix(subrect.xy, subrect.zw, rotUV), subrectEnable);
    vCopyT = t;
    vNormal = tilted_normal;
}
)GLSL";

// Real-geometry box: back face + 4 side faces for one detected object, instanced once per
// box via the same ObjectBoxes SSBO the card-stack path uses. The front face (the actual
// sprite) is still drawn separately by the existing quad program, on top of this.
//
// aPos: unit-box local space. x,y in [-0.5,0.5] match the box's on-screen footprint; z in
//       [-1,0] is the depth axis (0 = front, aligned with the sprite plane; -1 = back of the
//       box, before the uThickness scale is applied).
// aUV:  back face carries the normal 0..1 quad UV (mirrors the front sprite, per the "duplicate
//       the far side" approach — there is no real "behind" data to sample). Side faces carry
//       (depth_t, edge_t): depth_t is 0 at the front edge / 1 at the back edge; edge_t runs
//       along the side. Side faces sample a single edge column/row of the SAME texture
//       (GL_CLAMP_TO_EDGE — safe to sample past 0/1) so they read as a plausible continuation
//       of the sprite instead of a flat fill colour.
// aFace: 0=back, 1=left, 2=right, 3=top, 4=bottom.
static const char* kBoxLayerVS = R"GLSL(#version 310 es
layout(location = 0) in vec3  aPos;
layout(location = 1) in vec2  aUV;
layout(location = 2) in float aFace;

layout(std430, binding = 0) readonly buffer ObjectBoxes {
    vec4 uBoxRects[];   // (u0, v0, u1, v1) per detected object, UV space
};
layout(std430, binding = 1) readonly buffer ObjectDepths { float uObjectDepths[]; };

uniform mat4  uVP;
uniform float uDepth;
uniform float uZBufferDepths;
uniform float uQuadW;
uniform float uQuadH;
uniform float uQuadY;
uniform float uThickness;    // box depth in metres
uniform float uScreenCurve;
uniform float uTiltX;
uniform float uTiltY;
uniform float uCanvasX;
uniform float uCanvasY;
uniform float uCanvasAz;
// Table Mode: lays the whole stack flat as a tabletop, exactly as
// kLayerVS/kImmersiveLayerVS already do. Without this the real-geometry
// box path -- the one that draws the extruded 3D pixels, and the default
// -- had no table branch at all: it kept the normal upright spherical-arc
// placement and merely shifted each layer up by its stack height, so the
// scene came out as a jumble of floating vertical planes instead of a
// tabletop, and billboard-oriented layers additionally swung with the
// headset. uQuadY carries the CPU-computed stack height (see
// table_stack_t / eff_quad_y in render_eye), not the usual vertical nudge.
uniform float uTableMode;
uniform float uCanvasEl;
uniform float uLayerYaw; // see kLayerVS
uniform float uCanvasScale;
// 0 = Box (standing wall extrusion, original behaviour); 1 = Floor (laid flat, local y becomes
// depth recession, local z becomes a thin downward slab); 2 = Ceiling (same, slab goes upward).
// First-pass approximation — reuses the same 5-face mesh with axis roles reassigned rather than
// a dedicated floor/ceiling mesh; expect this to need visual tuning once tested in headset.
uniform float uOrientation;
// Skips the max(0.01, ...) clamp on d below, same purpose as kLayerVS's uAllowBehind — used by
// the Symmetric geometry mode's mirrored (behind-L1) box copy, whose depth is genuinely negative.
uniform float uAllowBehind;
// Nearest layer's own depth_meters — Floor/Ceiling recede from this layer's own depth (far edge)
// all the way to L1's depth, then continue the SAME span again past L1 toward the viewer (near
// edge), mirroring how the Symmetric geometry mode mirrors a whole box around L1.
uniform float uRefL1Depth;
// When on, clamps this box's Z extrusion to its own on-screen footprint instead of always using
// the full uThickness. A tiny (e.g. 1px) object extruded by the same absolute thickness as a
// large sprite becomes a thin rod pointed straight at the camera — its side walls are edge-on
// from a head-on view, so it reads as just a front/back pair with an invisible connection. Used
// by the per-object (one-box-per-detected-blob/pixel-run) draw path; whole-layer single-box
// modes (Symmetric/Split/plain WholeLayer) leave this off since their thickness is intentional.
uniform float uAutoThickness;
// DepthScatter: per-object depth jitter magnitude, metres. 0 = off (default at every OTHER mode's
// draw call, so it never leaks — see uYDepthRange's note on the same requirement).
uniform float uScatterRange;
// AutoYDepth: per-object depth-from-screen-row range, metres. 0 = off. Must be explicitly zeroed
// at every box-program draw call that ISN'T AutoYDepth's own — GL uniform state persists on the
// program across draw calls, so a stale nonzero value from a previous layer's AutoYDepth draw
// would otherwise silently bleed into the next layer's Box/Floor/etc draw this same frame.
uniform float uYDepthRange;
// Billboard (uOrientation == 5): HMD world position (app_space). Each box's right/up/normal basis
// is derived per-instance from (uHmdPos - box center) instead of the shared uCanvasAz/El basis
// used by every other orientation, so the box always faces the viewer.
uniform vec3  uHmdPos;
// SizeThickness: 1.0 = scale this box's extrusion by its own on-screen area (big object = thick,
// small object = thin), 0.0 = off. Same "must be explicitly zeroed at every other draw call"
// requirement as uScatterRange/uYDepthRange above.
uniform float uSizeThicknessMode;

out vec2  vUV;
out float vFace;
out float vDepthT; // 0 at front edge, 1 at back edge — used to shade the sides darker inward
out vec4  vSubrect; // passed through so the fragment stage can redo the side-face UV lookup
out float vEdgeT;   // 0..1 position along whichever side this vertex is on (= aUV.y)
out vec3  vNormal;

// Stable pseudo-random hash in [0,1], seeded from a box's own UV rect — same box always hashes to
// the same value every frame (no re-randomization), used by DepthScatter.
float hash01(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

void main() {
    int boxIndex = gl_InstanceID;
    vec4 subrect = uBoxRects[boxIndex];
    float sub_w = subrect.z - subrect.x;
    float sub_h = subrect.w - subrect.y;
    float sub_cx = 0.5 * (subrect.x + subrect.z);
    float sub_cy = 0.5 * (subrect.y + subrect.w);
    float center_dx = (sub_cx - 0.5) * uQuadW * uCanvasScale;
    float center_dy = (0.5 - sub_cy) * uQuadH * uCanvasScale;

    // Orientation codes: 0=Box, 1=Floor, 2=Ceiling (pure — tile a single edge row across the
    // whole depth span), 3=SplitFloor region, 4=SplitCeiling region (stretch their already-
    // cropped row band across depth instead — see the UV block below), 5=Billboard (standing
    // extrusion like Box, but with a per-instance HMD-facing basis instead of the shared canvas
    // basis — must NOT be caught by the flat_mode/is_ceiling ranges below, hence the upper bounds),
    // 6=SplitLeft region, 7=SplitRight region (mirror of 3/4 but swept along the horizontal axis
    // instead of vertical — see the is_side branch in the position block below).
    bool flat_mode = (uOrientation > 0.5 && uOrientation < 4.5) || (uOrientation > 5.5 && uOrientation < 7.5);
    bool is_ceiling = (uOrientation > 1.5 && uOrientation < 2.5) || (uOrientation > 3.5 && uOrientation < 4.5);
    bool is_billboard = uOrientation > 4.5 && uOrientation < 5.5;
    bool is_side = uOrientation > 5.5 && uOrientation < 7.5;
    bool is_right = uOrientation > 6.5 && uOrientation < 7.5;
    // SplitFloor/SplitCeiling/Room/SplitLeft/SplitRight ("region" codes 3/4/6/7, not the pure
    // standalone 1/2) are drawn as a *continuation* of the standing box's own front face, not a
    // separate plane hanging off uDepth — otherwise the two pieces don't share an edge and read as
    // a duplicated layer with a gap between them. Offsetting the region's reference depth by the
    // box's own uThickness lines its far edge up with exactly where the box's front face sits.
    bool is_region = (uOrientation > 2.5 && uOrientation < 4.5) || (uOrientation > 5.5 && uOrientation < 7.5);
    // Box: z sweeps depth as before. Floor/Ceiling: depth stays fixed at uDepth; recession into
    // the distance is added separately below via the normal axis, driven by local y instead.
    // MAX, not min: row/column-run boxes are deliberately thin in ONE axis by construction (a
    // background strip merged into a 200px-wide, 1px-tall run is not "tiny" — only a box that's
    // small in BOTH dimensions, like an isolated 1px star, should have its extrusion clamped).
    // Using min() here crushed every merged run's thickness to near-zero regardless of the
    // user's box_thickness_meters setting, since almost every run is 1px thin on one axis.
    float footprint = max(uQuadW * uCanvasScale * sub_w, uQuadH * uCanvasScale * sub_h);
    // sign-preserving clamp: the Symmetric mirrored copy passes a negated uThickness to reverse
    // its extrusion direction, so clamp the magnitude only and keep the sign.
    float eff_thickness = (uAutoThickness > 0.5)
        ? sign(uThickness) * min(abs(uThickness), max(footprint, 0.001))
        : uThickness;
    // SizeThickness: scale the (already-computed) thickness by this box's own on-screen area —
    // uThickness/box_thickness_meters (Thick +/-) is the CEILING a large object approaches;
    // sqrt(area) keeps the scale roughly linear in on-screen size rather than area, and the 0.1
    // floor means even a tiny Goomba still gets a thin sliver of depth, never fully flat. Gain of
    // 5.0 means a box covering ~4% of the frame's area (e.g. 20% width x 20% height, a fairly
    // large sprite) already reaches full ceiling thickness.
    if (uSizeThicknessMode > 0.5) {
        float area_frac = sub_w * sub_h;
        float size_scale = clamp(sqrt(area_frac) * 5.0, 0.1, 1.0);
        eff_thickness *= size_scale;
    }
    // DepthScatter (uScatterRange>0, hash-based, stable per-box) and AutoYDepth (uYDepthRange>0,
    // derived from the box's own vertical source position — top of image = farther, matching the
    // Floor/Ceiling "up=far" convention above) each add a per-object depth offset on top of the
    // layer's base uDepth. Both default to 0 and are mutually exclusive in practice (only one
    // mode's draw call sets either to nonzero), so summing them unconditionally is safe and
    // avoids needing a separate mode-select uniform.
    float scatter_offset = (hash01(subrect.xy) - 0.5) * uScatterRange;
    float y_depth_offset = ((subrect.y + subrect.w) * 0.5) * uYDepthRange;
    float eff_depth = uDepth + scatter_offset + y_depth_offset;
    if (uZBufferDepths > 0.5) eff_depth = uObjectDepths[boxIndex];
    // Region codes attach to the box's own front face (see is_region comment above); pure
    // Floor/Ceiling/Box/Billboard keep using the layer's own uDepth unchanged.
    float region_offset = is_region ? uThickness : 0.0;
    float base_depth = eff_depth - region_offset;
    float raw_d = flat_mode ? base_depth : (eff_depth - aPos.z * eff_thickness);
    float d = uAllowBehind > 0.5 ? raw_d : max(0.01, raw_d);

    float cos_el = cos(uCanvasEl);
    float sin_el = sin(uCanvasEl);
    float cos_az = cos(uCanvasAz);
    float sin_az = sin(uCanvasAz);

    vec3 center = vec3(d * sin_az * cos_el + uCanvasX,
                       d * sin_el          + uQuadY + uCanvasY,
                      -d * cos_az * cos_el);
    vec3 normal = vec3(sin_az * cos_el, sin_el, -cos_az * cos_el);
    vec3 right  = vec3(cos_az,           0.0,     sin_az);
    vec3 up     = vec3(-sin_az * sin_el, cos_el, -cos_az * sin_el);

    // Table Mode: flat tabletop basis, matching kLayerVS so the two paths agree
    // on where the scene is.
    //
    // Box extrusion does NOT live in a separate vertex offset -- it is baked
    // into `d` above (raw_d = eff_depth - aPos.z * eff_thickness), which the
    // normal path turns into world position along the view ray. Overwriting
    // `center` outright therefore threw the extrusion away and collapsed every
    // box to a flat plane, which is why Table Mode had no object depth at all.
    // Re-apply it here as a vertical offset instead: (d - eff_depth) is 0 on
    // the front face and grows negative through the box, so the pixels extrude
    // DOWN toward the floor while their lit face stays up.
    //
    // uCanvasY (height) and uCanvasAz (yaw) are honoured so free roam can move
    // and turn the table, as it can with any other placement.
    bool table_mode = uTableMode > 0.5;
    if (table_mode) {
        // Table/Ceiling share everything but facing, stack direction and
        // height. kTableDistance/kTableHeight/kCeilingHeight MUST stay
        // identical across all three layer programs and the CPU-side surface
        // in openxr_shell.cpp's canvas anchor -- more than one can draw in a
        // frame, and when they disagree the scene renders twice in two places.
        const float kTableDistance = 1.15;  // metres out from the play-space origin
        const float kTableHeight   = -0.62; // Table: metres below eye level
        const float kCeilingHeight =  0.78; // Ceiling: metres above eye level
        bool ceiling_mode = uTableMode > 1.5;
        float surf_y    = ceiling_mode ? kCeilingHeight : kTableHeight;
        float stack_dir = ceiling_mode ? -1.0 : 1.0;
        float t_cos = cos(uCanvasAz);
        float t_sin = sin(uCanvasAz);
        // Box extrusion is baked into `d` (raw_d = eff_depth - aPos.z *
        // eff_thickness), so overwriting `center` outright would discard it
        // and flatten every box. Re-apply it along the surface normal, and
        // let stack_dir carry it so a ceiling extrudes up and away from a
        // viewer looking at it from below.
        float extrude = d - eff_depth;
        center = vec3( kTableDistance * t_sin + uCanvasX,
                       surf_y + stack_dir * (uQuadY + extrude) + uCanvasY,
                      -kTableDistance * t_cos);
        normal = vec3(0.0, ceiling_mode ? -1.0 : 1.0, 0.0);
        right  = vec3(t_cos, 0.0, t_sin);
        // Flipping `up` with the normal keeps the basis right-handed, so the
        // picture reads correctly when viewed from the other side instead of
        // mirrored.
        up     = ceiling_mode ? vec3(-t_sin, 0.0, t_cos) : vec3(t_sin, 0.0, -t_cos);
    }

    // Bookshelf yaw -- see kLayerVS. The box turns with its layer so extruded
    // geometry stays welded to the front face it belongs to.
    if (uLayerYaw != 0.0) {
        float cyw = cos(uLayerYaw);
        float syw = sin(uLayerYaw);
        vec3 yawed_right = right * cyw - normal * syw;
        normal = right * syw + normal * cyw;
        right  = yawed_right;
    }

    // Billboard: this box's PLACEMENT (center) still comes from the shared canvas az/el above —
    // only its FACING changes, replacing the shared basis with one derived per-instance from
    // (HMD - center) so it always points at the viewer. Skips the tilt below entirely (tilt is a
    // whole-canvas concept that doesn't apply per-box).
    if (is_billboard && !table_mode) {
        vec3 to_hmd = normalize(uHmdPos - center + vec3(0.0001));
        vec3 world_up = vec3(0.0, 1.0, 0.0);
        right  = normalize(cross(world_up, to_hmd));
        up     = cross(to_hmd, right);
        normal = to_hmd;
    }

    float ctx = cos(table_mode ? 0.0 : uTiltX);
    float stx = sin(table_mode ? 0.0 : uTiltX);
    vec3 pitched_up     = up * ctx + normal * stx;
    vec3 pitched_normal = normal * ctx - up * stx;
    float cty = cos(table_mode ? 0.0 : uTiltY);
    float sty = sin(table_mode ? 0.0 : uTiltY);
    vec3 tilted_right  = right * cty - pitched_normal * sty;
    vec3 tilted_up     = pitched_up;

    float vx = aPos.x * uQuadW * uCanvasScale * sub_w;

    float cx = aPos.x * 2.0;
    float curve_offset = uScreenCurve * uQuadW * 0.18 * cx * cx;

    vec3 world_pos;
    if (!flat_mode) {
        float vy = aPos.y * uQuadH * uCanvasScale * sub_h;
        world_pos = center + tilted_right * (center_dx + vx) + tilted_up * (center_dy + vy)
                    + normal * curve_offset;
    } else if (!is_side) {
        // Floor/Ceiling: local y sweeps a depth span along the normal axis instead of a screen
        // extent — far edge (aPos.y=+0.5) stays at base_depth (the box's own front face for the
        // region codes 3/4, or this layer's uDepth for the pure standalone 1/2 codes); near edge
        // (aPos.y=-0.5) reaches all the way past L1 by the same span, i.e. 2*(base_depth-
        // uRefL1Depth) closer, mirroring the Symmetric mode's mirrored-box math.
        // local z becomes a thin slab thickness along the up axis, below the sprite's bottom
        // edge (Floor) or above its top edge (Ceiling).
        float span = max(0.0, base_depth - uRefL1Depth);
        float depth_extra = (aPos.y - 0.5) * 2.0 * span;
        float ceiling_sign = is_ceiling ? 1.0 : -1.0;
        float edge_height = ceiling_sign * 0.5 * uQuadH * uCanvasScale * sub_h;
        float slab = ceiling_sign * aPos.z * uThickness;
        world_pos = center + tilted_right * (center_dx + vx)
                    + tilted_up * (center_dy + edge_height + slab)
                    + normal * (curve_offset + depth_extra);
    } else {
        // SplitLeft/SplitRight (6/7): mirror of the Floor/Ceiling block above with the axes
        // swapped — local x sweeps the depth span (far edge aPos.x=+0.5 at base_depth, near edge
        // aPos.x=-0.5 reaches 2*span closer), local y sweeps the full vertical extent as a normal
        // screen extent, and the slab sits at the left/right edge instead of top/bottom.
        float span = max(0.0, base_depth - uRefL1Depth);
        float depth_extra = (aPos.x - 0.5) * 2.0 * span;
        float side_sign = is_right ? 1.0 : -1.0;
        float edge_width = side_sign * 0.5 * uQuadW * uCanvasScale * sub_w;
        float slab = side_sign * aPos.z * uThickness;
        float vy = aPos.y * uQuadH * uCanvasScale * sub_h;
        world_pos = center + tilted_right * (center_dx + edge_width + slab)
                    + tilted_up * (center_dy + vy)
                    + normal * (curve_offset + depth_extra);
    }

    gl_Position = uVP * vec4(world_pos, 1.0);

    // Pure Floor/Ceiling (orientation 1/2, NOT the SplitFloor/Ceiling region codes 3/4): sample
    // only the layer's single bottom (Floor) or top (Ceiling) pixel row and tile it across every
    // face of the box, instead of stretching the whole image into a depth ramp. That single row
    // is the "floor/ceiling material" extruded all the way through the depth span.
    bool tile_edge_row = uOrientation > 0.5 && uOrientation < 2.5;
    if (tile_edge_row) {
        float edge_v = is_ceiling ? subrect.y : subrect.w;
        float u = (aFace < 0.5) ? mix(subrect.x, subrect.z, aUV.x)
                                 : mix(subrect.x, subrect.z, aUV.y);
        vUV = vec2(u, edge_v);
    } else if (aFace < 0.5) {
        // Back face: same UV rect as the front sprite (mirrored duplicate, no real data exists
        // for "behind" a 2D sprite — see the earlier design discussion on symmetric duplication).
        vUV = mix(subrect.xy, subrect.zw, aUV);
    } else {
        // Side faces: aUV.y carries edge_t (0..1 position along the edge); aUV.x is unused here.
        // Each side samples a single fixed column/row of the SAME instance's subrect — i.e. the
        // texel strip right at that edge of the sprite — so it reads as a plausible continuation
        // rather than a flat fill. Left/right hold u fixed and sweep v; top/bottom hold v fixed
        // and sweep u.
        if (aFace < 1.5) {         // left
            vUV = vec2(subrect.x, mix(subrect.y, subrect.w, aUV.y));
        } else if (aFace < 2.5) {  // right
            vUV = vec2(subrect.z, mix(subrect.y, subrect.w, aUV.y));
        } else if (aFace < 3.5) {  // top
            vUV = vec2(mix(subrect.x, subrect.z, aUV.y), subrect.y);
        } else {                   // bottom
            vUV = vec2(mix(subrect.x, subrect.z, aUV.y), subrect.w);
        }
    }
    vFace = aFace;
    vDepthT = -aPos.z; // 0 front .. 1 back
    vSubrect = subrect;
    vEdgeT = aUV.y;
    vec3 face_normal = (aFace < 0.5) ? normal
                       : (aFace < 1.5) ? -right
                       : (aFace < 2.5) ? right
                       : (aFace < 3.5) ? up
                       : -up;
    vNormal = normalize(face_normal);
}
)GLSL";

static const char* kBoxLayerFS = R"GLSL(#version 300 es
precision highp float;

uniform sampler2D uTexture;
uniform float uGamma;
uniform float uContrast;
uniform float uSaturation;
uniform float uBrightness;
uniform float uPixelLight;
uniform vec3  uLightDir;
uniform float uLightAmbient;
uniform float uLightFlicker;
uniform float uFogFactor;
uniform vec3  uFogColor;
uniform float uForceOpaqueAlpha;
// When on, side faces resample using the per-row/per-column silhouette edge profile
// (uEdgeLR/uEdgeTB) instead of the vertex-computed static edge column, so the side reads
// as the sprite's real outline at each height instead of one repeated fixed strip.
uniform float uSilhouette;
uniform sampler2D uEdgeLR; // height x 1, R=left_u, G=right_u per row (global frame V)
uniform sampler2D uEdgeTB; // width x 1,  R=top_v,  G=bottom_v per column (global frame U)
// Per-layer override for this box mesh's faces (back + 4 sides — the real front sprite is
// drawn by a separate pass, not this mesh). When > 0.5, every fragment here is flattened to
// uSideColorRGB instead of the sampled texture color, making that layer's real geometry stand
// out as a solid-color silhouette from every angle except straight-on.
uniform float uSideColorMode;
uniform vec3  uSideColorRGB;
// "Darker" side-color mode (LayerFrame::side_color_mode == 6): dims the real
// sampled texture instead of replacing it with uSideColorRGB. Kept as its
// own uniform rather than folded into uSideColorMode's flat-color path,
// since uSideColorMode only ever carries an on/off flag from the CPU side
// (the actual mode index is resolved to either uSideColorRGB or this flag
// before upload) — see GlesRenderer::render_eye()'s side_color_mode_to_rgb()
// call sites.
uniform float uSideColorDarken;

in vec2  vUV;
in float vFace;
in float vDepthT;
in vec4  vSubrect;
in float vEdgeT;
in vec3  vNormal;
out vec4 fragColor;

void main() {
    vec2 uv = vUV;
    if (uSilhouette > 0.5 && vFace > 0.5) {
        if (vFace < 2.5) {
            // left/right: vEdgeT sweeps the subrect's local v range; edge_lr is indexed by
            // the global frame row, so recover the global v first.
            float global_v = mix(vSubrect.y, vSubrect.w, vEdgeT);
            vec2 lr = texture(uEdgeLR, vec2(global_v, 0.5)).rg;
            uv = vec2(vFace < 1.5 ? lr.r : lr.g, global_v);
        } else {
            float global_u = mix(vSubrect.x, vSubrect.z, vEdgeT);
            vec2 tb = texture(uEdgeTB, vec2(global_u, 0.5)).rg;
            uv = vec2(global_u, vFace < 3.5 ? tb.r : tb.g);
        }
    }
    vec4 color = texture(uTexture, uv);
    if (color.a < 0.01) discard;

    if (uSideColorDarken > 0.5) {
        // "Darker" side-color mode: keeps the real sampled sprite texture
        // instead of replacing it with a flat color like uSideColorMode
        // below, dimmed to exactly half its original brightness — mixing a
        // color with black at 50% is mathematically the same as multiplying
        // it by 0.5, so this is a genuine halfway point between the original
        // texture and pure black, not an arbitrary darkening amount.
        color.rgb *= 0.5;
        color.rgb *= mix(1.0, 0.72, vDepthT);
        color.rgb = clamp(color.rgb, 0.0, 1.0);
        color.rgb = mix(color.rgb, uFogColor, uFogFactor);
        if (uForceOpaqueAlpha > 0.5) color.a = 1.0;
        fragColor = color;
        return;
    }
    if (uSideColorMode > 0.5) {
        color.rgb = uSideColorRGB;
        color.rgb *= mix(1.0, 0.72, vDepthT);
        color.rgb = clamp(color.rgb, 0.0, 1.0);
        color.rgb = mix(color.rgb, uFogColor, uFogFactor);
        if (uForceOpaqueAlpha > 0.5) color.a = 1.0;
        fragColor = color;
        return;
    }

    color.rgb = pow(max(color.rgb, vec3(0.001)), vec3(uGamma));
    float luma2 = dot(color.rgb, vec3(0.299, 0.587, 0.114));
    color.rgb = mix(vec3(luma2), color.rgb, uSaturation);
    color.rgb = (color.rgb - 0.5) * uContrast + 0.5;
    color.rgb *= uBrightness;
    // Shade the sides slightly darker toward the back edge — a cheap depth cue that also
    // hides the seam where the edge-sampled texture repeats along the depth axis.
    color.rgb *= mix(1.0, 0.72, vDepthT);
    color.rgb = clamp(color.rgb, 0.0, 1.0);
    if (uPixelLight > 0.5) {
        float diffuse = max(dot(normalize(vNormal), normalize(uLightDir)), 0.0);
        color.rgb *= mix(uLightAmbient, 1.0, diffuse * uLightFlicker);
    }
    color.rgb = mix(color.rgb, uFogColor, uFogFactor);

    if (uForceOpaqueAlpha > 0.5) color.a = 1.0;
    fragColor = color;
}
)GLSL";

static const char* kLayerFS = R"GLSL(#version 300 es
precision highp float;

uniform sampler2D uTexture;
uniform float uDepth;
uniform float uUpscale;
uniform float uDepthMap;
uniform float uGamma;
uniform float uContrast;
uniform float uSaturation;
uniform float uBrightness;
uniform float uPixelLight;
uniform vec3  uLightDir;
uniform float uLightAmbient;
uniform float uLightFlicker;
uniform float uFogFactor;
uniform vec3  uFogColor;
uniform float uCopyCount;
uniform float uSolidStack;  // 1.0 = fill transparent copy pixels with dark extrusion colour
uniform float uForceOpaqueAlpha; // 1.0 = visible game pixels write full compositor alpha
uniform float uBboxMode; // 1.0 = apply bbox-centered width shrink on copy instances
uniform float uBboxDebug; // 1.0 = tint bbox copy instances per detected object
uniform int uObjectBoxCount;
// Solid-stack fill: instead of a flat extrusion shade, resample the sprite's own edge
// colour at this row/column (same edge profile the box/silhouette shader uses) so the
// filled interior reads as a continuation of the sprite rather than a dark slab behind it.
uniform float uHasEdgeProfile;
uniform sampler2D uEdgeLR; // height x 1, R=left_u, G=right_u per row (global frame V)
uniform sampler2D uEdgeTB; // width x 1,  R=top_v,  G=bottom_v per column (global frame U)

in vec2  vUV;
in float vCopyT;
in vec3  vNormal;
out vec4 fragColor;

// FSR1 RCAS (contrast-adaptive sharpen), simplified for LDR pixel-art content —
// not a bit-exact port of AMD's HLSL reference, but faithful to the algorithm's
// core idea: sharpen less where the local neighbourhood is already high-contrast,
// to avoid ringing/haloing on hard edges. Operates on premultiplied RGB, same as
// the pixel-art path above, so transparent cutout edges stay fringe-free.
vec4 sampleFsrRcas(vec2 uv) {
    vec2 tsz   = vec2(textureSize(uTexture, 0));
    vec2 texel = 1.0 / tsz;

    vec4 e  = texture(uTexture, uv);
    vec4 n  = texture(uTexture, uv + vec2(0.0, texel.y));
    vec4 s  = texture(uTexture, uv - vec2(0.0, texel.y));
    vec4 w  = texture(uTexture, uv - vec2(texel.x, 0.0));
    vec4 ee = texture(uTexture, uv + vec2(texel.x, 0.0));

    vec3 cE = e.rgb  * e.a;
    vec3 cN = n.rgb  * n.a;
    vec3 cS = s.rgb  * s.a;
    vec3 cW = w.rgb  * w.a;
    vec3 cX = ee.rgb * ee.a;

    vec3 mn4 = min(min(cN, cS), min(cW, cX));
    vec3 mx4 = max(max(cN, cS), max(cW, cX));
    vec3 mn5 = min(mn4, cE);
    vec3 mx5 = max(mx4, cE);

    vec3 rcpMRange = 1.0 / max(mx4 - mn4, vec3(1e-4));
    vec3 ampl = clamp(min(mn5, vec3(1.0) - mx5) * rcpMRange, vec3(0.0), vec3(1.0));
    ampl = sqrt(ampl);

    // -0.125 is a scaled-down version of AMD's peak sharpen weight, tuned
    // subtler for pixel-art-scale content rather than photographic FSR use.
    vec3 peak = ampl * (-0.125);
    vec3 sharpened = (cN * peak + cS * peak + cW * peak + cX * peak + cE) /
                      (1.0 + 4.0 * peak);

    if (e.a > 0.001) {
        return vec4(clamp(sharpened / e.a, 0.0, 1.0), e.a);
    }
    return e;
}

vec4 sampleLayer(vec2 uv) {
    if (uUpscale > 1.5) {
        return sampleFsrRcas(uv);
    } else if (uUpscale > 0.5) {
        // Pixel-aware reconstruction followed by a small alpha-safe unsharp pass.
        // The reconstruction keeps most of each source pixel flat and only blends
        // at its boundary; the sharpen pass restores edge contrast lost to that
        // blend.  Filtering premultiplied RGB avoids dark/bright fringes around
        // transparent cutout pixels.
        vec2 tsz = vec2(textureSize(uTexture, 0));
        vec2 p   = uv * tsz;           // position in source-pixel space
        vec2 fr  = fract(p);           // fractional part within each source pixel
        // Derivative of p with respect to screen pixels — gives output/source ratio.
        vec2 dpdx = dFdx(p);
        vec2 dpdy = dFdy(p);
        vec2 scale = vec2(length(dpdx), length(dpdy));  // output pixels per source pixel
        // Clamp scale to avoid div-by-zero at scale < 1 (downscale path).
        scale = max(scale, vec2(1.0));
        // Blend window: 1 output pixel wide, expressed in source-pixel fractions.
        vec2 w = 0.5 / scale;
        // Smoothstep within the edge window; flat (=snap to center) everywhere else.
        vec2 sharp = smoothstep(0.5 - w, 0.5 + w, fr);
        // Sample from texel centres.  The previous coordinate landed on texel
        // boundaries, which made GL_LINEAR blend four source pixels everywhere.
        vec2 suv   = (floor(p) + vec2(0.5) + sharp) / tsz;

        vec4 base = texture(uTexture, suv);
        vec2 texel = 1.0 / tsz;
        vec4 left  = texture(uTexture, suv - vec2(texel.x, 0.0));
        vec4 right = texture(uTexture, suv + vec2(texel.x, 0.0));
        vec4 down  = texture(uTexture, suv - vec2(0.0, texel.y));
        vec4 up    = texture(uTexture, suv + vec2(0.0, texel.y));

        vec3 blurPremul = base.rgb * base.a * 4.0
                        + left.rgb * left.a
                        + right.rgb * right.a
                        + down.rgb * down.a
                        + up.rgb * up.a;
        float blurAlpha = base.a * 4.0 + left.a + right.a + down.a + up.a;
        blurPremul /= 8.0;
        blurAlpha  /= 8.0;

        // Disable sharpening across the transparent silhouette boundary. This
        // keeps the cutout edge clean while retaining crisp opaque interiors.
        float edgeConfidence = smoothstep(0.35, 0.95, min(base.a, blurAlpha));
        // Keep the source character dominant; this is detail restoration, not a
        // high-contrast filter that can make contours look synthetic.
        float strength = 0.24 * edgeConfidence;
        vec3 basePremul = base.rgb * base.a;
        vec3 sharpenedPremul = basePremul + (basePremul - blurPremul) * strength;
        if (base.a > 0.001) {
            return vec4(clamp(sharpenedPremul / base.a, 0.0, 1.0), base.a);
        }
        return base;
    } else {
        return texture(uTexture, uv);
    }
}


vec3 bbox_debug_color(int idx) {
    float h = fract(float(idx) * 0.61803398875);
    float r = abs(h * 6.0 - 3.0) - 1.0;
    float g = 2.0 - abs(h * 6.0 - 2.0);
    float b = 2.0 - abs(h * 6.0 - 4.0);
    return clamp(vec3(r, g, b), 0.0, 1.0);
}

void main() {
    vec4 color = sampleLayer(vUV);

    // BBox mode: subrect already constrains UV to the object region — no clip/resample needed.
    // Depth and sizing are handled entirely in the vertex shader.
    if (uBboxMode > 0.5 && vCopyT > 0.0 && uBboxDebug > 0.5 && color.a >= 0.01) {
        color.rgb = mix(color.rgb, bbox_debug_color(0), 0.65);
    }

    if (color.a < 0.01) {
        // Copy instances with solid_stack: fill the transparent silhouette with a dark
        // extrusion colour so the stack looks like a thick block instead of floating cards.
        if (uSolidStack > 0.5f && vCopyT > 0.0) {
            if (uHasEdgeProfile > 0.5) {
                vec2 lr = texture(uEdgeLR, vec2(vUV.y, 0.5)).rg; // left_u, right_u at this row
                vec2 tb = texture(uEdgeTB, vec2(vUV.x, 0.5)).rg; // top_v, bottom_v at this column
                float dx = (vUV.x < lr.r) ? (lr.r - vUV.x) : (vUV.x > lr.g ? (vUV.x - lr.g) : 0.0);
                float dy = (vUV.y < tb.r) ? (tb.r - vUV.y) : (vUV.y > tb.g ? (vUV.y - tb.g) : 0.0);
                vec2 fillUv = (dx <= dy) ? vec2(clamp(vUV.x, lr.r, lr.g), vUV.y)
                                         : vec2(vUV.x, clamp(vUV.y, tb.r, tb.g));
                vec4 edgeColor = texture(uTexture, fillUv);
                float shade = mix(1.0, 0.55, vCopyT); // darken further back, same as box sides
                fragColor = vec4(edgeColor.rgb * shade, 1.0);
                return;
            }
            float shade = 0.04 + vCopyT * 0.04; // slightly lighter further back
            fragColor = vec4(shade, shade, shade * 1.2, 1.0);
            return;
        }
        discard;
    }

    if (uDepth > 0.0) {
        color.rgb = pow(max(color.rgb, vec3(0.001)), vec3(uGamma));
        float luma2 = dot(color.rgb, vec3(0.299, 0.587, 0.114));
        color.rgb = mix(vec3(luma2), color.rgb, uSaturation);
        color.rgb = (color.rgb - 0.5) * uContrast + 0.5;
        color.rgb *= uBrightness;
        color.rgb = clamp(color.rgb, 0.0, 1.0);
    }
    if (uPixelLight > 0.5) {
        float diffuse = max(dot(normalize(-vNormal), normalize(uLightDir)), 0.0);
        color.rgb *= mix(uLightAmbient, 1.0, diffuse * uLightFlicker);
    }
    color.rgb = mix(color.rgb, uFogColor, uFogFactor);
    if (uForceOpaqueAlpha > 0.5) {
        color.a = 1.0;
    }
    fragColor = color;
}
)GLSL";

static const char* kFlatVS = R"GLSL(#version 300 es
layout(location = 0) in vec3  aPos;
layout(location = 1) in float aAlpha;
uniform mat4 uVP;
out float vAlpha;
void main() { gl_Position = uVP * vec4(aPos, 1.0); vAlpha = aAlpha; }
)GLSL";

static const char* kFlatFS = R"GLSL(#version 300 es
precision highp float;
uniform vec4  uColor;
in float vAlpha;
out vec4 fragColor;
void main() { fragColor = vec4(uColor.rgb, uColor.a * vAlpha); }
)GLSL";

// Lit flat-shaded program for the lightgun model: per-vertex normal, single
// fixed directional light + ambient, so faceted low-poly shapes read as
// actual volume instead of a flat silhouette.
static const char* kGunVS = R"GLSL(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aVertexColor;
layout(location = 3) in float aPart;
uniform mat4 uVP;
uniform mat4 uModel;
uniform float uTrigger;
uniform float uRecoil;
uniform float uTilt;
out vec3 vNormal;
out vec3 vVertexColor;
void main() {
    vec3 local_pos = aPos;
    if (aPart > 0.5 && aPart < 1.5) {
        // Pull the trigger slightly toward the grip and down.
        local_pos += vec3(0.0, -0.0025 * uTrigger, 0.0035 * uTrigger);
    } else if (aPart > 1.5) {
        // The source's native -Z is barrel-forward, so +Z is rearward recoil.
        // Exaggerated past a real slide's travel so the cycling barrel is
        // clearly readable in VR at arm's length.
        local_pos.z += 0.034 * uRecoil;
    }
    // Rotate the downloaded pistol around the grip when the revolver envelope
    // is active. Positive X rotation raises the barrel (+Y) before settling.
    float tilt_cos = cos(uTilt);
    float tilt_sin = sin(uTilt);
    vec3 tilt_pivot = vec3(0.0, -0.045, 0.020);
    vec3 tilt_rel = local_pos - tilt_pivot;
    local_pos = tilt_pivot + vec3(
        tilt_rel.x,
        tilt_cos * tilt_rel.y - tilt_sin * tilt_rel.z,
        tilt_sin * tilt_rel.y + tilt_cos * tilt_rel.z);
    vec3 local_normal = vec3(
        aNormal.x,
        tilt_cos * aNormal.y - tilt_sin * aNormal.z,
        tilt_sin * aNormal.y + tilt_cos * aNormal.z);
    gl_Position = uVP * uModel * vec4(local_pos, 1.0);
    vNormal = mat3(uModel) * local_normal;
    vVertexColor = aVertexColor;
}
)GLSL";

static const char* kGunFS = R"GLSL(#version 300 es
precision highp float;
uniform vec4  uColor;
uniform float uVertexColor;
uniform vec3  uLightDir;
uniform float uLightAmbient;
in vec3 vNormal;
in vec3 vVertexColor;
out vec4 fragColor;
void main() {
    float diffuse = max(dot(normalize(vNormal), normalize(uLightDir)), 0.0);
    float shade = clamp(uLightAmbient + (1.0 - uLightAmbient) * diffuse, 0.0, 1.0);
    vec3 base_color = mix(uColor.rgb, vVertexColor, uVertexColor);
    fragColor = vec4(base_color * shade, uColor.a);
}
)GLSL";

// Real controller model (XR_FB_render_model, parsed via cgltf): a plain
// lit pos+normal mesh, one draw call per glTF node -- button animation is
// applied on the CPU as a per-node world-matrix override (see
// GlesRenderer::draw_controller_model()), not in this shader, so it stays
// as simple as the rest of the small utility shaders in this file.
static const char* kControllerVS = R"GLSL(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
uniform mat4 uVP;
uniform mat4 uModel;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
void main() {
    gl_Position = uVP * uModel * vec4(aPos, 1.0);
    vNormal = mat3(uModel) * aNormal;
    vUV = aUV;
    vColor = aColor;
}
)GLSL";

// uHasTexture picks between the flat uColor tint (markers, and any primitive
// with no base-color texture) and the real controller texture -- e.g. the
// black top-plate / logo / grip pattern on the body mesh live in this
// texture, not as separate geometry or a flat material factor. vColor
// (glTF COLOR_0, when present) is a second, independent source of the same
// kind of per-vertex painted detail -- some render models carry it instead
// of a texture -- so it's always multiplied in; it defaults to (1,1,1,1)
// via glVertexAttrib4f when a mesh has no COLOR_0 data (see
// load_controller_model()/draw_controller_model()), making it a no-op there.
static const char* kControllerFS = R"GLSL(#version 300 es
precision highp float;
uniform vec4  uColor;
uniform vec3  uLightDir;
uniform float uLightAmbient;
uniform sampler2D uTex;
uniform float uHasTexture;
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
out vec4 fragColor;
void main() {
    float diffuse = max(dot(normalize(vNormal), normalize(uLightDir)), 0.0);
    float shade = clamp(uLightAmbient + (1.0 - uLightAmbient) * diffuse, 0.0, 1.0);
    vec4 texel = texture(uTex, vUV);
    vec3 base = mix(uColor.rgb, uColor.rgb * texel.rgb, uHasTexture) * vColor.rgb;
    fragColor = vec4(base * shade, uColor.a);
}
)GLSL";

// Scope zoom composite: attributeless fullscreen triangle (no VBO — derives
// clip-space position from gl_VertexID), samples a copy of the just-rendered
// eye image and re-samples a zoomed-in crop inside a screen-space circle
// centered on the scope's projected front lens.
static const char* kScopeZoomVS = R"GLSL(#version 300 es
out vec2 vUV;
void main() {
    vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

static const char* kScopeZoomFS = R"GLSL(#version 300 es
precision highp float;
uniform sampler2D uTex;
uniform vec2  uCenter;
uniform float uRadius;
uniform float uZoomInv;
uniform float uAspect;
in vec2 vUV;
out vec4 fragColor;
void main() {
    vec2 d = vUV - uCenter;
    d.x *= uAspect;
    float dist = length(d);
    vec2 uv = vUV;
    if (dist < uRadius) {
        vec2 zoomed = uCenter + (vUV - uCenter) * uZoomInv;
        float edge = smoothstep(uRadius * 0.90, uRadius, dist);
        uv = mix(zoomed, vUV, edge);
    }
    fragColor = texture(uTex, uv);
}
)GLSL";

// World-space textured quad (ROM browser panel)
static const char* kUiVS = R"GLSL(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
uniform mat4 uVP;
uniform mat4 uModel;
out vec2 vUV;
void main() {
    gl_Position = uVP * uModel * vec4(aPos, 1.0);
    vUV = aUV;
}
)GLSL";

static const char* kUiFS = R"GLSL(#version 300 es
precision highp float;
uniform sampler2D uTexture;
uniform float     uAlpha;
uniform int       uShadowMode;
uniform vec4      uShadowColor;
in  vec2 vUV;
out vec4 fragColor;
void main() {
    vec4 c = texture(uTexture, vUV);
    if (uShadowMode != 0) {
        float a = 0.0;
        for (int i = 0; i < 8; ++i) {
            float y = (float(i) + 0.5) / 8.0;
            a = max(a, texture(uTexture, vec2(vUV.x, y)).a);
        }
        fragColor = vec4(uShadowColor.rgb, uShadowColor.a * a * uAlpha);
        return;
    }
    fragColor = vec4(c.rgb, c.a * uAlpha);
}
)GLSL";

static const char* kSkyVS = R"GLSL(#version 300 es
layout(location = 0) in vec3 aPos;
uniform mat4 uProj;
uniform mat4 uViewRot;
out float vSphereY;
void main() {
    gl_Position = uProj * uViewRot * vec4(aPos, 1.0);
    vSphereY = clamp(aPos.y / 18.0, -1.0, 1.0);
}
)GLSL";

// PSX depth-displaced screen. The colour image is SwanStation's own composited
// frame, so PSX rasterisation stays exactly correct; all this does is push each
// vertex of a dense screen mesh back along the canvas normal by the per-pixel
// depth the backend rasterised from PGXP. Placement matches kLayerVS's
// spherical-arc canvas so the PSX screen lands where every other system's does.
static const char* kPsxScreenVS = R"GLSL(#version 310 es
layout(location = 0) in vec2 aGrid;   // 0..1 across the screen quad
uniform mat4  uVP;
uniform sampler2D uDepthTex;
uniform float uDepth;        // screen distance from viewer (metres)
uniform float uQuadW;
uniform float uQuadH;
uniform float uCanvasX;
uniform float uCanvasY;
uniform float uCanvasAz;
uniform float uCanvasEl;
uniform float uCanvasScale;
uniform float uDepthScale;   // depth range behind the screen plane (metres)
uniform int   uHasDepth;
// 1 when sampling the emulator's own colour texture. The core renders bottom-up
// (it asks for bottom_left_origin); the readback path undoes that on the CPU
// while copying rows, so only the zero-copy path still needs the flip. Applies
// to colour alone — the depth target is written already-flipped by the resolve
// pass, so it is top-down in both paths.
uniform int   uFlipV;
// Fraction of the colour texture the scanned-out image occupies (1,1 for the
// CPU path, which uploads a texture that is exactly the image).
uniform vec2  uColorUvScale;
uniform vec2  uColorUvOffset;
// One mesh cell in grid space, so the vertex can measure the depth step to its
// neighbours and flag the stretched "skin" that spans a silhouette.
uniform vec2  uGridStep;
// Depth value that sits on the screen plane. 0 extrudes everything away from
// the viewer, 1 everything toward, 0.5 splits it both ways around the plane.
uniform float uDepthPivot;
out vec2 vUV;
out float vSteep;
void main() {
    float d = max(0.01, uDepth);
    float cos_el = cos(uCanvasEl);
    float sin_el = sin(uCanvasEl);
    float cos_az = cos(uCanvasAz);
    float sin_az = sin(uCanvasAz);

    vec3 center = vec3(d * sin_az * cos_el + uCanvasX,
                       d * sin_el          + uCanvasY,
                      -d * cos_az * cos_el);
    // Outward normal: viewer -> canvas centre, so displacing along it pushes
    // geometry away from the viewer, never out into the room.
    vec3 normal = vec3(sin_az * cos_el, sin_el, -cos_az * cos_el);
    vec3 right  = vec3(cos_az,           0.0,     sin_az);
    vec3 up     = vec3(-sin_az * sin_el, cos_el, -cos_az * sin_el);

    // Both the emulator's colour target and the resolved depth are bottom-up:
    // the resolve writes py = rect.y + (1 - vUV.y) * rect.h, so its row 0 is
    // the image's bottom row, same as the core's own output. The CPU path
    // flips depth while copying rows, so it samples straight.
    vec2 duv = vec2(aGrid.x, (uFlipV != 0) ? 1.0 - aGrid.y : aGrid.y);
    float far_ness = (uHasDepth != 0) ? texture(uDepthTex, duv).r : 0.0;

    // Depth step to the neighbouring cells. A face spanning a silhouette has a
    // large step, and its texture is stretched across the discontinuity — it
    // shows background colour running down the side of a foreground object.
    // Flagging it here lets the fragment stage shade it as a side wall.
    vSteep = 0.0;
    if (uHasDepth != 0) {
        float dx = texture(uDepthTex, duv + vec2(uGridStep.x, 0.0)).r;
        float dy = texture(uDepthTex, duv + vec2(0.0, uGridStep.y)).r;
        vSteep = max(abs(far_ness - dx), abs(far_ness - dy));
    }
    float vx = (aGrid.x - 0.5) * uQuadW * uCanvasScale;
    float vy = (0.5 - aGrid.y) * uQuadH * uCanvasScale;

    // Displace either side of the pivot rather than in one direction from the
    // screen plane, so the scene can straddle it. The resolve writes the pivot
    // value for pixels it could not resolve, so those land at zero
    // displacement — on the screen plane — whatever the pivot is.
    float disp = far_ness - uDepthPivot;
    vec3 p = center + right * vx + up * vy + normal * (disp * uDepthScale);
    gl_Position = uVP * vec4(p, 1.0);
    // Top of the quad is the top of the image: bottom-up textures put that at
    // the far edge of the occupied region, not at v = 1.
    float cv = (uFlipV != 0) ? 1.0 - aGrid.y : aGrid.y;
    vUV = uColorUvOffset + vec2(aGrid.x, cv) * uColorUvScale;
}
)GLSL";

static const char* kPsxScreenFS = R"GLSL(#version 310 es
precision mediump float;
uniform sampler2D uTexture;
// How hard to treat the stretched faces at depth discontinuities, and how.
// uEdgeMode: 0 = leave them alone, 1 = shade them, 2 = drop them entirely.
uniform float uEdgeDarken;
uniform int   uEdgeMode;
in vec2 vUV;
in float vSteep;
out vec4 fragColor;
void main() {
    // The faces bridging a silhouette carry colour from the wrong side of the
    // edge — background smeared down the side of a foreground object.
    float steep = vSteep * uEdgeDarken;
    // Hide: discard the bridging faces so the silhouette is a clean edge.
    // Nothing is drawn there, so the scene behind the screen shows through the
    // gap; that is the cost of not inventing colour for surfaces the emulator
    // never rendered.
    if (uEdgeMode == 2 && steep >= 1.0) discard;

    // Raw emulator output: no QRD colour grading on the PSX screen.
    vec3 rgb = texture(uTexture, vUV).rgb;
    // Shade: turn the smear into a side wall, the way the layered renderer
    // shades the sides of an extruded layer.
    if (uEdgeMode == 1) rgb *= 1.0 - clamp(steep, 0.0, 0.9);
    fragColor = vec4(rgb, 1.0);
}
)GLSL";


static const char* kSkyFS = R"GLSL(#version 300 es
precision highp float;
uniform vec4 uBands[12];
uniform int  uMode;
in float vSphereY;
out vec4 fragColor;

vec4 sampleBands(float t) {
    float band_pos = clamp(t, 0.0, 1.0) * 11.0;
    int idx0 = int(floor(band_pos));
    int idx1 = min(idx0 + 1, 11);
    float frac_t = fract(band_pos);
    return mix(uBands[idx0], uBands[idx1], frac_t);
}

// Band 0 is always the topmost sampled image row, band 11 the bottommost. Mapping is linear
// in ELEVATION ANGLE (not sin(elevation)) so image rows sweep the sphere evenly instead of
// bunching toward the poles. Each mode spans a different elevation range:
//   Sky    (1): image top half   over the upper hemisphere  (+90°..0°)
//   Full   (2): whole image      pole to pole                (+90°..-90°)
//   Ground (3): image bottom half over the lower hemisphere  (0°..-90°)
void main() {
    const float kPi = 3.14159265;
    const float kHalfPi = 1.57079633;
    float elev = asin(clamp(vSphereY, -1.0, 1.0)); // -PI/2 (nadir) .. +PI/2 (zenith)

    float t;
    float alpha_mask;
    if (uMode == 1) {          // Sky: upper hemisphere only
        if (elev < 0.0) discard;
        t = 1.0 - elev / kHalfPi;
        alpha_mask = 0.85 * smoothstep(0.0, 0.22, elev); // visible sky ambient
    } else if (uMode == 3) {   // Ground: lower hemisphere only
        if (elev > 0.0) discard;
        t = -elev / kHalfPi;
        alpha_mask = 0.85 * smoothstep(0.0, 0.22, -elev);
    } else if (uMode == 4) {   // BgColor: user-chosen preset, fully opaque pole to pole —
                                // this REPLACES the background (not a subtle overlay like Full).
        t = 0.5 - elev / kPi;
        alpha_mask = 1.0;
    } else {                   // Full: continuous pole to pole
        t = 0.5 - elev / kPi;
        // Fixed 0.5 — Full mode is a solid surround (bands are opaque), so this is a flat
        // intensity cap rather than a real settings knob, to keep it from overpowering the
        // scene. Not exposed in the UI to avoid growing the settings list for a tweak like
        // this; adjust here directly if it needs retuning after testing in headset.
        alpha_mask = 0.85;
    }

    vec4 env = sampleBands(t);
    fragColor = vec4(env.rgb, env.a * alpha_mask);
}
)GLSL";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static GLuint compile_shader(GLenum type, const char* src, std::string& err) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return s;
    GLint len = 0;
    glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
    err.resize(std::max(1, len));
    glGetShaderInfoLog(s, len, nullptr, err.data());
    glDeleteShader(s);
    return 0;
}

static GLuint link_program(GLuint vs, GLuint fs, std::string& err) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (ok == GL_TRUE) return p;
    GLint len = 0;
    glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
    err.resize(std::max(1, len));
    glGetProgramInfoLog(p, len, nullptr, err.data());
    glDeleteProgram(p);
    return 0;
}

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------

bool GlesRenderer::init_layer_program(std::string& err) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   kLayerVS, err); if (!vs) return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kLayerFS, err);
    if (!fs) { glDeleteShader(vs); return false; }
    m_program = link_program(vs, fs, err);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!m_program) return false;

    m_u_vp           = glGetUniformLocation(m_program, "uVP");
    m_u_depth        = glGetUniformLocation(m_program, "uDepth");
    m_u_quad_w       = glGetUniformLocation(m_program, "uQuadW");
    m_u_quad_h       = glGetUniformLocation(m_program, "uQuadH");
    m_u_quad_y       = glGetUniformLocation(m_program, "uQuadY");
    m_u_roundness    = glGetUniformLocation(m_program, "uRoundness");
    m_u_copy_count   = glGetUniformLocation(m_program, "uCopyCount");
    m_u_copy_span    = glGetUniformLocation(m_program, "uCopySpan");
    m_u_screen_curve = glGetUniformLocation(m_program, "uScreenCurve");
    m_u_upscale      = glGetUniformLocation(m_program, "uUpscale");
    m_u_depthmap     = glGetUniformLocation(m_program, "uDepthMap");
    m_u_gamma        = glGetUniformLocation(m_program, "uGamma");
    m_u_contrast     = glGetUniformLocation(m_program, "uContrast");
    m_u_saturation   = glGetUniformLocation(m_program, "uSaturation");
    m_u_brightness   = glGetUniformLocation(m_program, "uBrightness");
    m_u_pixel_light  = glGetUniformLocation(m_program, "uPixelLight");
    m_u_light_dir    = glGetUniformLocation(m_program, "uLightDir");
    m_u_light_ambient = glGetUniformLocation(m_program, "uLightAmbient");
    m_u_light_flicker = glGetUniformLocation(m_program, "uLightFlicker");
    m_u_fog_factor    = glGetUniformLocation(m_program, "uFogFactor");
    m_u_fog_color     = glGetUniformLocation(m_program, "uFogColor");
    m_u_texture      = glGetUniformLocation(m_program, "uTexture");
    m_u_canvas_x     = glGetUniformLocation(m_program, "uCanvasX");
    m_u_canvas_y     = glGetUniformLocation(m_program, "uCanvasY");
    m_u_canvas_az    = glGetUniformLocation(m_program, "uCanvasAz");
    m_u_layer_yaw    = glGetUniformLocation(m_program, "uLayerYaw");
    m_u_canvas_el    = glGetUniformLocation(m_program, "uCanvasEl");
    m_u_canvas_scale = glGetUniformLocation(m_program, "uCanvasScale");
    m_u_solid_stack  = glGetUniformLocation(m_program, "uSolidStack");
    m_u_force_opaque_alpha = glGetUniformLocation(m_program, "uForceOpaqueAlpha");
    m_u_bbox_mode    = glGetUniformLocation(m_program, "uBboxMode");
    m_u_zbuffer_depths = glGetUniformLocation(m_program, "uZBufferDepths");
    m_u_bbox_debug   = glGetUniformLocation(m_program, "uBboxDebug");
    m_u_subrect_enable = glGetUniformLocation(m_program, "uSubrectEnable");
    m_u_subrect      = glGetUniformLocation(m_program, "uSubrect");
    m_u_instance_base = glGetUniformLocation(m_program, "uInstanceBase");
    m_u_object_box_count = glGetUniformLocation(m_program, "uObjectBoxCount");
    m_u_allow_behind = glGetUniformLocation(m_program, "uAllowBehind");
    m_u_has_y_depth    = glGetUniformLocation(m_program, "uHasYDepth");
    m_u_y_depth_tex    = glGetUniformLocation(m_program, "uYDepthTex");
    m_u_y_depth_spread = glGetUniformLocation(m_program, "uYDepthSpread");
    m_u_edge_lr          = glGetUniformLocation(m_program, "uEdgeLR");
    m_u_edge_tb          = glGetUniformLocation(m_program, "uEdgeTB");
    m_u_has_edge_profile = glGetUniformLocation(m_program, "uHasEdgeProfile");
    m_u_rotate90         = glGetUniformLocation(m_program, "uRotateMode");
    m_u_table_mode       = glGetUniformLocation(m_program, "uTableMode");
    // Bind the Y-depth sampler permanently to texture unit 1
    if (m_u_y_depth_tex >= 0) {
        glUseProgram(m_program);
        glUniform1i(m_u_y_depth_tex, 1);
        glUseProgram(0);
    }
    return true;
}

bool GlesRenderer::init_immersive_layer_program(std::string& err) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   kImmersiveLayerVS, err); if (!vs) return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kLayerFS, err);
    if (!fs) { glDeleteShader(vs); return false; }
    m_immersive_program = link_program(vs, fs, err);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!m_immersive_program) return false;

    m_i_u_vp           = glGetUniformLocation(m_immersive_program, "uVP");
    m_i_u_depth        = glGetUniformLocation(m_immersive_program, "uDepth");
    m_i_u_quad_w       = glGetUniformLocation(m_immersive_program, "uQuadW");
    m_i_u_quad_h       = glGetUniformLocation(m_immersive_program, "uQuadH");
    m_i_u_quad_y       = glGetUniformLocation(m_immersive_program, "uQuadY");
    m_i_u_roundness    = glGetUniformLocation(m_immersive_program, "uRoundness");
    m_i_u_copy_count   = glGetUniformLocation(m_immersive_program, "uCopyCount");
    m_i_u_copy_span    = glGetUniformLocation(m_immersive_program, "uCopySpan");
    m_i_u_screen_curve = glGetUniformLocation(m_immersive_program, "uScreenCurve");
    m_i_u_tilt_x       = glGetUniformLocation(m_immersive_program, "uTiltX");
    m_i_u_tilt_y       = glGetUniformLocation(m_immersive_program, "uTiltY");
    m_i_u_upscale      = glGetUniformLocation(m_immersive_program, "uUpscale");
    m_i_u_depthmap     = glGetUniformLocation(m_immersive_program, "uDepthMap");
    m_i_u_gamma        = glGetUniformLocation(m_immersive_program, "uGamma");
    m_i_u_contrast     = glGetUniformLocation(m_immersive_program, "uContrast");
    m_i_u_saturation   = glGetUniformLocation(m_immersive_program, "uSaturation");
    m_i_u_brightness   = glGetUniformLocation(m_immersive_program, "uBrightness");
    m_i_u_pixel_light  = glGetUniformLocation(m_immersive_program, "uPixelLight");
    m_i_u_light_dir    = glGetUniformLocation(m_immersive_program, "uLightDir");
    m_i_u_light_ambient = glGetUniformLocation(m_immersive_program, "uLightAmbient");
    m_i_u_light_flicker = glGetUniformLocation(m_immersive_program, "uLightFlicker");
    m_i_u_fog_factor    = glGetUniformLocation(m_immersive_program, "uFogFactor");
    m_i_u_fog_color     = glGetUniformLocation(m_immersive_program, "uFogColor");
    m_i_u_texture      = glGetUniformLocation(m_immersive_program, "uTexture");
    m_i_u_canvas_x     = glGetUniformLocation(m_immersive_program, "uCanvasX");
    m_i_u_canvas_y     = glGetUniformLocation(m_immersive_program, "uCanvasY");
    m_i_u_canvas_az    = glGetUniformLocation(m_immersive_program, "uCanvasAz");
    m_i_u_layer_yaw    = glGetUniformLocation(m_immersive_program, "uLayerYaw");
    m_i_u_canvas_el    = glGetUniformLocation(m_immersive_program, "uCanvasEl");
    m_i_u_canvas_scale = glGetUniformLocation(m_immersive_program, "uCanvasScale");
    m_i_u_has_y_depth = glGetUniformLocation(m_immersive_program, "uHasYDepth");
    m_i_u_y_depth_tex = glGetUniformLocation(m_immersive_program, "uYDepthTex");
    m_i_u_y_depth_spread = glGetUniformLocation(m_immersive_program, "uYDepthSpread");
    if (m_i_u_y_depth_tex >= 0) {
        glUseProgram(m_immersive_program);
        glUniform1i(m_i_u_y_depth_tex, 1);
    }
    m_i_u_solid_stack  = glGetUniformLocation(m_immersive_program, "uSolidStack");
    m_i_u_force_opaque_alpha = glGetUniformLocation(m_immersive_program, "uForceOpaqueAlpha");
    m_i_u_bbox_mode    = glGetUniformLocation(m_immersive_program, "uBboxMode");
    m_i_u_zbuffer_depths = glGetUniformLocation(m_immersive_program, "uZBufferDepths");
    m_i_u_bbox_debug   = glGetUniformLocation(m_immersive_program, "uBboxDebug");
    m_i_u_subrect_enable = glGetUniformLocation(m_immersive_program, "uSubrectEnable");
    m_i_u_subrect      = glGetUniformLocation(m_immersive_program, "uSubrect");
    m_i_u_instance_base = glGetUniformLocation(m_immersive_program, "uInstanceBase");
    m_i_u_object_box_count = glGetUniformLocation(m_immersive_program, "uObjectBoxCount");
    m_i_u_allow_behind = glGetUniformLocation(m_immersive_program, "uAllowBehind");
    m_i_u_edge_lr          = glGetUniformLocation(m_immersive_program, "uEdgeLR");
    m_i_u_edge_tb          = glGetUniformLocation(m_immersive_program, "uEdgeTB");
    m_i_u_has_edge_profile = glGetUniformLocation(m_immersive_program, "uHasEdgeProfile");
    m_i_u_rotate90         = glGetUniformLocation(m_immersive_program, "uRotateMode");
    m_i_u_table_mode       = glGetUniformLocation(m_immersive_program, "uTableMode");
    return true;
}

bool GlesRenderer::init_flat_program(std::string& err) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   kFlatVS, err); if (!vs) return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFlatFS, err);
    if (!fs) { glDeleteShader(vs); return false; }
    m_flat_prog = link_program(vs, fs, err);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!m_flat_prog) return false;
    m_flat_u_vp    = glGetUniformLocation(m_flat_prog, "uVP");
    m_flat_u_color = glGetUniformLocation(m_flat_prog, "uColor");
    return true;
}

bool GlesRenderer::init_gun_program(std::string& err) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   kGunVS, err); if (!vs) return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kGunFS, err);
    if (!fs) { glDeleteShader(vs); return false; }
    m_gun_prog = link_program(vs, fs, err);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!m_gun_prog) return false;
    m_gun_u_vp            = glGetUniformLocation(m_gun_prog, "uVP");
    m_gun_u_model         = glGetUniformLocation(m_gun_prog, "uModel");
    m_gun_u_color         = glGetUniformLocation(m_gun_prog, "uColor");
    m_gun_u_vertex_color  = glGetUniformLocation(m_gun_prog, "uVertexColor");
    m_gun_u_trigger       = glGetUniformLocation(m_gun_prog, "uTrigger");
    m_gun_u_recoil        = glGetUniformLocation(m_gun_prog, "uRecoil");
    m_gun_u_tilt          = glGetUniformLocation(m_gun_prog, "uTilt");
    m_gun_u_light_dir     = glGetUniformLocation(m_gun_prog, "uLightDir");
    m_gun_u_light_ambient = glGetUniformLocation(m_gun_prog, "uLightAmbient");
    return true;
}

bool GlesRenderer::init_controller_program(std::string& err) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   kControllerVS, err); if (!vs) return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kControllerFS, err);
    if (!fs) { glDeleteShader(vs); return false; }
    m_controller_prog = link_program(vs, fs, err);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!m_controller_prog) return false;
    m_controller_u_model         = glGetUniformLocation(m_controller_prog, "uModel");
    m_controller_u_vp            = glGetUniformLocation(m_controller_prog, "uVP");
    m_controller_u_color         = glGetUniformLocation(m_controller_prog, "uColor");
    m_controller_u_light_dir     = glGetUniformLocation(m_controller_prog, "uLightDir");
    m_controller_u_light_ambient = glGetUniformLocation(m_controller_prog, "uLightAmbient");
    m_controller_u_tex           = glGetUniformLocation(m_controller_prog, "uTex");
    m_controller_u_has_texture   = glGetUniformLocation(m_controller_prog, "uHasTexture");
    return true;
}

bool GlesRenderer::init_scope_zoom_program(std::string& err) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   kScopeZoomVS, err); if (!vs) return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kScopeZoomFS, err);
    if (!fs) { glDeleteShader(vs); return false; }
    m_scope_zoom_prog = link_program(vs, fs, err);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!m_scope_zoom_prog) return false;
    m_scope_u_tex      = glGetUniformLocation(m_scope_zoom_prog, "uTex");
    m_scope_u_center   = glGetUniformLocation(m_scope_zoom_prog, "uCenter");
    m_scope_u_radius   = glGetUniformLocation(m_scope_zoom_prog, "uRadius");
    m_scope_u_zoom_inv = glGetUniformLocation(m_scope_zoom_prog, "uZoomInv");
    m_scope_u_aspect   = glGetUniformLocation(m_scope_zoom_prog, "uAspect");
    glGenVertexArrays(1, &m_scope_vao);
    return true;
}

bool GlesRenderer::init_ui_program(std::string& err) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   kUiVS, err); if (!vs) return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kUiFS, err);
    if (!fs) { glDeleteShader(vs); return false; }
    m_ui_prog = link_program(vs, fs, err);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!m_ui_prog) return false;

    m_ui_u_vp      = glGetUniformLocation(m_ui_prog, "uVP");
    m_ui_u_model   = glGetUniformLocation(m_ui_prog, "uModel");
    m_ui_u_texture = glGetUniformLocation(m_ui_prog, "uTexture");
    m_ui_u_alpha   = glGetUniformLocation(m_ui_prog, "uAlpha");
    m_ui_u_shadow_mode = glGetUniformLocation(m_ui_prog, "uShadowMode");
    m_ui_u_shadow_color = glGetUniformLocation(m_ui_prog, "uShadowColor");

    // Unit quad in XY plane, centred at origin, pos+uv (5 floats/vert)
    static const float kUiQuad[] = {
        -0.5f,  0.5f, 0.0f,  0.0f, 0.0f,
         0.5f,  0.5f, 0.0f,  1.0f, 0.0f,
        -0.5f, -0.5f, 0.0f,  0.0f, 1.0f,
        -0.5f, -0.5f, 0.0f,  0.0f, 1.0f,
         0.5f,  0.5f, 0.0f,  1.0f, 0.0f,
         0.5f, -0.5f, 0.0f,  1.0f, 1.0f,
    };
    glGenVertexArrays(1, &m_ui_vao);
    glGenBuffers(1, &m_ui_vbo);
    glBindVertexArray(m_ui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_ui_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kUiQuad), kUiQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(3*sizeof(float)));
    glBindVertexArray(0);
    return true;
}

bool GlesRenderer::init_sky_program(std::string& err) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, kSkyVS, err); if (!vs) return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kSkyFS, err);
    if (!fs) { glDeleteShader(vs); return false; }
    m_sky_prog = link_program(vs, fs, err);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!m_sky_prog) return false;
    m_sky_u_proj = glGetUniformLocation(m_sky_prog, "uProj");
    m_sky_u_view = glGetUniformLocation(m_sky_prog, "uViewRot");
    m_sky_u_bands = glGetUniformLocation(m_sky_prog, "uBands[0]");
    m_sky_u_mode = glGetUniformLocation(m_sky_prog, "uMode");
    return true;
}

bool GlesRenderer::init_box_program(std::string& err) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   kBoxLayerVS, err); if (!vs) return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kBoxLayerFS, err);
    if (!fs) { glDeleteShader(vs); return false; }
    m_box_program = link_program(vs, fs, err);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!m_box_program) return false;

    m_box_u_vp           = glGetUniformLocation(m_box_program, "uVP");
    m_box_u_depth        = glGetUniformLocation(m_box_program, "uDepth");
    m_box_u_zbuffer_depths = glGetUniformLocation(m_box_program, "uZBufferDepths");
    m_box_u_quad_w       = glGetUniformLocation(m_box_program, "uQuadW");
    m_box_u_quad_h       = glGetUniformLocation(m_box_program, "uQuadH");
    m_box_u_quad_y       = glGetUniformLocation(m_box_program, "uQuadY");
    m_box_u_table_mode   = glGetUniformLocation(m_box_program, "uTableMode");
    m_box_u_thickness    = glGetUniformLocation(m_box_program, "uThickness");
    m_box_u_screen_curve = glGetUniformLocation(m_box_program, "uScreenCurve");
    m_box_u_orientation  = glGetUniformLocation(m_box_program, "uOrientation");
    m_box_u_allow_behind = glGetUniformLocation(m_box_program, "uAllowBehind");
    m_box_u_ref_l1_depth = glGetUniformLocation(m_box_program, "uRefL1Depth");
    m_box_u_auto_thickness = glGetUniformLocation(m_box_program, "uAutoThickness");
    m_box_u_scatter_range  = glGetUniformLocation(m_box_program, "uScatterRange");
    m_box_u_y_depth_range  = glGetUniformLocation(m_box_program, "uYDepthRange");
    m_box_u_hmd_pos        = glGetUniformLocation(m_box_program, "uHmdPos");
    m_box_u_size_thickness_mode = glGetUniformLocation(m_box_program, "uSizeThicknessMode");
    m_box_u_tilt_x       = glGetUniformLocation(m_box_program, "uTiltX");
    m_box_u_tilt_y       = glGetUniformLocation(m_box_program, "uTiltY");
    m_box_u_canvas_x     = glGetUniformLocation(m_box_program, "uCanvasX");
    m_box_u_canvas_y     = glGetUniformLocation(m_box_program, "uCanvasY");
    m_box_u_canvas_az    = glGetUniformLocation(m_box_program, "uCanvasAz");
    m_box_u_layer_yaw    = glGetUniformLocation(m_box_program, "uLayerYaw");
    m_box_u_canvas_el    = glGetUniformLocation(m_box_program, "uCanvasEl");
    m_box_u_canvas_scale = glGetUniformLocation(m_box_program, "uCanvasScale");
    m_box_u_gamma        = glGetUniformLocation(m_box_program, "uGamma");
    m_box_u_contrast     = glGetUniformLocation(m_box_program, "uContrast");
    m_box_u_saturation   = glGetUniformLocation(m_box_program, "uSaturation");
    m_box_u_brightness   = glGetUniformLocation(m_box_program, "uBrightness");
    m_box_u_pixel_light  = glGetUniformLocation(m_box_program, "uPixelLight");
    m_box_u_light_dir    = glGetUniformLocation(m_box_program, "uLightDir");
    m_box_u_light_ambient = glGetUniformLocation(m_box_program, "uLightAmbient");
    m_box_u_light_flicker = glGetUniformLocation(m_box_program, "uLightFlicker");
    m_box_u_fog_factor    = glGetUniformLocation(m_box_program, "uFogFactor");
    m_box_u_fog_color     = glGetUniformLocation(m_box_program, "uFogColor");
    m_box_u_texture      = glGetUniformLocation(m_box_program, "uTexture");
    m_box_u_force_opaque_alpha = glGetUniformLocation(m_box_program, "uForceOpaqueAlpha");
    m_box_u_silhouette  = glGetUniformLocation(m_box_program, "uSilhouette");
    m_box_u_edge_lr     = glGetUniformLocation(m_box_program, "uEdgeLR");
    m_box_u_edge_tb     = glGetUniformLocation(m_box_program, "uEdgeTB");
    m_box_u_side_color_mode = glGetUniformLocation(m_box_program, "uSideColorMode");
    m_box_u_side_color_rgb  = glGetUniformLocation(m_box_program, "uSideColorRGB");
    m_box_u_side_color_darken = glGetUniformLocation(m_box_program, "uSideColorDarken");
    return true;
}

bool GlesRenderer::init_psx_screen_program(std::string& err) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, kPsxScreenVS, err);
    if (!vs) return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kPsxScreenFS, err);
    if (!fs) { glDeleteShader(vs); return false; }
    m_psx_screen_program = link_program(vs, fs, err);
    glDeleteShader(vs); glDeleteShader(fs);
    if (!m_psx_screen_program) return false;

    m_psx_screen_u_vp           = glGetUniformLocation(m_psx_screen_program, "uVP");
    m_psx_screen_u_texture      = glGetUniformLocation(m_psx_screen_program, "uTexture");
    m_psx_screen_u_depth_tex    = glGetUniformLocation(m_psx_screen_program, "uDepthTex");
    m_psx_screen_u_depth        = glGetUniformLocation(m_psx_screen_program, "uDepth");
    m_psx_screen_u_quad_w       = glGetUniformLocation(m_psx_screen_program, "uQuadW");
    m_psx_screen_u_quad_h       = glGetUniformLocation(m_psx_screen_program, "uQuadH");
    m_psx_screen_u_canvas_x     = glGetUniformLocation(m_psx_screen_program, "uCanvasX");
    m_psx_screen_u_canvas_y     = glGetUniformLocation(m_psx_screen_program, "uCanvasY");
    m_psx_screen_u_canvas_az    = glGetUniformLocation(m_psx_screen_program, "uCanvasAz");
    m_psx_screen_u_canvas_el    = glGetUniformLocation(m_psx_screen_program, "uCanvasEl");
    m_psx_screen_u_canvas_scale = glGetUniformLocation(m_psx_screen_program, "uCanvasScale");
    m_psx_screen_u_depth_scale  = glGetUniformLocation(m_psx_screen_program, "uDepthScale");
    m_psx_screen_u_has_depth    = glGetUniformLocation(m_psx_screen_program, "uHasDepth");
    m_psx_screen_u_flip_v       = glGetUniformLocation(m_psx_screen_program, "uFlipV");
    m_psx_screen_u_color_uv_scale = glGetUniformLocation(m_psx_screen_program, "uColorUvScale");
    m_psx_screen_u_color_uv_offset = glGetUniformLocation(m_psx_screen_program, "uColorUvOffset");
    m_psx_screen_u_grid_step    = glGetUniformLocation(m_psx_screen_program, "uGridStep");
    m_psx_screen_u_edge_darken  = glGetUniformLocation(m_psx_screen_program, "uEdgeDarken");
    m_psx_screen_u_edge_mode    = glGetUniformLocation(m_psx_screen_program, "uEdgeMode");
    m_psx_screen_u_depth_pivot  = glGetUniformLocation(m_psx_screen_program, "uDepthPivot");

    glGenVertexArrays(1, &m_psx_screen_vao);
    glGenBuffers(1, &m_psx_screen_vbo);
    glGenBuffers(1, &m_psx_screen_ibo);
    build_psx_screen_mesh(kPsxScreenGridX, kPsxScreenGridY);

    glGenTextures(1, &m_psx_screen_color);
    glBindTexture(GL_TEXTURE_2D, m_psx_screen_color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &m_psx_screen_depth_tex);
    glBindTexture(GL_TEXTURE_2D, m_psx_screen_depth_tex);
    // Linear: softens the depth step at silhouette edges into a short ramp
    // instead of a one-texel tear.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

void GlesRenderer::build_psx_screen_mesh(int grid_x, int grid_y) {
    // The mesh is in normalised screen space, so it is independent of the
    // game's resolution — it only needs rebuilding when the density changes.
    // Density is what limits displacement detail: depth is sampled per vertex,
    // and the stretched face at a silhouette is exactly one cell wide.
    grid_x = std::clamp(grid_x, 16, 1024);
    grid_y = std::clamp(grid_y, 16, 1024);

    std::vector<float> grid;
    grid.reserve((std::size_t)(grid_x + 1) * (grid_y + 1) * 2);
    for (int y = 0; y <= grid_y; ++y) {
        for (int x = 0; x <= grid_x; ++x) {
            grid.push_back((float)x / (float)grid_x);
            grid.push_back((float)y / (float)grid_y);
        }
    }
    std::vector<uint32_t> indices;
    indices.reserve((std::size_t)grid_x * grid_y * 6);
    const uint32_t stride = (uint32_t)grid_x + 1u;
    for (uint32_t y = 0; y < (uint32_t)grid_y; ++y) {
        for (uint32_t x = 0; x < (uint32_t)grid_x; ++x) {
            const uint32_t i0 = y * stride + x;
            const uint32_t i1 = i0 + 1u;
            const uint32_t i2 = i0 + stride;
            const uint32_t i3 = i2 + 1u;
            indices.push_back(i0); indices.push_back(i2); indices.push_back(i1);
            indices.push_back(i1); indices.push_back(i2); indices.push_back(i3);
        }
    }
    m_psx_screen_index_count = (GLsizei)indices.size();
    m_psx_screen_grid_x = grid_x;
    m_psx_screen_grid_y = grid_y;

    glBindVertexArray(m_psx_screen_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_psx_screen_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(grid.size() * sizeof(float)),
                 grid.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_psx_screen_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(indices.size() * sizeof(uint32_t)),
                 indices.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);

    LOGI("PSX mesh: %dx%d grid, %d triangles per eye",
         grid_x, grid_y, m_psx_screen_index_count / 3);
}

bool GlesRenderer::draw_psx_screen(const LayerFrame& frame, const PsxDepthFrame* depth,
                                   const PsxGpuFrame* gpu,
                                   const Mat4& view, const Mat4& proj,
                                   float canvas_x, float canvas_y, float canvas_az,
                                   float canvas_el, float canvas_scale) {
    if (!m_psx_screen_program || !m_psx_screen_vao) return false;
    // Zero copy when the hardware renderer is live: these are the emulator's own
    // textures, already fenced. The CPU upload below is the software fallback.
    const bool use_gpu = gpu && gpu->color_texture != 0 && gpu->width > 0 && gpu->height > 0;
    if (!use_gpu && (frame.width <= 0 || frame.height <= 0 || frame.rgba.empty())) return false;
    const int src_w = use_gpu ? gpu->width : frame.width;
    const int src_h = use_gpu ? gpu->height : frame.height;

    bool has_depth = false;
    if (use_gpu) {
        has_depth = gpu->has_depth && gpu->depth_texture != 0;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gpu->color_texture);
        if (has_depth) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, gpu->depth_texture);
        }
    } else {
        has_depth = depth && depth->has_geometry &&
                    depth->width == (uint32_t)frame.width &&
                    depth->height == (uint32_t)frame.height &&
                    depth->depth.size() == (std::size_t)frame.width * frame.height;

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_psx_screen_color);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (m_psx_screen_color_w != frame.width || m_psx_screen_color_h != frame.height) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame.width, frame.height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, frame.rgba.data());
            m_psx_screen_color_w = frame.width;
            m_psx_screen_color_h = frame.height;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
                            GL_RGBA, GL_UNSIGNED_BYTE, frame.rgba.data());
        }

        if (has_depth) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, m_psx_screen_depth_tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            if (m_psx_screen_depth_w != (int)depth->width || m_psx_screen_depth_h != (int)depth->height) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, depth->width, depth->height, 0,
                             GL_RED, GL_UNSIGNED_BYTE, depth->depth.data());
                m_psx_screen_depth_w = (int)depth->width;
                m_psx_screen_depth_h = (int)depth->height;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, depth->width, depth->height,
                                GL_RED, GL_UNSIGNED_BYTE, depth->depth.data());
            }
        }
    }

    const float quad_w = (frame.quad_width_meters > 0.0f) ? frame.quad_width_meters : 2.56f;
    const float quad_h = quad_w * (float)src_h / (float)std::max(1, src_w);

    const Mat4 vp = Mat4::mul(proj, view);
    glUseProgram(m_psx_screen_program);
    glUniformMatrix4fv(m_psx_screen_u_vp, 1, GL_FALSE, vp.data());
    glUniform1i(m_psx_screen_u_texture, 0);
    glUniform1i(m_psx_screen_u_depth_tex, 1);
    glUniform1f(m_psx_screen_u_depth, frame.depth_meters > 0.0f ? frame.depth_meters : 1.5f);
    glUniform1f(m_psx_screen_u_quad_w, quad_w);
    glUniform1f(m_psx_screen_u_quad_h, quad_h);
    glUniform1f(m_psx_screen_u_canvas_x, canvas_x);
    glUniform1f(m_psx_screen_u_canvas_y, canvas_y);
    glUniform1f(m_psx_screen_u_canvas_az, canvas_az);
    glUniform1f(m_psx_screen_u_canvas_el, canvas_el);
    glUniform1f(m_psx_screen_u_canvas_scale, canvas_scale);
    // Re-read occasionally rather than every draw: this is a tuning knob, and
    // __system_property_get is not free at 72Hz x 2 eyes.
    {
        static float depth_metres = kPsxScreenDepthMetres;
        static float edge_darken = kPsxScreenEdgeDarken;
        static int edge_mode = kPsxScreenEdgeMode;
        static float depth_pivot = qrd::kPsxDepthPivotDefault;
        static int depth_prop_poll = 0;
        if (++depth_prop_poll % 240 == 1) {
            char buf[PROP_VALUE_MAX] = {0};
            depth_metres = kPsxScreenDepthMetres;
            // Range allows deliberate over-exaggeration (10x the default and
            // beyond) when checking whether an artefact is depth-related.
            if (__system_property_get("debug.qrd.psxdepth", buf) > 0) {
                const float v = (float)atof(buf);
                if (v > 0.0f && v <= 10.0f) depth_metres = v;
            }
            buf[0] = '\0';
            edge_darken = kPsxScreenEdgeDarken;
            if (__system_property_get("debug.qrd.psxedge", buf) > 0) {
                const float v = (float)atof(buf);
                if (v >= 0.0f && v <= 64.0f) edge_darken = v;
            }
            buf[0] = '\0';
            edge_mode = kPsxScreenEdgeMode;
            if (__system_property_get("debug.qrd.psxedgemode", buf) > 0) {
                const int v = atoi(buf);
                if (v >= 0 && v <= 2) edge_mode = v;
            }
            // Must match the resolve pass, which writes this value for pixels
            // it could not resolve.
            buf[0] = '\0';
            depth_pivot = qrd::kPsxDepthPivotDefault;
            if (__system_property_get("debug.qrd.psxpivot", buf) > 0) {
                const float v = (float)atof(buf);
                if (v >= 0.0f && v <= 1.0f) depth_pivot = v;
            }
            // Mesh density. Depth is sampled per vertex, so this — not the
            // depth texture — is what limits displacement detail, and the
            // stretched face at a silhouette is one cell wide. Given as the
            // horizontal count; the vertical follows the image aspect so cells
            // stay square.
            buf[0] = '\0';
            int want_x = kPsxScreenGridX;
            if (__system_property_get("debug.qrd.psxgrid", buf) > 0) {
                const int v = atoi(buf);
                if (v >= 16 && v <= 1024) want_x = v;
            }
            const int want_y = (src_w > 0 && src_h > 0)
                                   ? std::clamp(want_x * src_h / src_w, 16, 1024)
                                   : kPsxScreenGridY;
            if (want_x != m_psx_screen_grid_x || want_y != m_psx_screen_grid_y)
                build_psx_screen_mesh(want_x, want_y);
        }
        glUniform1f(m_psx_screen_u_depth_scale, depth_metres);
        glUniform1f(m_psx_screen_u_edge_darken, edge_darken);
        glUniform1i(m_psx_screen_u_edge_mode, edge_mode);
        glUniform1f(m_psx_screen_u_depth_pivot, depth_pivot);
        glUniform2f(m_psx_screen_u_grid_step,
                    1.0f / (float)m_psx_screen_grid_x, 1.0f / (float)m_psx_screen_grid_y);
    }
    glUniform1i(m_psx_screen_u_has_depth, has_depth ? 1 : 0);
    glUniform1i(m_psx_screen_u_flip_v, use_gpu ? 1 : 0);
    // The emulator's slots are sized for its max geometry, so the live image is
    // only the bottom-left corner of the texture; the CPU path uploads a
    // texture that is exactly the image.
    float uv_scale_x = 1.0f, uv_scale_y = 1.0f;
    float uv_off_x = 0.0f, uv_off_y = 0.0f;
    if (use_gpu && gpu->tex_width > 0 && gpu->tex_height > 0) {
        uv_scale_x = (float)gpu->width / (float)gpu->tex_width;
        uv_scale_y = (float)gpu->height / (float)gpu->tex_height;
        // Where the scanned-out window actually sits in the shared texture --
        // not always the origin (see PsxGpuFrame::x/y).
        uv_off_x = (float)gpu->x / (float)gpu->tex_width;
        uv_off_y = (float)gpu->y / (float)gpu->tex_height;
    }
    glUniform2f(m_psx_screen_u_color_uv_scale, uv_scale_x, uv_scale_y);
    glUniform2f(m_psx_screen_u_color_uv_offset, uv_off_x, uv_off_y);
    {
        static int uv_dbg = 0;
        if (++uv_dbg % 240 == 1) {
            LOGI("PSX UV: use_gpu=%d img=%dx%d tex=%dx%d scale=%.4f,%.4f has_depth=%d",
                 (int)use_gpu,
                 use_gpu ? gpu->width : frame.width,
                 use_gpu ? gpu->height : frame.height,
                 use_gpu ? gpu->tex_width : 0, use_gpu ? gpu->tex_height : 0,
                 uv_scale_x, uv_scale_y, (int)has_depth);
        }
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(m_psx_screen_vao);
    glDrawElements(GL_TRIANGLES, m_psx_screen_index_count, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    return true;
}

bool GlesRenderer::init(std::string& error_out) {
    // GPU frame-time: check for GL_EXT_disjoint_timer_query and load the 64-bit result getter
    // (glGetQueryObjectui64vEXT isn't in the GLES3 core headers we include). glGenQueries/
    // glBeginQuery/glEndQuery/glGetQueryObjectuiv ARE core GLES3 entry points and the extension
    // reuses them with the GL_TIME_ELAPSED_EXT target, so only this one function needs loading.
    {
        const char* ext = (const char*)glGetString(GL_EXTENSIONS);
        if (ext && std::strstr(ext, "GL_EXT_disjoint_timer_query")) {
            m_glGetQueryObjectui64vEXT = reinterpret_cast<void (*)(GLuint, GLenum, uint64_t*)>(
                eglGetProcAddress("glGetQueryObjectui64vEXT"));
            if (m_glGetQueryObjectui64vEXT) {
                glGenQueries(2, m_gpu_query);
                m_gpu_timer_supported = true;
            }
        }
    }

    if (!init_layer_program(error_out)) return false;
    std::string psx_screen_err;
    if (!init_psx_screen_program(psx_screen_err)) {
        // Native PSX replay is deliberately optional. Older GLES drivers can
        // lack integer texture support; leave the normal layered renderer
        // usable and let the frame path fall back to it.
        LOGE("PSX depth-screen program unavailable; using flat fallback: %s", psx_screen_err.c_str());
        if (m_psx_screen_program) { glDeleteProgram(m_psx_screen_program); m_psx_screen_program = 0; }
    }
    if (!init_flat_program(error_out))  return false;
    if (!init_gun_program(error_out))   return false;
    if (!init_scope_zoom_program(error_out)) return false;
    if (!init_sky_program(error_out))   return false;
    if (!init_ui_program(error_out))    return false;
    std::string immersive_err;
    if (!init_immersive_layer_program(immersive_err)) {
        LOGE("Immersive layer program unavailable; using flat fallback: %s", immersive_err.c_str());
        if (m_immersive_program) { glDeleteProgram(m_immersive_program); m_immersive_program = 0; }
    }
    std::string box_err;
    if (!init_box_program(box_err)) {
        LOGE("Box layer program unavailable; bbox mode will fall back to card stack: %s", box_err.c_str());
        if (m_box_program) { glDeleteProgram(m_box_program); m_box_program = 0; }
    }
    std::string controller_err;
    if (!init_controller_program(controller_err)) {
        LOGE("Controller model program unavailable; controller models won't render: %s", controller_err.c_str());
        if (m_controller_prog) { glDeleteProgram(m_controller_prog); m_controller_prog = 0; }
    }
    init_controller_label_font();

    // Unit centred quad  (pos xyz, uv xy) — 6 vertices, 2 triangles
    // UV: y=0 at top of quad (top of image), y=1 at bottom
    static const float kQuadVerts[] = {
        -0.5f,  0.5f, 0.0f,  0.0f, 0.0f,   // top-left
         0.5f,  0.5f, 0.0f,  1.0f, 0.0f,   // top-right
        -0.5f, -0.5f, 0.0f,  0.0f, 1.0f,   // bottom-left
        -0.5f, -0.5f, 0.0f,  0.0f, 1.0f,   // bottom-left
         0.5f,  0.5f, 0.0f,  1.0f, 0.0f,   // top-right
         0.5f, -0.5f, 0.0f,  1.0f, 1.0f,   // bottom-right
    };
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuadVerts), kQuadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);

    glGenBuffers(1, &m_object_box_ssbo);
    glGenBuffers(1, &m_object_depth_ssbo);

    // Box mesh: back face (z=-1, normal quad UV) + 4 side faces (z sweeps -1..0, edge_t sweeps
    // the side) — 5 faces × 6 verts (2 tris) = 30 verts. aUV.x is unused on side faces (edge_t
    // lives in aUV.y; see kBoxLayerVS's per-face UV derivation from the SSBO subrect).
    {
        struct BoxVert { float x, y, z, u, v, face; };
        std::vector<BoxVert> bv;
        bv.reserve(30);
        auto quad = [&](float face,
                         float x0, float y0, float z0, float u0, float v0,
                         float x1, float y1, float z1, float u1, float v1,
                         float x2, float y2, float z2, float u2, float v2,
                         float x3, float y3, float z3, float u3, float v3) {
            bv.push_back({x0, y0, z0, u0, v0, face});
            bv.push_back({x1, y1, z1, u1, v1, face});
            bv.push_back({x2, y2, z2, u2, v2, face});
            bv.push_back({x2, y2, z2, u2, v2, face});
            bv.push_back({x1, y1, z1, u1, v1, face});
            bv.push_back({x3, y3, z3, u3, v3, face});
        };
        // Back face (face=0): z=-1, standard quad UV (mirrors front sprite).
        quad(0.0f,
             -0.5f,  0.5f, -1.0f, 0.0f, 0.0f,
              0.5f,  0.5f, -1.0f, 1.0f, 0.0f,
             -0.5f, -0.5f, -1.0f, 0.0f, 1.0f,
              0.5f, -0.5f, -1.0f, 1.0f, 1.0f);
        // Left face (face=1): x=-0.5, spans z:[-1,0], y:[-0.5,0.5]. edge_t (aUV.y) = y position.
        quad(1.0f,
             -0.5f,  0.5f, -1.0f, 0.0f, 0.0f,
             -0.5f,  0.5f,  0.0f, 0.0f, 0.0f,
             -0.5f, -0.5f, -1.0f, 0.0f, 1.0f,
             -0.5f, -0.5f,  0.0f, 0.0f, 1.0f);
        // Right face (face=2): x=0.5.
        quad(2.0f,
              0.5f,  0.5f,  0.0f, 0.0f, 0.0f,
              0.5f,  0.5f, -1.0f, 0.0f, 0.0f,
              0.5f, -0.5f,  0.0f, 0.0f, 1.0f,
              0.5f, -0.5f, -1.0f, 0.0f, 1.0f);
        // Top face (face=3): y=0.5, spans z:[-1,0], x:[-0.5,0.5]. edge_t (aUV.y) = x position.
        quad(3.0f,
             -0.5f,  0.5f, -1.0f, 0.0f, 0.0f,
              0.5f,  0.5f, -1.0f, 1.0f, 0.0f,
             -0.5f,  0.5f,  0.0f, 0.0f, 0.0f,
              0.5f,  0.5f,  0.0f, 1.0f, 0.0f);
        // Bottom face (face=4): y=-0.5.
        quad(4.0f,
             -0.5f, -0.5f,  0.0f, 0.0f, 0.0f,
              0.5f, -0.5f,  0.0f, 1.0f, 0.0f,
             -0.5f, -0.5f, -1.0f, 0.0f, 0.0f,
              0.5f, -0.5f, -1.0f, 1.0f, 0.0f);

        m_box_vertex_count = (int)bv.size();
        glGenVertexArrays(1, &m_box_vao);
        glGenBuffers(1, &m_box_vbo);
        glBindVertexArray(m_box_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_box_vbo);
        glBufferData(GL_ARRAY_BUFFER, bv.size() * sizeof(BoxVert), bv.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BoxVert), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(BoxVert), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(BoxVert), (void*)(5 * sizeof(float)));
        glBindVertexArray(0);
    }

    constexpr int kCurveStrips = 48;
    std::vector<float> curve_verts;
    curve_verts.reserve(kCurveStrips * 6 * 5);
    auto push_curve_vert = [&](float x, float y, float u, float v) {
        curve_verts.push_back(x);
        curve_verts.push_back(y);
        curve_verts.push_back(0.0f);
        curve_verts.push_back(u);
        curve_verts.push_back(v);
    };
    for (int s = 0; s < kCurveStrips; ++s) {
        const float u0 = (float)s / (float)kCurveStrips;
        const float u1 = (float)(s + 1) / (float)kCurveStrips;
        const float x0 = -0.5f + u0;
        const float x1 = -0.5f + u1;
        push_curve_vert(x0,  0.5f, u0, 0.0f);
        push_curve_vert(x1,  0.5f, u1, 0.0f);
        push_curve_vert(x0, -0.5f, u0, 1.0f);
        push_curve_vert(x0, -0.5f, u0, 1.0f);
        push_curve_vert(x1,  0.5f, u1, 0.0f);
        push_curve_vert(x1, -0.5f, u1, 1.0f);
    }
    m_curve_vertex_count = (int)(curve_verts.size() / 5);
    glGenVertexArrays(1, &m_curve_vao);
    glGenBuffers(1, &m_curve_vbo);
    glBindVertexArray(m_curve_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_curve_vbo);
    glBufferData(GL_ARRAY_BUFFER, curve_verts.size() * sizeof(float), curve_verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);

    // Flat VAO (xyz + alpha per vertex, dynamically updated; 64 verts max)
    glGenVertexArrays(1, &m_flat_vao);
    glGenBuffers(1, &m_flat_vbo);
    glBindVertexArray(m_flat_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_flat_vbo);
    glBufferData(GL_ARRAY_BUFFER, 64 * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);

    // Procedural gun VAO (xyz + normal per vertex, dynamically updated per
    // shape; retained for the Zapper and scope variants).
    glGenVertexArrays(1, &m_gun_vao);
    glGenBuffers(1, &m_gun_vbo);
    glBindVertexArray(m_gun_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_gun_vbo);
    glBufferData(GL_ARRAY_BUFFER, 512 * 6 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);

    // Downloaded CC0 pistol: baked position + normal + palette color. It is
    // uploaded once and rendered in one draw call, so the replacement remains
    // cheaper than the old multi-part procedural pistol.
    glGenVertexArrays(1, &m_pistol_vao);
    glGenBuffers(1, &m_pistol_vbo);
    glBindVertexArray(m_pistol_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_pistol_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kDownloadedPistolVertices),
                 kDownloadedPistolVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float),
                          (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 10 * sizeof(float),
                          (void*)(9 * sizeof(float)));
    glBindVertexArray(0);
    m_pistol_vertex_count = kDownloadedPistolVertexCount;

    constexpr float kSkyRadius = 18.0f;
    constexpr int kSkyLatBands = 24;
    constexpr int kSkyLonBands = 32;
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kHalfPi = 1.57079632679489661923f;
    std::vector<float> sky_verts;
    sky_verts.reserve(kSkyLatBands * kSkyLonBands * 6 * 3);
    auto push_sky = [&](float x, float y, float z) {
        sky_verts.push_back(x);
        sky_verts.push_back(y);
        sky_verts.push_back(z);
    };
    for (int lat = 0; lat < kSkyLatBands; ++lat) {
        const float v0 = (float)lat / (float)kSkyLatBands;
        const float v1 = (float)(lat + 1) / (float)kSkyLatBands;
        const float theta0 = -kHalfPi + v0 * kPi;
        const float theta1 = -kHalfPi + v1 * kPi;
        const float y0 = std::sin(theta0) * kSkyRadius;
        const float y1 = std::sin(theta1) * kSkyRadius;
        const float r0 = std::cos(theta0) * kSkyRadius;
        const float r1 = std::cos(theta1) * kSkyRadius;
        for (int lon = 0; lon < kSkyLonBands; ++lon) {
            const float u0 = (float)lon / (float)kSkyLonBands;
            const float u1 = (float)(lon + 1) / (float)kSkyLonBands;
            const float phi0 = u0 * 2.0f * kPi;
            const float phi1 = u1 * 2.0f * kPi;
            const float x00 = std::cos(phi0) * r0;
            const float z00 = std::sin(phi0) * r0;
            const float x01 = std::cos(phi1) * r0;
            const float z01 = std::sin(phi1) * r0;
            const float x10 = std::cos(phi0) * r1;
            const float z10 = std::sin(phi0) * r1;
            const float x11 = std::cos(phi1) * r1;
            const float z11 = std::sin(phi1) * r1;
            push_sky(x00, y0, z00);
            push_sky(x11, y1, z11);
            push_sky(x10, y1, z10);
            push_sky(x00, y0, z00);
            push_sky(x01, y0, z01);
            push_sky(x11, y1, z11);
        }
    }
    m_sky_vertex_count = (int)(sky_verts.size() / 3);
    glGenVertexArrays(1, &m_sky_vao);
    glGenBuffers(1, &m_sky_vbo);
    glBindVertexArray(m_sky_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_sky_vbo);
    glBufferData(GL_ARRAY_BUFFER, sky_verts.size() * sizeof(float), sky_verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);

    return true;
}

void GlesRenderer::begin_gpu_timer() {
    if (!m_gpu_timer_supported) return;
    const int idx = m_gpu_query_write_idx;
    // Read back the OTHER buffer's result (from ~2 frames ago) first, if ready — never blocks,
    // since we only check availability instead of forcing a wait.
    const int read_idx = 1 - idx;
    if (m_gpu_query_has_result[read_idx]) {
        GLuint available = 0;
        glGetQueryObjectuiv(m_gpu_query[read_idx], GL_QUERY_RESULT_AVAILABLE, &available);
        if (available) {
            uint64_t ns = 0;
            m_glGetQueryObjectui64vEXT(m_gpu_query[read_idx], GL_QUERY_RESULT, &ns);
            m_last_gpu_ms = (float)ns / 1e6f;
        }
    }
    glBeginQuery(GL_TIME_ELAPSED_EXT, m_gpu_query[idx]);
}

void GlesRenderer::end_gpu_timer() {
    if (!m_gpu_timer_supported) return;
    glEndQuery(GL_TIME_ELAPSED_EXT);
    m_gpu_query_has_result[m_gpu_query_write_idx] = true;
    m_gpu_query_write_idx = 1 - m_gpu_query_write_idx;
}

void GlesRenderer::shutdown() {
    if (m_gpu_timer_supported) {
        glDeleteQueries(2, m_gpu_query);
        m_gpu_timer_supported = false;
    }
    for (auto& lt : m_layers) {
        if (lt.tex)         { glDeleteTextures(1, &lt.tex);         lt.tex         = 0; }
        if (lt.depth_tex)   { glDeleteTextures(1, &lt.depth_tex);   lt.depth_tex   = 0; }
        if (lt.edge_lr_tex) { glDeleteTextures(1, &lt.edge_lr_tex); lt.edge_lr_tex = 0; }
        if (lt.edge_tb_tex) { glDeleteTextures(1, &lt.edge_tb_tex); lt.edge_tb_tex = 0; }
    }
    m_layers.clear();
    if (m_dm_ebo) { glDeleteBuffers(1, &m_dm_ebo);        m_dm_ebo = 0; }
    if (m_dm_vbo) { glDeleteBuffers(1, &m_dm_vbo);        m_dm_vbo = 0; }
    if (m_dm_vao) { glDeleteVertexArrays(1, &m_dm_vao);   m_dm_vao = 0; }
    m_dm_W = m_dm_H = m_dm_index_count = 0;
    if (m_vbo)       { glDeleteBuffers(1, &m_vbo);       m_vbo       = 0; }
    if (m_vao)       { glDeleteVertexArrays(1, &m_vao);  m_vao       = 0; }
    if (m_object_box_ssbo) { glDeleteBuffers(1, &m_object_box_ssbo); m_object_box_ssbo = 0; }
    if (m_object_depth_ssbo) { glDeleteBuffers(1, &m_object_depth_ssbo); m_object_depth_ssbo = 0; }
    if (m_curve_vbo) { glDeleteBuffers(1, &m_curve_vbo); m_curve_vbo = 0; }
    if (m_curve_vao) { glDeleteVertexArrays(1, &m_curve_vao); m_curve_vao = 0; }
    if (m_flat_vbo)  { glDeleteBuffers(1, &m_flat_vbo);  m_flat_vbo  = 0; }
    if (m_flat_vao)  { glDeleteVertexArrays(1, &m_flat_vao); m_flat_vao = 0; }
    if (m_gun_vbo)   { glDeleteBuffers(1, &m_gun_vbo);   m_gun_vbo   = 0; }
    if (m_gun_vao)   { glDeleteVertexArrays(1, &m_gun_vao); m_gun_vao = 0; }
    if (m_pistol_vbo) { glDeleteBuffers(1, &m_pistol_vbo); m_pistol_vbo = 0; }
    if (m_pistol_vao) { glDeleteVertexArrays(1, &m_pistol_vao); m_pistol_vao = 0; }
    m_pistol_vertex_count = 0;
    for (ControllerModel& cm : m_controller_model) {
        for (ControllerModelMesh& gm : cm.meshes) {
            if (gm.ebo) glDeleteBuffers(1, &gm.ebo);
            if (gm.vbo) glDeleteBuffers(1, &gm.vbo);
            if (gm.vao) glDeleteVertexArrays(1, &gm.vao);
            if (gm.tex) glDeleteTextures(1, &gm.tex);
        }
        cm = ControllerModel{};
    }
    if (m_controller_prog) { glDeleteProgram(m_controller_prog); m_controller_prog = 0; }
    if (m_controller_label_vbo) { glDeleteBuffers(1, &m_controller_label_vbo); m_controller_label_vbo = 0; }
    if (m_controller_label_vao) { glDeleteVertexArrays(1, &m_controller_label_vao); m_controller_label_vao = 0; }
    if (m_controller_label_tex) { glDeleteTextures(1, &m_controller_label_tex); m_controller_label_tex = 0; }
    if (m_scope_copy_tex) { glDeleteTextures(1, &m_scope_copy_tex); m_scope_copy_tex = 0; }
    if (m_scope_copy_fbo) { glDeleteFramebuffers(1, &m_scope_copy_fbo); m_scope_copy_fbo = 0; }
    if (m_scope_vao) { glDeleteVertexArrays(1, &m_scope_vao); m_scope_vao = 0; }
    if (m_scope_zoom_prog) { glDeleteProgram(m_scope_zoom_prog); m_scope_zoom_prog = 0; }
    if (m_sky_vbo)   { glDeleteBuffers(1, &m_sky_vbo);   m_sky_vbo   = 0; }
    if (m_sky_vao)   { glDeleteVertexArrays(1, &m_sky_vao); m_sky_vao = 0; }
    if (m_program)   { glDeleteProgram(m_program);       m_program   = 0; }
    if (m_immersive_program) { glDeleteProgram(m_immersive_program); m_immersive_program = 0; }
    if (m_box_vbo)   { glDeleteBuffers(1, &m_box_vbo);   m_box_vbo   = 0; }
    if (m_box_vao)   { glDeleteVertexArrays(1, &m_box_vao); m_box_vao = 0; }
    if (m_box_program) { glDeleteProgram(m_box_program); m_box_program = 0; }
    if (m_flat_prog) { glDeleteProgram(m_flat_prog);     m_flat_prog = 0; }
    if (m_gun_prog)  { glDeleteProgram(m_gun_prog);      m_gun_prog  = 0; }
    if (m_sky_prog)  { glDeleteProgram(m_sky_prog);      m_sky_prog  = 0; }
    if (m_ui_vbo)    { glDeleteBuffers(1, &m_ui_vbo);    m_ui_vbo    = 0; }
    if (m_ui_vao)    { glDeleteVertexArrays(1, &m_ui_vao); m_ui_vao  = 0; }
    if (m_ui_prog)   { glDeleteProgram(m_ui_prog);       m_ui_prog   = 0; }
    if (m_psx_screen_color) { glDeleteTextures(1, &m_psx_screen_color); m_psx_screen_color = 0; }
    if (m_psx_screen_depth_tex) { glDeleteTextures(1, &m_psx_screen_depth_tex); m_psx_screen_depth_tex = 0; }
    if (m_psx_screen_ibo) { glDeleteBuffers(1, &m_psx_screen_ibo); m_psx_screen_ibo = 0; }
    if (m_psx_screen_vbo) { glDeleteBuffers(1, &m_psx_screen_vbo); m_psx_screen_vbo = 0; }
    if (m_psx_screen_vao) { glDeleteVertexArrays(1, &m_psx_screen_vao); m_psx_screen_vao = 0; }
    if (m_psx_screen_program) { glDeleteProgram(m_psx_screen_program); m_psx_screen_program = 0; }
}

// ---------------------------------------------------------------------------
// Layer texture management
// ---------------------------------------------------------------------------

void GlesRenderer::resize_layers(int n) {
    const int cur = (int)m_layers.size();
    if (cur < n) {
        m_layers.resize(n);
        for (int i = cur; i < n; ++i) {
            glGenTextures(1, &m_layers[i].tex);
            glBindTexture(GL_TEXTURE_2D, m_layers[i].tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void GlesRenderer::ensure_depth_mesh(int W, int H) {
    if (m_dm_W == W && m_dm_H == H && m_dm_vao != 0) return;

    if (m_dm_ebo) { glDeleteBuffers(1, &m_dm_ebo);      m_dm_ebo = 0; }
    if (m_dm_vbo) { glDeleteBuffers(1, &m_dm_vbo);      m_dm_vbo = 0; }
    if (m_dm_vao) { glDeleteVertexArrays(1, &m_dm_vao); m_dm_vao = 0; }

    // (W+1)*(H+1) vertices: [x, y, z, u, v], positions in [-0.5, 0.5]
    const int nverts = (W + 1) * (H + 1);
    std::vector<float> verts(nverts * 5);
    for (int vy = 0; vy <= H; ++vy) {
        const float v =  (float)vy / (float)H;   // 0=top, 1=bottom
        const float y =  0.5f - v;               // 0.5=top, -0.5=bottom
        for (int vx = 0; vx <= W; ++vx) {
            const float u = (float)vx / (float)W; // 0=left, 1=right
            const float x = u - 0.5f;
            float* p = verts.data() + (vy * (W + 1) + vx) * 5;
            p[0] = x; p[1] = y; p[2] = 0.0f; p[3] = u; p[4] = v;
        }
    }

    m_dm_index_count = W * H * 6;
    std::vector<unsigned int> idx(m_dm_index_count);
    int ii = 0;
    for (int qy = 0; qy < H; ++qy) {
        for (int qx = 0; qx < W; ++qx) {
            unsigned int tl = (unsigned int)(qy * (W + 1) + qx);
            unsigned int tr = tl + 1u;
            unsigned int bl = tl + (unsigned int)(W + 1);
            unsigned int br = bl + 1u;
            idx[ii++] = tl; idx[ii++] = bl; idx[ii++] = tr;
            idx[ii++] = tr; idx[ii++] = bl; idx[ii++] = br;
        }
    }

    glGenVertexArrays(1, &m_dm_vao);
    glGenBuffers(1, &m_dm_vbo);
    glGenBuffers(1, &m_dm_ebo);
    glBindVertexArray(m_dm_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_dm_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_dm_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size() * sizeof(unsigned int)), idx.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);

    m_dm_W = W;
    m_dm_H = H;
}

void GlesRenderer::update_layer(int idx, const LayerFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.rgba.empty()) return;
    resize_layers(idx + 1);
    auto& lt = m_layers[idx];
    if (lt.width == frame.width &&
        lt.height == frame.height &&
        lt.uploaded_revision == frame.content_revision) {
        return;
    }
    glBindTexture(GL_TEXTURE_2D, lt.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (lt.width != frame.width || lt.height != frame.height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     frame.width, frame.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, frame.rgba.data());
        lt.width  = frame.width;
        lt.height = frame.height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        frame.width, frame.height,
                        GL_RGBA, GL_UNSIGNED_BYTE, frame.rgba.data());
    }
    lt.uploaded_revision = frame.content_revision;
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Upload depth_map as GL_R8 (always glTexImage2D — size may vary with game)
    if (!frame.depth_map.empty()) {
        if (lt.depth_tex == 0) glGenTextures(1, &lt.depth_tex);
        glBindTexture(GL_TEXTURE_2D, lt.depth_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8,
                     frame.width, frame.height, 0,
                     GL_RED, GL_UNSIGNED_BYTE, frame.depth_map.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // Upload silhouette edge profiles as Nx1 GL_RG8 textures (see LayerFrame::edge_lr/edge_tb).
    // Only present when the layer is wedge_eligible; harmless to skip otherwise since the
    // silhouette-sides shader path only runs for wedge-eligible layers anyway.
    auto upload_edge_profile = [](GLuint& tex, const std::vector<float>& profile, int count) {
        if (profile.empty() || count <= 0) return;
        std::vector<uint8_t> packed((std::size_t)count * 2);
        for (int i = 0; i < count; ++i) {
            packed[(std::size_t)i * 2 + 0] = (uint8_t)std::clamp(profile[(std::size_t)i * 2 + 0] * 255.0f, 0.0f, 255.0f);
            packed[(std::size_t)i * 2 + 1] = (uint8_t)std::clamp(profile[(std::size_t)i * 2 + 1] * 255.0f, 0.0f, 255.0f);
        }
        if (tex == 0) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, count, 1, 0, GL_RG, GL_UNSIGNED_BYTE, packed.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    };
    upload_edge_profile(lt.edge_lr_tex, frame.edge_lr, frame.height);
    upload_edge_profile(lt.edge_tb_tex, frame.edge_tb, frame.width);
}

// ---------------------------------------------------------------------------
// Eye FBO
// ---------------------------------------------------------------------------

EyeFbo GlesRenderer::make_eye_fbo(GLuint color_tex, int width, int height) {
    EyeFbo fbo;
    fbo.color_tex = color_tex;
    fbo.width     = width;
    fbo.height    = height;

    glGenRenderbuffers(1, &fbo.depth_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, fbo.depth_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);

    glGenFramebuffers(1, &fbo.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fbo.depth_rbo);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return fbo;
}

void GlesRenderer::destroy_eye_fbo(EyeFbo& fbo) {
    if (fbo.fbo)       { glDeleteFramebuffers(1,  &fbo.fbo);       fbo.fbo       = 0; }
    if (fbo.depth_rbo) { glDeleteRenderbuffers(1, &fbo.depth_rbo); fbo.depth_rbo = 0; }
}

UiFbo GlesRenderer::make_or_resize_ui_fbo(UiFbo fbo, int width, int height) {
    if (fbo.fbo && fbo.width == width && fbo.height == height) return fbo;
    destroy_ui_fbo(fbo);

    glGenTextures(1, &fbo.color_tex);
    glBindTexture(GL_TEXTURE_2D, fbo.color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    // Tried GL_LINEAR_MIPMAP_LINEAR + per-frame glGenerateMipmap here to fix thin
    // border-line aliasing, but it visibly blurred the whole panel (the GPU's mip
    // selection was much more aggressive than the actual minification warranted) —
    // reverted to plain GL_LINEAR. The border fix now lives in imgui_bridge.cpp's
    // apply_theme() as thicker border strokes instead, which doesn't touch overall
    // texture sharpness at all.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &fbo.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo.color_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    fbo.width  = width;
    fbo.height = height;
    return fbo;
}

void GlesRenderer::destroy_ui_fbo(UiFbo& fbo) {
    if (fbo.fbo)       { glDeleteFramebuffers(1, &fbo.fbo);       fbo.fbo       = 0; }
    if (fbo.color_tex) { glDeleteTextures(1,     &fbo.color_tex); fbo.color_tex = 0; }
    fbo.width = fbo.height = 0;
}

// ---------------------------------------------------------------------------
// Ambilight helper — draw glowing shells around the screen edges
// ---------------------------------------------------------------------------

void GlesRenderer::draw_ambilight(const std::vector<LayerFrame*>& frames,
                                   const Mat4& vp, const VrState& state) {
    if (frames.empty() || m_flat_prog == 0) return;

    // Sample edge pixels from all layers that have contrib_ambilight set.
    // Fall back to the first frame for depth/size reference.
    float r = 0, g = 0, b = 0, cnt = 0;
    const LayerFrame* ref_frame = nullptr;
    for (const LayerFrame* lfp : frames) {
        if (!lfp) continue;
        const LayerFrame& lf = *lfp;
        if (!lf.contrib_ambilight) continue;
        if (lf.rgba.empty() || lf.width <= 0 || lf.height <= 0 || !lf.has_pixels) continue;
        if (!ref_frame) ref_frame = &lf;
        const int w = lf.width, h = lf.height;
        struct RGB3 { float r, g, b; };
        auto sample = [&](int x, int y) -> RGB3 {
            const uint8_t* p = &lf.rgba[(std::clamp(y,0,h-1) * w + std::clamp(x,0,w-1)) * 4];
            return { p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f };
        };
        int stride = std::max(1, w / 8);
        for (int x = 0; x < w; x += stride) {
            auto ct = sample(x, 0);     r += ct.r; g += ct.g; b += ct.b; cnt++;
            auto cb = sample(x, h-1);   r += cb.r; g += cb.g; b += cb.b; cnt++;
        }
        stride = std::max(1, h / 8);
        for (int y = 0; y < h; y += stride) {
            auto cl = sample(0,   y);   r += cl.r; g += cl.g; b += cl.b; cnt++;
            auto cr = sample(w-1, y);   r += cr.r; g += cr.g; b += cr.b; cnt++;
        }
    }
    if (cnt == 0) return; // no contributing layers
    r /= cnt; g /= cnt; b /= cnt;
    r *= 1.1f; g *= 1.1f; b *= 1.1f; // slight boost

    const LayerFrame& bg = *ref_frame;
    const int h = bg.height;
    const float depth  = bg.depth_meters;
    const float qw     = bg.quad_width_meters;
    const float qh     = qw * (float)h / (float)bg.width;
    const float qy     = 0.0f; // canvas at eye level (app_space origin = HMD position)

    glUseProgram(m_flat_prog);
    glUniformMatrix4fv(m_flat_u_vp, 1, GL_FALSE, vp.data());
    glBindVertexArray(m_flat_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_flat_vbo);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    // Large fading fan: screen-colour glow that fills ~180° of the visible hemisphere.
    // Centre vertex (at screen world position) has full alpha; far corners fade to zero.
    // Per-vertex alpha is interpolated by the fragment shader for a smooth gradient.
    // NOT tied to canvas transform offsets — always anchored to the raw screen world pos.
    const float far = 40.0f;         // extend 40 m → covers ~87° from centre at 2 m depth
    const float base_alpha = 0.38f;  // peak glow strength at screen centre
    glUniform4f(m_flat_u_color, r, g, b, 1.0f);  // rgb colour; alpha comes from per-vertex
    auto draw_fan = [&](AmbilightPlacement placement) {
        float verts[48]{};
        const bool horizontal = placement != AmbilightPlacement::Screen;
        const float cx = 0.0f;
        const float cy = placement == AmbilightPlacement::Floor ? qy - qh * 0.5f
                         : placement == AmbilightPlacement::Ceiling ? qy + qh * 0.5f : qy;
        const float cz = -depth;
        auto put = [&](int n, float x, float y, float z, float a) {
            verts[n * 4 + 0] = x; verts[n * 4 + 1] = y; verts[n * 4 + 2] = z; verts[n * 4 + 3] = a;
        };
        put(0, cx, cy, cz, base_alpha);
        if (!horizontal) {
            put(1, cx-far, cy+far, cz, 0); put(2, cx+far, cy+far, cz, 0);
            put(3, cx, cy, cz, base_alpha); put(4, cx+far, cy+far, cz, 0); put(5, cx+far, cy-far, cz, 0);
            put(6, cx, cy, cz, base_alpha); put(7, cx+far, cy-far, cz, 0); put(8, cx-far, cy-far, cz, 0);
            put(9, cx, cy, cz, base_alpha); put(10, cx-far, cy-far, cz, 0); put(11, cx-far, cy+far, cz, 0);
        } else {
            const float sy = (placement == AmbilightPlacement::Floor) ? -1.0f : 1.0f;
            put(1, cx-far, cy, cz, 0); put(2, cx+far, cy, cz, 0);
            put(3, cx, cy, cz, base_alpha); put(4, cx+far, cy, cz, 0); put(5, cx+far, cy, cz+sy*far, 0);
            put(6, cx, cy, cz, base_alpha); put(7, cx+far, cy, cz+sy*far, 0); put(8, cx-far, cy, cz+sy*far, 0);
            put(9, cx, cy, cz, base_alpha); put(10, cx-far, cy, cz+sy*far, 0); put(11, cx-far, cy, cz, 0);
        }
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glDrawArrays(GL_TRIANGLES, 0, 12);
    };
    if (state.ambilight_placement == AmbilightPlacement::All) {
        draw_fan(AmbilightPlacement::Screen);
        draw_fan(AmbilightPlacement::Floor);
        draw_fan(AmbilightPlacement::Ceiling);
    } else {
        draw_fan(state.ambilight_placement);
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// Shadow helper — dark gradient below each layer
// ---------------------------------------------------------------------------

void GlesRenderer::draw_shadow(int layer_index, const LayerFrame& frame, const Mat4& vp, const VrState& /*state*/) {
    if (m_ui_prog == 0 || layer_index < 0 || layer_index >= (int)m_layers.size() ||
        !m_layers[layer_index].tex || frame.width <= 0) return;
    const float qw  = frame.quad_width_meters;
    const float qh  = qw * (float)frame.height / (float)frame.width;
    const float qy  = 0.0f; // eye level
    const float z   = -(frame.depth_meters + 0.01f); // just behind
    const float bot = qy - qh * 0.5f;
    const float w   = qw * 1.05f;
    const float sh  = qh * 0.18f;

    Mat4 model = Mat4::identity();
    model.m[0] = w;
    model.m[5] = sh;
    model.m[12] = 0.0f;
    model.m[13] = bot - sh * 0.5f;
    model.m[14] = z;

    glUseProgram(m_ui_prog);
    glUniformMatrix4fv(m_ui_u_vp, 1, GL_FALSE, vp.data());
    glUniformMatrix4fv(m_ui_u_model, 1, GL_FALSE, model.data());
    glUniform1i(m_ui_u_texture, 0);
    glUniform1f(m_ui_u_alpha, 1.0f);
    glUniform1i(m_ui_u_shadow_mode, 1);
    glUniform4f(m_ui_u_shadow_color, 0.0f, 0.0f, 0.0f, 0.28f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_layers[layer_index].tex);
    glBindVertexArray(m_ui_vao);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
}

void GlesRenderer::update_library_preview_layer_texture(int layer, const std::vector<uint8_t>& rgba,
                                                        int width, int height) {
    if (layer < 0 || layer >= k_max_library_preview_layers ||
        width <= 0 || height <= 0 || rgba.empty()) return;
    GLuint& tex = m_library_preview_layer_tex[layer];
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GlesRenderer::clear_library_preview_layers() {
    for (int l = 0; l < k_max_library_preview_layers; ++l) {
        GLuint& tex = m_library_preview_layer_tex[l];
        if (tex) { glDeleteTextures(1, &tex); tex = 0; }
    }
}

// ---------------------------------------------------------------------------
// draw_panel — world-space textured quad for the ROM browser
// ---------------------------------------------------------------------------
void GlesRenderer::draw_panel(const OverlayInfo& ov, const Mat4& vp, float eye_x, float eye_y, float eye_z) {
    if (!m_ui_prog || ov.panel_count == 0) return;

    glUseProgram(m_ui_prog);
    glUniform1i(m_ui_u_shadow_mode, 0);
    glUniformMatrix4fv(m_ui_u_vp, 1, GL_FALSE, vp.data());
    glUniform1i(m_ui_u_texture, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glBindVertexArray(m_ui_vao);
    glActiveTexture(GL_TEXTURE0);

    // Overlay panels are alpha-blended with depth writes off, so whichever
    // one is drawn *last* wins wherever two overlap on screen — with no
    // sorting that was purely array order (e.g. a shelf card's row/column
    // index), so a farther card could paint over a nearer one (like the
    // enlarged, pulled-forward live-preview card) just because it came
    // later in the list. Sort back-to-front by real distance from the eye
    // so painter's algorithm draws the closest panel last/on top, matching
    // what should actually be visible.
    static thread_local std::vector<int> order;
    order.clear();
    order.reserve(ov.panel_count);
    for (int pi = 0; pi < ov.panel_count; ++pi) {
        if (ov.panels[pi].tex) order.push_back(pi);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        // PanelInfo::draw_order wins outright: distance is the right
        // heuristic for panels scattered around the room, but not for one
        // standing out of another off its centre (see PanelInfo).
        const int oa = ov.panels[a].draw_order;
        const int ob = ov.panels[b].draw_order;
        if (oa != ob) return oa < ob; // lower draws first, so higher lands on top
        const XrVector3f& pa = ov.panels[a].pose.position;
        const XrVector3f& pb = ov.panels[b].pose.position;
        const float da = (pa.x-eye_x)*(pa.x-eye_x) + (pa.y-eye_y)*(pa.y-eye_y) + (pa.z-eye_z)*(pa.z-eye_z);
        const float db = (pb.x-eye_x)*(pb.x-eye_x) + (pb.y-eye_y)*(pb.y-eye_y) + (pb.z-eye_z)*(pb.z-eye_z);
        return da > db; // farthest first
    });

    for (int pi : order) {
        const PanelInfo& p = ov.panels[pi];

        Mat4 scale;
        scale.m[0] = p.w;
        scale.m[5] = p.h;
        const Mat4 model = Mat4::mul(Mat4::from_pose(p.pose), scale);
        glUniformMatrix4fv(m_ui_u_model, 1, GL_FALSE, model.data());
        glUniform1f(m_ui_u_alpha, p.alpha);
        glBindTexture(GL_TEXTURE_2D, p.tex);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    // Draw highlight overlay quad on top of the panel
    if (ov.highlight.panel_idx >= 0 && ov.highlight.panel_idx < ov.panel_count) {
        const auto& hl = ov.highlight;
        const PanelInfo& pd = ov.panels[hl.panel_idx];
        if (pd.tex) {
            const XrQuaternionf& q = pd.pose.orientation;
            const XrVector3f&    P = pd.pose.position;

            // Panel right and up vectors
            XrVector3f right;
            right.x = 1.0f - 2.0f*(q.y*q.y + q.z*q.z);
            right.y = 2.0f*(q.x*q.y + q.w*q.z);
            right.z = 2.0f*(q.x*q.z - q.w*q.y);
            XrVector3f up;
            up.x = 2.0f*(q.x*q.y - q.w*q.z);
            up.y = 1.0f - 2.0f*(q.x*q.x + q.z*q.z);
            up.z = 2.0f*(q.y*q.z + q.w*q.x);

            // Highlight quad corners in world space (UV mapped to panel)
            float hw = pd.w * 0.5f;
            float hh = pd.h * 0.5f;
            float x0 = P.x + right.x * (-hw + hl.u0 * pd.w) + up.x * (hh - hl.v0 * pd.h);
            float y0 = P.y + right.y * (-hw + hl.u0 * pd.w) + up.y * (hh - hl.v0 * pd.h);
            float z0 = P.z + right.z * (-hw + hl.u0 * pd.w) + up.z * (hh - hl.v0 * pd.h);
            float x1 = P.x + right.x * (-hw + hl.u1 * pd.w) + up.x * (hh - hl.v0 * pd.h);
            float y1 = P.y + right.y * (-hw + hl.u1 * pd.w) + up.y * (hh - hl.v0 * pd.h);
            float z1 = P.z + right.z * (-hw + hl.u1 * pd.w) + up.z * (hh - hl.v0 * pd.h);
            float x2 = P.x + right.x * (-hw + hl.u0 * pd.w) + up.x * (hh - hl.v1 * pd.h);
            float y2 = P.y + right.y * (-hw + hl.u0 * pd.w) + up.y * (hh - hl.v1 * pd.h);
            float z2 = P.z + right.z * (-hw + hl.u0 * pd.w) + up.z * (hh - hl.v1 * pd.h);
            float x3 = P.x + right.x * (-hw + hl.u1 * pd.w) + up.x * (hh - hl.v1 * pd.h);
            float y3 = P.y + right.y * (-hw + hl.u1 * pd.w) + up.y * (hh - hl.v1 * pd.h);
            float z3 = P.z + right.z * (-hw + hl.u1 * pd.w) + up.z * (hh - hl.v1 * pd.h);

            // Push slightly toward viewer to avoid z-fighting
            XrVector3f normal;
            normal.x = 2.0f*(q.x*q.z + q.w*q.y);
            normal.y = 2.0f*(q.y*q.z - q.w*q.x);
            normal.z = 1.0f - 2.0f*(q.x*q.x + q.y*q.y);
            constexpr float k_push = 0.001f;
            x0 += normal.x * k_push; y0 += normal.y * k_push; z0 += normal.z * k_push;
            x1 += normal.x * k_push; y1 += normal.y * k_push; z1 += normal.z * k_push;
            x2 += normal.x * k_push; y2 += normal.y * k_push; z2 += normal.z * k_push;
            x3 += normal.x * k_push; y3 += normal.y * k_push; z3 += normal.z * k_push;

            float verts[] = {
                x0, y0, z0,  1.0f,
                x1, y1, z1,  1.0f,
                x2, y2, z2,  1.0f,
                x2, y2, z2,  1.0f,
                x1, y1, z1,  1.0f,
                x3, y3, z3,  1.0f,
            };

            // Use flat program for highlight
            glUseProgram(m_flat_prog);
            glUniformMatrix4fv(m_flat_u_vp, 1, GL_FALSE, vp.data());
            glBindVertexArray(m_flat_vao);
            glBindBuffer(GL_ARRAY_BUFFER, m_flat_vbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
            glUniform4f(m_flat_u_color, hl.r, hl.g, hl.b, hl.alpha);
            glDrawArrays(GL_TRIANGLES, 0, 6);

            // Restore UI program state
            glUseProgram(m_ui_prog);
            glUniformMatrix4fv(m_ui_u_vp, 1, GL_FALSE, vp.data());
            glBindVertexArray(m_ui_vao);
        }
    }

    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GlesRenderer::draw_live_layer_guides(const OverlayInfo& ov, const Mat4& vp) {
    if (!m_flat_prog || !m_flat_vao || !m_flat_vbo || ov.live_layer_guides.empty()) return;

    glUseProgram(m_flat_prog);
    glUniformMatrix4fv(m_flat_u_vp, 1, GL_FALSE, vp.data());
    glBindVertexArray(m_flat_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_flat_vbo);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    for (const LayerCanvasGuide& guide : ov.live_layer_guides) {
        if (guide.width <= 0.0f || guide.height <= 0.0f || guide.alpha <= 0.001f) continue;
        const float hw = guide.width * 0.5f;
        const float hh = guide.height * 0.5f;
        auto point = [&](float x, float y) {
            return XrVector3f{
                guide.center.x + guide.right.x * x + guide.up.x * y + guide.normal.x * 0.0015f,
                guide.center.y + guide.right.y * x + guide.up.y * y + guide.normal.y * 0.0015f,
                guide.center.z + guide.right.z * x + guide.up.z * y + guide.normal.z * 0.0015f,
            };
        };
        const XrVector3f tl = point(-hw, hh);
        const XrVector3f tr = point( hw, hh);
        const XrVector3f bl = point(-hw,-hh);
        const XrVector3f br = point( hw,-hh);
        const float verts[] = {
            tl.x, tl.y, tl.z, 1.0f,
            tr.x, tr.y, tr.z, 1.0f,
            bl.x, bl.y, bl.z, 1.0f,
            bl.x, bl.y, bl.z, 1.0f,
            tr.x, tr.y, tr.z, 1.0f,
            br.x, br.y, br.z, 1.0f,
        };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glUniform4f(m_flat_u_color, guide.r, guide.g, guide.b, guide.alpha);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

// ---------------------------------------------------------------------------
// draw_laser — bright billboard quad from controller to hit point
// ---------------------------------------------------------------------------
void GlesRenderer::draw_laser(const OverlayInfo& ov, const Mat4& vp) {
    if (!m_flat_prog || !ov.show_laser) return;

    const XrVector3f& O = ov.laser_origin;
    const XrVector3f& E = ov.laser_end;
    const XrVector3f& eye = ov.laser_eye;

    // Laser direction
    float dx = E.x - O.x, dy = E.y - O.y, dz = E.z - O.z;
    float len = sqrtf(dx*dx + dy*dy + dz*dz);
    if (len < 0.001f) return;

    // Billboard: perpendicular to laser dir and to (eye - midpoint)
    float mx = (O.x + E.x) * 0.5f;
    float my = (O.y + E.y) * 0.5f;
    float mz = (O.z + E.z) * 0.5f;
    float ex = eye.x - mx, ey = eye.y - my, ez = eye.z - mz;
    float el = sqrtf(ex*ex + ey*ey + ez*ez);
    if (el < 0.001f) { ex = 0; ey = 1; ez = 0; } else { ex /= el; ey /= el; ez /= el; }

    // cross(laser_dir_normalised, eye_dir)
    float lx = dx/len, ly = dy/len, lz = dz/len;
    float px = ly*ez - lz*ey;
    float py = lz*ex - lx*ez;
    float pz = lx*ey - ly*ex;
    float pl = sqrtf(px*px + py*py + pz*pz);
    if (pl < 0.001f) { px = 0; py = 1; pz = 0; } else { px /= pl; py /= pl; pz /= pl; }

    constexpr float hw = 0.004f; // half-width (4mm)
    px *= hw; py *= hw; pz *= hw;

    float verts[] = {
        O.x - px, O.y - py, O.z - pz, 1.f,
        O.x + px, O.y + py, O.z + pz, 1.f,
        E.x + px, E.y + py, E.z + pz, 1.f,
        E.x + px, E.y + py, E.z + pz, 1.f,
        E.x - px, E.y - py, E.z - pz, 1.f,
        O.x - px, O.y - py, O.z - pz, 1.f,
    };

    glUseProgram(m_flat_prog);
    glUniformMatrix4fv(m_flat_u_vp, 1, GL_FALSE, vp.data());
    glBindVertexArray(m_flat_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_flat_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    // Bright cyan beam
    glUniform4f(m_flat_u_color, 0.3f, 0.85f, 1.0f, 0.85f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// draw_laser2 — second edit-mode laser, drawn in green
// ---------------------------------------------------------------------------
void GlesRenderer::draw_laser2(const OverlayInfo& ov, const Mat4& vp) {
    if (!m_flat_prog || !ov.show_laser2) return;

    const XrVector3f& O = ov.laser2_origin;
    const XrVector3f& E = ov.laser2_end;

    float dx = E.x - O.x, dy = E.y - O.y, dz = E.z - O.z;
    float len = sqrtf(dx*dx + dy*dy + dz*dz);
    if (len < 0.001f) return;

    // Billboard using laser_eye from first laser (shared camera position)
    float mx = (O.x + E.x) * 0.5f;
    float my = (O.y + E.y) * 0.5f;
    float mz = (O.z + E.z) * 0.5f;
    float ex = ov.laser_eye.x - mx, ey = ov.laser_eye.y - my, ez = ov.laser_eye.z - mz;
    float el = sqrtf(ex*ex + ey*ey + ez*ez);
    if (el < 0.001f) { ex = 0; ey = 1; ez = 0; } else { ex /= el; ey /= el; ez /= el; }

    float lx = dx/len, ly = dy/len, lz = dz/len;
    float px = ly*ez - lz*ey;
    float py = lz*ex - lx*ez;
    float pz = lx*ey - ly*ex;
    float pl = sqrtf(px*px + py*py + pz*pz);
    if (pl < 0.001f) { px = 0; py = 1; pz = 0; } else { px /= pl; py /= pl; pz /= pl; }

    constexpr float hw = 0.004f;
    px *= hw; py *= hw; pz *= hw;

    float verts[] = {
        O.x - px, O.y - py, O.z - pz, 1.f,
        O.x + px, O.y + py, O.z + pz, 1.f,
        E.x + px, E.y + py, E.z + pz, 1.f,
        E.x + px, E.y + py, E.z + pz, 1.f,
        E.x - px, E.y - py, E.z - pz, 1.f,
        O.x - px, O.y - py, O.z - pz, 1.f,
    };

    glUseProgram(m_flat_prog);
    glUniformMatrix4fv(m_flat_u_vp, 1, GL_FALSE, vp.data());
    glBindVertexArray(m_flat_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_flat_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    // Bright green beam
    glUniform4f(m_flat_u_color, 0.2f, 1.0f, 0.3f, 0.85f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// draw_calibration_target — unobtrusive target on the rendered game surface
// ---------------------------------------------------------------------------
void GlesRenderer::draw_calibration_target(const OverlayInfo& ov, const Mat4& vp) {
    if (!m_flat_prog || !ov.show_calibration_target) return;

    constexpr int kSegments = 12;
    const float radius = std::max(0.006f, ov.calibration_target_radius);
    std::array<float, (kSegments * 2 + 4) * 4> verts{};
    int cursor = 0;
    auto point = [&](float x, float y) {
        const XrVector3f& c = ov.calibration_target_center;
        const XrVector3f& r = ov.calibration_target_right;
        const XrVector3f& u = ov.calibration_target_up;
        verts[cursor * 4 + 0] = c.x + r.x * x + u.x * y;
        verts[cursor * 4 + 1] = c.y + r.y * x + u.y * y;
        verts[cursor * 4 + 2] = c.z + r.z * x + u.z * y;
        verts[cursor * 4 + 3] = 1.0f;
        ++cursor;
    };
    for (int i = 0; i < kSegments; ++i) {
        const float a0 = (float)i * 6.28318530718f / (float)kSegments;
        const float a1 = (float)(i + 1) * 6.28318530718f / (float)kSegments;
        point(std::cos(a0) * radius, std::sin(a0) * radius);
        point(std::cos(a1) * radius, std::sin(a1) * radius);
    }
    point(-radius * 0.72f, 0.0f);
    point( radius * 0.72f, 0.0f);
    point(0.0f, -radius * 0.72f);
    point(0.0f,  radius * 0.72f);

    glUseProgram(m_flat_prog);
    glUniformMatrix4fv(m_flat_u_vp, 1, GL_FALSE, vp.data());
    glBindVertexArray(m_flat_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_flat_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (size_t)cursor * 4 * sizeof(float), verts.data());
    glUniform4f(m_flat_u_color, 1.0f, 0.72f, 0.12f, 0.95f);
    // This is a calibration guide, not scene geometry.  Keep it visible even
    // when the controller-attached gun mesh or another foreground layer is
    // between the eye and the target.
    const GLboolean depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);
    if (depth_was_enabled) glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDrawArrays(GL_LINES, 0, cursor);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    if (depth_was_enabled) glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
}

// Rotates a local-space offset into world space by a gun-pose orientation
// (no translation). Shared by draw_gun_model() and draw_scope_zoom() so the
// scope-tube geometry used for rendering and for the zoom-pass projection
// can never drift apart.
static XrVector3f gun_rotate_dir(const XrQuaternionf& q, float x, float y, float z) {
    const XrVector3f qv{ q.x, q.y, q.z };
    const XrVector3f v{ x, y, z };
    XrVector3f t;
    t.x = qv.y*v.z - qv.z*v.y + q.w*v.x;
    t.y = qv.z*v.x - qv.x*v.z + q.w*v.y;
    t.z = qv.x*v.y - qv.y*v.x + q.w*v.z;
    XrVector3f out;
    out.x = v.x + 2.0f*(qv.y*t.z - qv.z*t.y);
    out.y = v.y + 2.0f*(qv.z*t.x - qv.x*t.z);
    out.z = v.z + 2.0f*(qv.x*t.y - qv.y*t.x);
    return out;
}

// Local gun-space geometry of the scope-rifle's tube (case 2 in
// draw_gun_model) — shared with draw_scope_zoom() so the zoom effect lines
// up with what's actually drawn.
namespace {
constexpr XrVector3f kScopeRearLocal{ 0.0f, 0.052f, 0.025f };   // near-eye opening
constexpr XrVector3f kScopeFrontLocal{ 0.0f, 0.052f, -0.14f };  // far opening
constexpr float kScopeRadius = 0.024f;  // 2x diameter vs. the original 0.012 tube
}

// ---------------------------------------------------------------------------
// draw_gun_model — lightgun attached to the aiming controller. Model chosen
// by ov.gun_model (0=downloaded pistol, 1=restored low-poly pistol,
// 2=scope rifle; VrState::gun_model).
// Local gun space: origin at the controller grip, -Z = forward/barrel
// direction (matches the aim ray direction the shell raycasts with), +Y = up.
// ---------------------------------------------------------------------------
void GlesRenderer::draw_gun_model(const OverlayInfo& ov, const Mat4& vp, const XrPosef* pose_override) {
    // With an override this is player two's gun, gated by its own flag: player
    // one's may be hidden while player two's is visible.
    if (!m_gun_prog) return;
    if (!(pose_override ? ov.show_gun2 : ov.show_gun)) return;

    const XrPosef& gun_pose_used = pose_override ? *pose_override : ov.gun_pose;
    const XrVector3f& P = gun_pose_used.position;
    const XrQuaternionf& q = gun_pose_used.orientation;

    // Rotates a local-space offset into world space (gun-pose orientation only).
    auto rotate_dir = [&](float x, float y, float z) -> XrVector3f {
        return gun_rotate_dir(q, x, y, z);
    };
    auto rotate = [&](float x, float y, float z) -> XrVector3f {
        const XrVector3f d = rotate_dir(x, y, z);
        return { P.x + d.x, P.y + d.y, P.z + d.z };
    };
    auto add = [](const XrVector3f& a, const XrVector3f& b) -> XrVector3f {
        return { a.x + b.x, a.y + b.y, a.z + b.z };
    };
    auto scale = [](const XrVector3f& a, float s) -> XrVector3f {
        return { a.x * s, a.y * s, a.z * s };
    };
    auto normalize = [](XrVector3f v) -> XrVector3f {
        const float len = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
        if (len < 1e-6f) return { 0.0f, 0.0f, 1.0f };
        return { v.x / len, v.y / len, v.z / len };
    };
    auto cross = [](const XrVector3f& a, const XrVector3f& b) -> XrVector3f {
        return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
    };

    // Pushes a world-space triangle-pair quad with a shared world-space normal.
    auto push_quad_n = [](std::vector<float>& out, const XrVector3f& a, const XrVector3f& b,
                           const XrVector3f& c, const XrVector3f& d, const XrVector3f& n) {
        const XrVector3f* tri[6] = { &a, &b, &c, &c, &d, &a };
        for (auto* v : tri) {
            out.push_back(v->x); out.push_back(v->y); out.push_back(v->z);
            out.push_back(n.x);  out.push_back(n.y);  out.push_back(n.z);
        }
    };
    auto submit = [&](std::vector<float>& verts, float r, float g, float b) {
        glBindBuffer(GL_ARRAY_BUFFER, m_gun_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, verts.size() * sizeof(float), verts.data());
        glUniform4f(m_gun_u_color, r, g, b, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size() / 6));
    };

    // Faceted box (still axis-aligned, but now lit per-face so it reads as a
    // real volume rather than a flat silhouette).
    auto draw_box = [&](float cx, float cy, float cz, float hx, float hy, float hz,
                         float r, float g, float b) {
        const XrVector3f c000 = rotate(cx-hx, cy-hy, cz-hz);
        const XrVector3f c100 = rotate(cx+hx, cy-hy, cz-hz);
        const XrVector3f c010 = rotate(cx-hx, cy+hy, cz-hz);
        const XrVector3f c110 = rotate(cx+hx, cy+hy, cz-hz);
        const XrVector3f c001 = rotate(cx-hx, cy-hy, cz+hz);
        const XrVector3f c101 = rotate(cx+hx, cy-hy, cz+hz);
        const XrVector3f c011 = rotate(cx-hx, cy+hy, cz+hz);
        const XrVector3f c111 = rotate(cx+hx, cy+hy, cz+hz);
        const XrVector3f nnz = rotate_dir( 0, 0,-1), npz = rotate_dir(0, 0, 1);
        const XrVector3f nnx = rotate_dir(-1, 0, 0), npx = rotate_dir(1, 0, 0);
        const XrVector3f npy = rotate_dir( 0, 1, 0), nny = rotate_dir(0,-1, 0);

        std::vector<float> verts;
        verts.reserve(36 * 6);
        push_quad_n(verts, c000, c100, c110, c010, nnz);
        push_quad_n(verts, c101, c001, c011, c111, npz);
        push_quad_n(verts, c001, c000, c010, c011, nnx);
        push_quad_n(verts, c100, c101, c111, c110, npx);
        push_quad_n(verts, c010, c110, c111, c011, npy);
        push_quad_n(verts, c001, c101, c100, c000, nny);
        submit(verts, r, g, b);
    };

    // Extruded convex N-gon prism between local-space points p0/p1, with
    // independent cross-section radii at each end (equal = straight
    // cylinder, unequal = taper). Replaces sharp box corners with a rounded
    // low-poly silhouette (barrels, grips, stocks, scope tube) while staying
    // cheap — 6-12 sides, nowhere near a real perf concern on Quest.
    auto draw_prism = [&](float p0x, float p0y, float p0z, float p1x, float p1y, float p1z,
                           int sides, float r0, float r1, float r, float g, float b,
                           bool caps = true) {
        const XrVector3f p0{ p0x, p0y, p0z }, p1{ p1x, p1y, p1z };
        XrVector3f axis = normalize({ p1.x - p0.x, p1.y - p0.y, p1.z - p0.z });
        XrVector3f up{ 0.0f, 1.0f, 0.0f };
        if (std::fabs(axis.y) > 0.99f) up = { 1.0f, 0.0f, 0.0f };
        const XrVector3f u = normalize(cross(up, axis));
        const XrVector3f v = cross(axis, u);
        constexpr float kTau = 6.28318530718f;

        std::vector<float> verts;
        verts.reserve((size_t)sides * 6 * 3 * 4);
        for (int i = 0; i < sides; ++i) {
            const float a0 = kTau * (float)i / (float)sides;
            const float a1 = kTau * (float)(i + 1) / (float)sides;
            const XrVector3f dir0{ std::cos(a0), std::sin(a0), 0.0f };
            const XrVector3f dir1{ std::cos(a1), std::sin(a1), 0.0f };
            auto ring_local = [&](const XrVector3f& d) { return add(scale(u, d.x), scale(v, d.y)); };
            const XrVector3f rd0 = ring_local(dir0), rd1 = ring_local(dir1);

            const XrVector3f l00 = add(p0, scale(rd0, r0));
            const XrVector3f l01 = add(p0, scale(rd1, r0));
            const XrVector3f l10 = add(p1, scale(rd0, r1));
            const XrVector3f l11 = add(p1, scale(rd1, r1));
            const XrVector3f w00 = rotate(l00.x, l00.y, l00.z);
            const XrVector3f w01 = rotate(l01.x, l01.y, l01.z);
            const XrVector3f w10 = rotate(l10.x, l10.y, l10.z);
            const XrVector3f w11 = rotate(l11.x, l11.y, l11.z);

            const float amid = 0.5f * (a0 + a1);
            const XrVector3f nmid_local = ring_local({ std::cos(amid), std::sin(amid), 0.0f });
            const XrVector3f nmid = rotate_dir(nmid_local.x, nmid_local.y, nmid_local.z);
            push_quad_n(verts, w00, w01, w11, w10, nmid);

            // End caps (triangle fan from the axis center) — cheap and keeps
            // the barrel/grip tips from showing as open shells. Skipped when
            // caps=false (e.g. the scope tube, which needs to read as hollow
            // so you can see/aim through it instead of a solid rod).
            if (caps) {
                const XrVector3f center0 = rotate(p0.x, p0.y, p0.z);
                const XrVector3f center1 = rotate(p1.x, p1.y, p1.z);
                const XrVector3f ncap0 = rotate_dir(-axis.x, -axis.y, -axis.z);
                const XrVector3f ncap1 = rotate_dir(axis.x, axis.y, axis.z);
                push_quad_n(verts, center0, w01, w00, center0, ncap0);
                push_quad_n(verts, center1, w10, w11, center1, ncap1);
            }
        }
        submit(verts, r, g, b);
    };

    glUseProgram(m_gun_prog);
    glUniformMatrix4fv(m_gun_u_vp, 1, GL_FALSE, vp.data());
    // Procedural meshes already arrive in world space; the imported pistol is
    // local-space and receives the controller pose below.
    const Mat4 identity_model = Mat4::identity();
    glUniformMatrix4fv(m_gun_u_model, 1, GL_FALSE, identity_model.data());
    glUniform1f(m_gun_u_trigger, 0.0f);
    glUniform1f(m_gun_u_recoil, 0.0f);
    glUniform1f(m_gun_u_tilt, 0.0f);
    glUniform3f(m_gun_u_light_dir, -0.35f, 0.75f, 0.55f);
    glUniform1f(m_gun_u_light_ambient, 0.45f);
    glUniform1f(m_gun_u_vertex_color, 0.0f);
    glBindVertexArray(m_gun_vao);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);

    // Player two draws with its own model selection.
    const int gun_model_used = pose_override ? ov.gun2_model : ov.gun_model;
    switch (gun_model_used) {
    default:
    case 0: {
        // FireWarden's CC0 low-poly pistol, baked from the downloaded FBX.
        // The source's +Z barrel axis was converted to native -Z gun space.
        if (m_pistol_vao && m_pistol_vertex_count > 0) {
            glBindVertexArray(m_pistol_vao);
            // Player two draws at its own controller: gun_pose_used, not
            // ov.gun_pose, or both pistols land on player one's hand.
            const Mat4 pistol_model = Mat4::from_pose(gun_pose_used);
            glUniformMatrix4fv(m_gun_u_model, 1, GL_FALSE, pistol_model.data());
            // Each gun animates on its own trigger. Player two has no tilt
            // envelope of its own (that is the revolver swell, which is part
            // of player one's vibration-mode animation).
            const bool  trig   = pose_override ? ov.gun2_trigger : ov.gun_trigger;
            const float recoil = pose_override ? ov.gun2_recoil : ov.gun_recoil;
            glUniform1f(m_gun_u_trigger, trig ? 1.0f : 0.0f);
            glUniform1f(m_gun_u_recoil, std::clamp(recoil, 0.0f, 1.0f));
            glUniform1f(m_gun_u_tilt, pose_override ? 0.0f : ov.gun_tilt);
            glUniform1f(m_gun_u_vertex_color, 1.0f);
            glUniform4f(m_gun_u_color, 1.0f, 1.0f, 1.0f, 1.0f);
            glDrawArrays(GL_TRIANGLES, 0, m_pistol_vertex_count);
        }
        break;
    }
    case 1: {
        // Compact pistol: short rounded black slide, chrome muzzle, angled
        // octagonal grip. Rail-mounted reflex-sight reticle (open ring with a
        // crosshair inside) instead of iron sight posts — aim through the
        // ring at the cross, not by lining up two separate posts.
        draw_prism(0.0f, 0.0f, 0.015f, 0.0f, 0.0f, -0.085f, 8, 0.023f, 0.023f, 0.10f, 0.10f, 0.11f); // body/slide
        draw_prism(0.0f, 0.0f, -0.085f, 0.0f, 0.0f, -0.105f, 8, 0.023f, 0.014f,
                   ov.gun_muzzle_color[0], ov.gun_muzzle_color[1], ov.gun_muzzle_color[2]); // muzzle, re-tinted per shot
        draw_prism(0.0f, -0.005f, 0.02f, 0.0f, -0.085f, 0.055f, 8, 0.017f, 0.013f, 0.08f, 0.06f, 0.05f); // angled grip
        draw_box(0.0f, 0.028f, -0.06f, 0.003f, 0.005f, 0.003f, 0.10f, 0.10f, 0.11f);       // reticle mount riser
        draw_prism(0.0f, 0.036f, -0.056f, 0.0f, 0.036f, -0.060f, 12, 0.011f, 0.011f, 0.05f, 0.05f, 0.05f, false); // reticle ring (hollow)
        draw_box(0.0f, 0.036f, -0.058f, 0.011f, 0.0007f, 0.0007f, 0.95f, 0.15f, 0.10f);    // reticle cross, horizontal bar
        draw_box(0.0f, 0.036f, -0.058f, 0.0007f, 0.011f, 0.0007f, 0.95f, 0.15f, 0.10f);    // reticle cross, vertical bar
        break;
    }
    case 2: {
        // Scope rifle: long rounded barrel + stock, with a raised hollow
        // scope tube on top — open all the way through (no end caps) so you
        // can actually see/aim through it instead of a solid rod. The tube
        // always shows a 4x zoomed view, see draw_scope_zoom().
        draw_prism(0.0f, 0.0f, 0.03f, 0.0f, 0.0f, -0.212f, 8, 0.018f, 0.018f, 0.72f, 0.72f, 0.74f);  // long barrel/body
        // Muzzle band that heats up and cools down per shot. The rifle itself
        // never moves: the scope zoom quad is placed from the same fixed local
        // constants as the tube, so a recoil kick would desync the two.
        // Cold steel -> deep red -> bright orange, so the ramp reads as heat
        // rather than as an abrupt colour swap.
        const float heat = std::clamp(ov.gun_muzzle_heat, 0.0f, 1.0f);
        constexpr float kCold[3] = {0.72f, 0.72f, 0.74f};
        constexpr float kWarm[3] = {0.62f, 0.10f, 0.05f}; // dull red
        constexpr float kHot[3]  = {1.00f, 0.48f, 0.12f}; // glowing orange
        float mr, mg, mb;
        if (heat < 0.5f) {
            const float t = heat * 2.0f;
            mr = kCold[0] + (kWarm[0] - kCold[0]) * t;
            mg = kCold[1] + (kWarm[1] - kCold[1]) * t;
            mb = kCold[2] + (kWarm[2] - kCold[2]) * t;
        } else {
            const float t = (heat - 0.5f) * 2.0f;
            mr = kWarm[0] + (kHot[0] - kWarm[0]) * t;
            mg = kWarm[1] + (kHot[1] - kWarm[1]) * t;
            mb = kWarm[2] + (kHot[2] - kWarm[2]) * t;
        }
        draw_prism(0.0f, 0.0f, -0.212f, 0.0f, 0.0f, -0.23f, 8, 0.019f, 0.015f, mr, mg, mb);
        draw_prism(0.0f, -0.008f, 0.05f, 0.0f, -0.058f, 0.15f, 8, 0.026f, 0.020f, 0.30f, 0.20f, 0.13f); // stock
        draw_prism(0.0f, -0.005f, -0.01f, 0.0f, -0.095f, 0.06f, 8, 0.018f, 0.013f, 0.10f, 0.08f, 0.07f); // grip
        draw_prism(kScopeRearLocal.x, kScopeRearLocal.y, kScopeRearLocal.z,
                   kScopeFrontLocal.x, kScopeFrontLocal.y, kScopeFrontLocal.z,
                   10, kScopeRadius, kScopeRadius, 0.08f, 0.08f, 0.09f, false); // scope tube (hollow)
        draw_prism(0.0f, kScopeFrontLocal.y, kScopeFrontLocal.z, 0.0f, kScopeFrontLocal.y, kScopeFrontLocal.z - 0.015f,
                   10, kScopeRadius * 1.33f, kScopeRadius * 0.83f, 0.06f, 0.06f, 0.07f, false); // scope front bezel (hollow)
        draw_prism(0.0f, kScopeRearLocal.y, kScopeRearLocal.z, 0.0f, kScopeRearLocal.y, kScopeRearLocal.z + 0.02f,
                   10, kScopeRadius * 1.33f, kScopeRadius * 1.17f, 0.06f, 0.06f, 0.07f, false);  // scope rear bezel (hollow)
        break;
    }
    }

    glUniform1f(m_gun_u_vertex_color, 0.0f);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// Real controller models (XR_FB_render_model) -- load_controller_model()
// parses the GLB glTF buffer OpenXrShell::load_controller_render_models()
// got from the runtime; draw_controller_model() places and renders it each
// frame, with button/trigger/stick state nudging the matching named nodes.
//
// Node-name matching below is a first-pass best guess (Meta doesn't publish
// the controller glTF's node names anywhere offline) -- load_controller_model()
// logs every node name once via LOGI so the real names can be read from
// logcat on an actual headset and this matching tightened up if it's off.
// ---------------------------------------------------------------------------
static Mat4 controller_scale_mat(float sx, float sy, float sz) {
    Mat4 m = Mat4::identity();
    m.m[0] = sx; m.m[5] = sy; m.m[10] = sz;
    return m;
}

static Mat4 controller_translate_mat(float x, float y, float z) {
    Mat4 m = Mat4::identity();
    m.m[12] = x; m.m[13] = y; m.m[14] = z;
    return m;
}

// 180 deg rotation about the local Y (up) axis. The model is now anchored to
// the AIM pose (matching the real laser/lightgun), and on-device it renders
// exactly 180 deg backward from that -- a clean yaw flip, not the tilted
// nonsense the earlier grip-pose attempts produced. Applied between the pose
// and the model's own authored hierarchy so it flips the whole model in
// place around its own anchor instead of the world origin.
static Mat4 controller_yaw_flip_mat() {
    Mat4 m = Mat4::identity();
    m.m[0] = -1; m.m[10] = -1; // x' = -x, z' = -z, y unchanged
    return m;
}

// ---------------------------------------------------------------------------
// Tiny baked 5x7 bitmap font for the "LEFT+GRIP" press-feedback label --
// just the glyphs that string can ever need (hand names, button names, '+').
// Each entry is 7 rows, 5 bits/row (bit 4 = leftmost pixel). Laid out into a
// small atlas texture once in init_controller_label_font(); drawn as one
// textured quad per character via m_ui_prog (the same generic quad shader
// panels already use), no new shader needed.
// ---------------------------------------------------------------------------
struct ControllerFontGlyph { char c; uint8_t rows[7]; };
static const ControllerFontGlyph kControllerFont[] = {
    {'A', {0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}},
    {'B', {0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110}},
    {'C', {0b01111,0b10000,0b10000,0b10000,0b10000,0b10000,0b01111}},
    {'D', {0b11110,0b10001,0b10001,0b10001,0b10001,0b10001,0b11110}},
    {'E', {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111}},
    {'F', {0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b10000}},
    {'G', {0b01111,0b10000,0b10000,0b10111,0b10001,0b10001,0b01111}},
    {'H', {0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}},
    {'I', {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b11111}},
    {'K', {0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001}},
    {'L', {0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111}},
    {'N', {0b10001,0b11001,0b11001,0b10101,0b10011,0b10011,0b10001}},
    {'O', {0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}},
    {'P', {0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000}},
    {'R', {0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001}},
    {'S', {0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110}},
    {'T', {0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100}},
    {'U', {0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}},
    {'W', {0b10001,0b10001,0b10001,0b10101,0b10101,0b11011,0b11011}},
    {'X', {0b10001,0b10001,0b01010,0b00100,0b01010,0b10001,0b10001}},
    {'Y', {0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100}},
    {'+', {0b00000,0b00100,0b00100,0b11111,0b00100,0b00100,0b00000}},
};
static constexpr int kControllerFontCols = 6;
static constexpr int kControllerFontCellPx = 8; // 5x7 glyph padded into an 8x8 cell

static int controller_font_glyph_index(char c) {
    for (size_t i = 0; i < sizeof(kControllerFont) / sizeof(kControllerFont[0]); ++i)
        if (kControllerFont[i].c == c) return (int)i;
    return -1; // space / unknown -- caller just skips drawing a quad
}

void GlesRenderer::init_controller_label_font() {
    if (!m_ui_prog) return;
    const int glyph_count = (int)(sizeof(kControllerFont) / sizeof(kControllerFont[0]));
    const int rows = (glyph_count + kControllerFontCols - 1) / kControllerFontCols;
    const int atlas_w = kControllerFontCols * kControllerFontCellPx;
    const int atlas_h = rows * kControllerFontCellPx;
    std::vector<uint8_t> pixels(atlas_w * atlas_h * 4, 0); // RGBA, transparent black
    for (int gi = 0; gi < glyph_count; ++gi) {
        const int cx = (gi % kControllerFontCols) * kControllerFontCellPx;
        const int cy = (gi / kControllerFontCols) * kControllerFontCellPx;
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (!((kControllerFont[gi].rows[row] >> (4 - col)) & 1)) continue;
                const int px = cx + col, py = cy + row;
                uint8_t* p = &pixels[(py * atlas_w + px) * 4];
                p[0] = p[1] = p[2] = p[3] = 255; // opaque white
            }
        }
    }
    glGenTextures(1, &m_controller_label_tex);
    glBindTexture(GL_TEXTURE_2D, m_controller_label_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, atlas_w, atlas_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenVertexArrays(1, &m_controller_label_vao);
    glGenBuffers(1, &m_controller_label_vbo);
    glBindVertexArray(m_controller_label_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_controller_label_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
}

// Draws `text` as a left-aligned row of small quads billboarded to the
// controller's own local XY plane (i.e. rigidly attached via `anchor`, same
// as the body mesh -- not camera-facing, kept simple on purpose). One draw
// call per character, sampling that glyph's cell out of the shared atlas.
static void controller_draw_label(GLuint prog, GLint u_vp, GLint u_model, GLint u_texture, GLint u_alpha,
                                   GLint u_shadow_mode, GLuint vao, GLuint vbo, GLuint tex,
                                   const Mat4& vp, const Mat4& anchor, const std::string& text) {
    if (!prog || !vao || !tex || text.empty()) return;
    const float glyph_w = 0.012f, glyph_h = 0.0168f, advance = 0.014f;
    const float total_w = advance * (float)text.size();
    float x = -total_w * 0.5f + glyph_w * 0.5f;

    glUseProgram(prog);
    glUniformMatrix4fv(u_vp, 1, GL_FALSE, vp.data());
    glUniform1i(u_texture, 0);
    glUniform1f(u_alpha, 1.0f);
    glUniform1i(u_shadow_mode, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const float atlas_w = (float)(kControllerFontCols * kControllerFontCellPx);
    const float atlas_h = (float)(((sizeof(kControllerFont) / sizeof(kControllerFont[0])
                                     + kControllerFontCols - 1) / kControllerFontCols) * kControllerFontCellPx);
    for (char c : text) {
        const int gi = controller_font_glyph_index((char)std::toupper((unsigned char)c));
        if (gi >= 0) {
            const float cx = (float)((gi % kControllerFontCols) * kControllerFontCellPx);
            const float cy = (float)((gi / kControllerFontCols) * kControllerFontCellPx);
            const float u0 = cx / atlas_w, u1 = (cx + 5.0f) / atlas_w;
            const float v0 = cy / atlas_h, v1 = (cy + 7.0f) / atlas_h;
            const float verts[] = {
                x - glyph_w*0.5f,  glyph_h*0.5f, 0.0f,  u0, v0,
                x + glyph_w*0.5f,  glyph_h*0.5f, 0.0f,  u1, v0,
                x - glyph_w*0.5f, -glyph_h*0.5f, 0.0f,  u0, v1,
                x - glyph_w*0.5f, -glyph_h*0.5f, 0.0f,  u0, v1,
                x + glyph_w*0.5f,  glyph_h*0.5f, 0.0f,  u1, v0,
                x + glyph_w*0.5f, -glyph_h*0.5f, 0.0f,  u1, v1,
            };
            const Mat4 model = anchor; // glyph offset already baked into vertex x above
            glUniformMatrix4fv(u_model, 1, GL_FALSE, model.data());
            glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
        x += advance;
    }
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

static Mat4 controller_node_local_mat(const ControllerModelNode& n) {
    if (n.has_matrix) {
        Mat4 m;
        for (int i = 0; i < 16; ++i) m.m[i] = n.matrix[i];
        return m;
    }
    XrPosef pose;
    pose.orientation = { n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3] };
    pose.position     = { n.translation[0], n.translation[1], n.translation[2] };
    return Mat4::mul(Mat4::from_pose(pose), controller_scale_mat(n.scale[0], n.scale[1], n.scale[2]));
}

// Best-effort keyword match against a lowercased node name -- see the
// comment block above for why this isn't (yet) verified against real names.
static bool controller_name_has(const std::string& lower_name, const char* needle) {
    return lower_name.find(needle) != std::string::npos;
}

bool GlesRenderer::load_controller_model(int hand, const std::vector<uint8_t>& glb) {
    if (hand < 0 || hand > 1 || glb.empty()) return false;
    ControllerModel& out = m_controller_model[hand];
    out = ControllerModel{}; // reset any previous load for this hand

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse(&options, glb.data(), glb.size(), &data) != cgltf_result_success) {
        LOGE("cgltf_parse failed for %s controller model.", hand == 0 ? "left" : "right");
        return false;
    }
    if (cgltf_load_buffers(&options, data, nullptr) != cgltf_result_success) {
        LOGE("cgltf_load_buffers failed for %s controller model.", hand == 0 ? "left" : "right");
        cgltf_free(data);
        return false;
    }

    // One ControllerModelMesh per glTF primitive (not per cgltf_mesh) -- a
    // controller body mesh can carry more than one primitive when a part
    // (e.g. the black top plate) uses a different material than the rest of
    // the body, and only reading primitives[0] silently dropped those.
    std::vector<std::vector<int>> mesh_indices_for_cgltf_mesh(data->meshes_count);
    for (size_t mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& mesh = data->meshes[mi];
        for (size_t pi = 0; pi < mesh.primitives_count; ++pi) {
        const cgltf_primitive& prim = mesh.primitives[pi];
        if (prim.type != cgltf_primitive_type_triangles || !prim.indices) continue;

        const cgltf_accessor* pos_acc = nullptr;
        const cgltf_accessor* nrm_acc = nullptr;
        const cgltf_accessor* uv_acc  = nullptr;
        const cgltf_accessor* color_acc = nullptr;
        for (size_t ai = 0; ai < prim.attributes_count; ++ai) {
            if (prim.attributes[ai].type == cgltf_attribute_type_position) pos_acc = prim.attributes[ai].data;
            else if (prim.attributes[ai].type == cgltf_attribute_type_normal) nrm_acc = prim.attributes[ai].data;
            else if (prim.attributes[ai].type == cgltf_attribute_type_texcoord && !uv_acc) uv_acc = prim.attributes[ai].data;
            else if (prim.attributes[ai].type == cgltf_attribute_type_color && !color_acc) color_acc = prim.attributes[ai].data;
        }
        if (!pos_acc) continue;
        const cgltf_size vertex_count = pos_acc->count;

        std::vector<float> verts;
        verts.reserve(vertex_count * 12);
        for (cgltf_size vi = 0; vi < vertex_count; ++vi) {
            float pos[3] = {0, 0, 0};
            cgltf_accessor_read_float(pos_acc, vi, pos, 3);
            float nrm[3] = {0, 1, 0};
            if (nrm_acc) cgltf_accessor_read_float(nrm_acc, vi, nrm, 3);
            float uv[2] = {0, 0};
            if (uv_acc) cgltf_accessor_read_float(uv_acc, vi, uv, 2);
            float col[4] = {1, 1, 1, 1};
            if (color_acc) cgltf_accessor_read_float(color_acc, vi, col, 4);
            verts.push_back(pos[0]); verts.push_back(pos[1]); verts.push_back(pos[2]);
            verts.push_back(nrm[0]); verts.push_back(nrm[1]); verts.push_back(nrm[2]);
            verts.push_back(uv[0]);  verts.push_back(uv[1]);
            verts.push_back(col[0]); verts.push_back(col[1]); verts.push_back(col[2]); verts.push_back(col[3]);
        }

        std::vector<uint32_t> indices(prim.indices->count);
        for (cgltf_size ii = 0; ii < prim.indices->count; ++ii)
            indices[ii] = (uint32_t)cgltf_accessor_read_index(prim.indices, ii);

        // The real controller's dark-gray/black look (including the black
        // top plate) comes from a texture painted onto this single mesh --
        // there's no separate "plate" primitive/material to split out, so
        // sampling the actual base-color texture (decoded below) is the only
        // way to recover it. base_color_factor alone would render solid
        // white/gray (it's meant to multiply that texture, not stand in for
        // it), so it's still used only as a per-primitive tint/fallback.
        float mat_darkness = 1.0f; // 1 = full brightness, <1 = darker part (no texture case)
        if (prim.material && prim.material->has_pbr_metallic_roughness) {
            const float* f = prim.material->pbr_metallic_roughness.base_color_factor;
            mat_darkness = (f[0] + f[1] + f[2]) / 3.0f;
            if (mat_darkness < 0.05f) mat_darkness = 0.05f;
            if (mat_darkness > 1.0f) mat_darkness = 1.0f;
        }
        ControllerModelMesh gm;
        gm.base_color[0] = 0.16f * mat_darkness; gm.base_color[1] = 0.16f * mat_darkness; gm.base_color[2] = 0.18f * mat_darkness; gm.base_color[3] = 1.0f;

        // Decode the base-color texture straight out of the GLB's embedded
        // buffer (cgltf_load_buffers already resolved buffer->data; render
        // models ship their image as a bufferView, never an external uri).
        if (uv_acc && prim.material && prim.material->has_pbr_metallic_roughness) {
            const cgltf_texture_view& tv = prim.material->pbr_metallic_roughness.base_color_texture;
            const cgltf_image* img = tv.texture ? tv.texture->image : nullptr;
            if (img && img->buffer_view && img->buffer_view->buffer && img->buffer_view->buffer->data) {
                const unsigned char* img_bytes = (const unsigned char*)img->buffer_view->buffer->data + img->buffer_view->offset;
                const int img_size = (int)img->buffer_view->size;
                int w = 0, h = 0, comp = 0;
                unsigned char* pixels = stbi_load_from_memory(img_bytes, img_size, &w, &h, &comp, 4);
                if (pixels) {
                    glGenTextures(1, &gm.tex);
                    glBindTexture(GL_TEXTURE_2D, gm.tex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    gm.has_texture = true;
                    stbi_image_free(pixels);
                    LOGI("Controller model texture decoded: %dx%d comp=%d mesh=%s", w, h, comp, mesh.name ? mesh.name : "?");
                } else {
                    LOGE("Controller model texture decode failed: %s", stbi_failure_reason());
                }
            }
            // Meta ships this texture Basis-Universal-compressed
            // (KHR_texture_basisu: tv.texture->has_basisu, image() null) --
            // confirmed on-device, not decodable by stb_image. Falls back to
            // the flat gm.base_color fill above, which is the accepted look.
        }

        glGenVertexArrays(1, &gm.vao);
        glGenBuffers(1, &gm.vbo);
        glGenBuffers(1, &gm.ebo);
        glBindVertexArray(gm.vao);
        glBindBuffer(GL_ARRAY_BUFFER, gm.vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gm.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t), indices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 12 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 12 * sizeof(float), (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 12 * sizeof(float), (void*)(8 * sizeof(float)));
        glBindVertexArray(0);
        gm.index_count = (int)indices.size();

        mesh_indices_for_cgltf_mesh[mi].push_back((int)out.meshes.size());
        out.meshes.push_back(gm);
        }
    }

    // DFS from each scene root, assigning our own node indices so a parent
    // always lands at a lower index than its children -- draw_controller_model()
    // relies on that single-forward-pass ordering to accumulate world matrices.
    std::vector<const cgltf_node*> cgltf_node_for_index;
    std::function<void(const cgltf_node*, int)> visit = [&](const cgltf_node* node, int parent_idx) {
        ControllerModelNode n;
        n.name = node->name ? node->name : "";
        n.parent = parent_idx;
        if (node->has_translation) { n.translation[0]=node->translation[0]; n.translation[1]=node->translation[1]; n.translation[2]=node->translation[2]; }
        if (node->has_rotation)    { n.rotation[0]=node->rotation[0]; n.rotation[1]=node->rotation[1]; n.rotation[2]=node->rotation[2]; n.rotation[3]=node->rotation[3]; }
        if (node->has_scale)       { n.scale[0]=node->scale[0]; n.scale[1]=node->scale[1]; n.scale[2]=node->scale[2]; }
        if (node->has_matrix) {
            n.has_matrix = true;
            for (int mi = 0; mi < 16; ++mi) n.matrix[mi] = node->matrix[mi];
        }
        if (node->mesh) {
            const size_t mesh_idx = node->mesh - data->meshes;
            if (mesh_idx < mesh_indices_for_cgltf_mesh.size()) n.mesh_indices = mesh_indices_for_cgltf_mesh[mesh_idx];
        }
        // Confirmed on-device (logged via the dump below) against Meta's
        // actual Touch controller render model node names: b_trigger_front,
        // b_trigger_grip, b_thumbstick, and b_button_{a,b} (right) /
        // b_button_{x,y} (left). Exact match first (now that real names are
        // known); the substring fallback covers a future firmware/model
        // rename without silently going unmatched again.
        const std::string lower = [&]{ std::string s = n.name; std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }();
        n.is_world_ref = controller_name_has(lower, "_world");

        const int my_idx = (int)out.nodes.size();
        out.nodes.push_back(n);
        cgltf_node_for_index.push_back(node);

        if (out.node_trigger < 0 && lower == "b_trigger_front") out.node_trigger = my_idx;
        if (out.node_grip < 0 && lower == "b_trigger_grip") out.node_grip = my_idx;
        if (out.node_thumbstick < 0 && lower == "b_thumbstick") out.node_thumbstick = my_idx;
        if (out.node_button_a < 0 && (lower == "b_button_a" || lower == "b_button_x")) out.node_button_a = my_idx;
        if (out.node_button_b < 0 && (lower == "b_button_b" || lower == "b_button_y")) out.node_button_b = my_idx;
        if (out.node_trigger < 0 && controller_name_has(lower, "trigger_front")) out.node_trigger = my_idx;
        if (out.node_grip < 0 && controller_name_has(lower, "trigger_grip")) out.node_grip = my_idx;
        if (out.node_thumbstick < 0 && controller_name_has(lower, "thumbstick")) out.node_thumbstick = my_idx;
        if (out.node_button_a < 0 && (controller_name_has(lower, "button_a") || controller_name_has(lower, "button_x"))) out.node_button_a = my_idx;
        if (out.node_button_b < 0 && (controller_name_has(lower, "button_b") || controller_name_has(lower, "button_y"))) out.node_button_b = my_idx;

        for (cgltf_size ci = 0; ci < node->children_count; ++ci) visit(node->children[ci], my_idx);
    };

    if (data->scenes_count > 0) {
        const cgltf_scene& scene = data->scene ? *data->scene : data->scenes[0];
        for (cgltf_size ni = 0; ni < scene.nodes_count; ++ni) visit(scene.nodes[ni], -1);
    }

    LOGI("Loaded %s controller model: %d nodes, %d meshes (trigger=%d grip=%d thumbstick=%d btnA=%d btnB=%d)",
         hand == 0 ? "left" : "right", (int)out.nodes.size(), (int)out.meshes.size(),
         out.node_trigger, out.node_grip, out.node_thumbstick, out.node_button_a, out.node_button_b);
    for (size_t i = 0; i < out.nodes.size(); ++i) {
        const ControllerModelNode& dn = out.nodes[i];
        LOGI("  node[%d] '%s' parent=%d meshes=%d matrix=%d t=(%.4f,%.4f,%.4f) q=(%.3f,%.3f,%.3f,%.3f)",
             (int)i, dn.name.c_str(), dn.parent, (int)dn.mesh_indices.size(), (int)dn.has_matrix,
             dn.translation[0], dn.translation[1], dn.translation[2],
             dn.rotation[0], dn.rotation[1], dn.rotation[2], dn.rotation[3]);
    }
    // World-space (accumulated from root, before the grip pose is applied)
    // position of a few key nodes -- lets us work out the model's real
    // authored forward axis from actual numbers instead of guessing Euler
    // corrections again. left/right_laser_begin is expected to sit roughly
    // where the real controller's aim ray starts, i.e. out in front.
    {
        std::vector<Mat4> dbg_world(out.nodes.size());
        for (size_t i = 0; i < out.nodes.size(); ++i) {
            const Mat4 local = controller_node_local_mat(out.nodes[i]);
            const Mat4 parent_world = (out.nodes[i].parent < 0) ? Mat4::identity() : dbg_world[out.nodes[i].parent];
            dbg_world[i] = Mat4::mul(parent_world, local);
            LOGI("  world[%d] '%s' pos=(%.4f,%.4f,%.4f)", (int)i, out.nodes[i].name.c_str(),
                 dbg_world[i].m[12], dbg_world[i].m[13], dbg_world[i].m[14]);
        }
    }

    cgltf_free(data);
    out.loaded = !out.nodes.empty();
    return out.loaded;
}

void GlesRenderer::draw_controller_model(const OverlayInfo& ov, const Mat4& vp) {
    if (!m_controller_prog || !ov.show_controller_models) return;

    glUseProgram(m_controller_prog);
    glUniformMatrix4fv(m_controller_u_vp, 1, GL_FALSE, vp.data());
    glUniform3f(m_controller_u_light_dir, -0.35f, 0.75f, 0.55f);
    glUniform1f(m_controller_u_light_ambient, 0.45f);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(m_controller_u_tex, 0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);

    // (anchor node index, active?, marker color) per hand -- these nodes
    // carry no mesh of their own (confirmed via the node dump in
    // load_controller_model(): they're plain transform anchors, siblings of
    // the body mesh, not parent joints that would deform it), so a lit
    // marker sphere at each one's live position is how press feedback
    // actually reads on screen, instead of trying to move geometry that
    // isn't there.
    struct Marker { int node; bool active; float r, g, b; };

    for (int hand = 0; hand < 2; ++hand) {
        const ControllerModel& cm = m_controller_model[hand];
        if (!cm.loaded || !ov.controller_pose_valid[hand]) continue;

        // Anchored to the aim pose (see poll_actions() in openxr_shell.cpp)
        // plus a single 180 deg yaw correction -- see controller_yaw_flip_mat().
        const Mat4 grip_model = Mat4::mul(Mat4::from_pose(ov.controller_pose[hand]), controller_yaw_flip_mat());
        std::vector<Mat4> world(cm.nodes.size());
        for (size_t i = 0; i < cm.nodes.size(); ++i) {
            const ControllerModelNode& n = cm.nodes[i];
            // is_world_ref: drop this node's own baked rotation (see the
            // field comment) so its subtree -- every interactive anchor on
            // this asset -- shares the body mesh's frame instead of an
            // extra flip on top of it.
            const Mat4 local = n.is_world_ref
                ? controller_translate_mat(n.translation[0], n.translation[1], n.translation[2])
                : controller_node_local_mat(n);
            const Mat4 parent_world = (n.parent < 0) ? Mat4::identity() : world[n.parent];
            world[i] = Mat4::mul(parent_world, local);
        }

        for (size_t i = 0; i < cm.nodes.size(); ++i) {
            const ControllerModelNode& n = cm.nodes[i];
            if (n.mesh_indices.empty()) continue;
            const Mat4 final_model = Mat4::mul(grip_model, world[i]);
            glUniformMatrix4fv(m_controller_u_model, 1, GL_FALSE, final_model.data());
            for (int mesh_idx : n.mesh_indices) {
                const ControllerModelMesh& gm = cm.meshes[mesh_idx];
                glUniform4f(m_controller_u_color, gm.base_color[0], gm.base_color[1], gm.base_color[2], gm.base_color[3]);
                glUniform1f(m_controller_u_has_texture, gm.has_texture ? 1.0f : 0.0f);
                if (gm.has_texture) glBindTexture(GL_TEXTURE_2D, gm.tex);
                glBindVertexArray(gm.vao);
                glDrawElements(GL_TRIANGLES, gm.index_count, GL_UNSIGNED_INT, nullptr);
            }
        }

        // Per-button node positions come from this asset's own hierarchy,
        // which has a node partway up the button-anchor chain that doesn't
        // match the body-mesh branch. Keep press feedback attached to the
        // already-confirmed body transform instead of chasing that hierarchy.
        // The hand is visually obvious, so the label only names the active
        // control: e.g. "TRIGGER", "UP", or "CLICK".
        std::string label;
        bool any = false;
        auto add = [&](bool on, const char* name) {
            if (!on) return;
            if (any) label += "+";
            label += name; any = true;
        };
        add(ov.controller_trigger[hand] > 0.15f, "TRIGGER");
        add(ov.controller_grip[hand] > 0.15f, "GRIP");
        add(ov.controller_btn_a[hand], hand == 0 ? "X" : "A");
        add(ov.controller_btn_b[hand], hand == 0 ? "Y" : "B");
        constexpr float k_stick_label_threshold = 0.2f;
        add(ov.controller_stick_y[hand] >  k_stick_label_threshold, "UP");
        add(ov.controller_stick_y[hand] < -k_stick_label_threshold, "DOWN");
        add(ov.controller_stick_x[hand] < -k_stick_label_threshold, "LEFT");
        add(ov.controller_stick_x[hand] >  k_stick_label_threshold, "RIGHT");
        add(ov.controller_stick_click[hand], "CLICK");
        if (any) {
            // The controller body needs the yaw correction above, but that
            // correction mirrors the UI quad's local X axis. Flip only the
            // label geometry back so it reads left-to-right from the headset.
            const Mat4 label_local = Mat4::mul(
                controller_translate_mat(0.0f, 0.03f, -0.03f),
                controller_scale_mat(-1.0f, 1.0f, 1.0f));
            const Mat4 label_anchor = Mat4::mul(grip_model, label_local);
            controller_draw_label(m_ui_prog, m_ui_u_vp, m_ui_u_model, m_ui_u_texture, m_ui_u_alpha,
                                   m_ui_u_shadow_mode, m_controller_label_vao, m_controller_label_vbo,
                                   m_controller_label_tex, vp, label_anchor, label);
            glUseProgram(m_controller_prog); // controller_draw_label switched programs; restore for next hand
        }
    }
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// draw_scope_zoom — always-on 4x magnification for the scope-rifle's tube.
// Copies the already-rendered eye image to a texture, then composites a
// zoomed-in circular crop back over the spot where the scope's front lens
// projects on screen, so whatever's visible through the tube always reads
// as 4x zoomed in.
// ---------------------------------------------------------------------------
void GlesRenderer::draw_scope_zoom(const OverlayInfo& ov, const Mat4& vp, const EyeFbo& fbo,
                                    float hmd_x, float hmd_y, float hmd_z) {
    (void)hmd_x; (void)hmd_y; (void)hmd_z;
    if (!m_scope_zoom_prog || !ov.show_gun || ov.gun_model != 2) return;
    if (fbo.width <= 0 || fbo.height <= 0) return;

    const XrVector3f& P = ov.gun_pose.position;
    const XrQuaternionf& q = ov.gun_pose.orientation;
    auto world = [&](const XrVector3f& local) -> XrVector3f {
        const XrVector3f d = gun_rotate_dir(q, local.x, local.y, local.z);
        return { P.x + d.x, P.y + d.y, P.z + d.z };
    };
    auto sub = [](const XrVector3f& a, const XrVector3f& b) -> XrVector3f {
        return { a.x - b.x, a.y - b.y, a.z - b.z };
    };
    auto length = [](const XrVector3f& v) -> float {
        return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    };
    auto normalize = [&](XrVector3f v) -> XrVector3f {
        const float len = length(v);
        if (len < 1e-6f) return { 0.0f, 0.0f, 1.0f };
        return { v.x / len, v.y / len, v.z / len };
    };
    auto cross = [](const XrVector3f& a, const XrVector3f& b) -> XrVector3f {
        return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
    };

    const XrVector3f rearWorld  = world(kScopeRearLocal);
    const XrVector3f frontWorld = world(kScopeFrontLocal);
    const XrVector3f axis       = normalize(sub(frontWorld, rearWorld));

    XrVector3f up{ 0.0f, 1.0f, 0.0f };
    if (std::fabs(axis.y) > 0.99f) up = { 1.0f, 0.0f, 0.0f };
    const XrVector3f u = normalize(cross(up, axis));
    const XrVector3f rimWorld = { frontWorld.x + u.x*kScopeRadius,
                                   frontWorld.y + u.y*kScopeRadius,
                                   frontWorld.z + u.z*kScopeRadius };

    auto project_uv = [&](const XrVector3f& w, bool& valid) -> std::array<float, 2> {
        const float* m = vp.data();
        const float cx = m[0]*w.x + m[4]*w.y + m[8]*w.z  + m[12];
        const float cy = m[1]*w.x + m[5]*w.y + m[9]*w.z  + m[13];
        const float cw = m[3]*w.x + m[7]*w.y + m[11]*w.z + m[15];
        valid = cw > 1e-4f;
        if (!valid) return { 0.5f, 0.5f };
        return { (cx / cw) * 0.5f + 0.5f, (cy / cw) * 0.5f + 0.5f };
    };

    bool valid_c = false, valid_r = false;
    const auto center_uv = project_uv(frontWorld, valid_c);
    const auto rim_uv    = project_uv(rimWorld, valid_r);
    if (!valid_c || !valid_r) return;
    const float dux = rim_uv[0] - center_uv[0], duy = rim_uv[1] - center_uv[1];
    float radius_uv = std::sqrt(dux*dux + duy*duy);
    if (radius_uv < 0.001f) return;
    radius_uv = std::min(radius_uv, 0.35f);

    // Refresh the copy target if the eye resolution changed.
    if (m_scope_copy_tex == 0 || m_scope_copy_w != fbo.width || m_scope_copy_h != fbo.height) {
        if (m_scope_copy_tex) { glDeleteTextures(1, &m_scope_copy_tex); m_scope_copy_tex = 0; }
        if (m_scope_copy_fbo) { glDeleteFramebuffers(1, &m_scope_copy_fbo); m_scope_copy_fbo = 0; }
        glGenTextures(1, &m_scope_copy_tex);
        glBindTexture(GL_TEXTURE_2D, m_scope_copy_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fbo.width, fbo.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &m_scope_copy_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_scope_copy_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_scope_copy_tex, 0);
        m_scope_copy_w = fbo.width;
        m_scope_copy_h = fbo.height;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // Snapshot the just-rendered eye image (can't sample and write the same
    // attachment in the same pass) then composite the zoomed crop back over it.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_scope_copy_fbo);
    glBlitFramebuffer(0, 0, fbo.width, fbo.height, 0, 0, fbo.width, fbo.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
    glViewport(0, 0, fbo.width, fbo.height);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glUseProgram(m_scope_zoom_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_scope_copy_tex);
    glUniform1i(m_scope_u_tex, 0);
    glUniform2f(m_scope_u_center, center_uv[0], center_uv[1]);
    glUniform1f(m_scope_u_radius, radius_uv);
    glUniform1f(m_scope_u_zoom_inv, 0.5f); // 2x magnification
    glUniform1f(m_scope_u_aspect, (float)fbo.width / (float)fbo.height);
    glBindVertexArray(m_scope_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

void GlesRenderer::draw_sky_dome(const Mat4& view, const Mat4& proj, const SkyDomeInfo& info) {
    {
        static int sky_draw_log = 0;
        if (++sky_draw_log % 120 == 1) {
            LOGE("SKY_DBG draw_sky_dome: enabled=%d prog=%u vao=%u verts=%d mode=%d",
                 (int)info.enabled, m_sky_prog, m_sky_vao, m_sky_vertex_count, (int)info.mode);
        }
    }
    if (!info.enabled || !m_sky_prog || !m_sky_vao || m_sky_vertex_count <= 0) return;
    Mat4 view_rot = view;
    view_rot.m[12] = 0.0f;
    view_rot.m[13] = 0.0f;
    view_rot.m[14] = 0.0f;
    glUseProgram(m_sky_prog);
    glUniformMatrix4fv(m_sky_u_proj, 1, GL_FALSE, proj.data());
    glUniformMatrix4fv(m_sky_u_view, 1, GL_FALSE, view_rot.data());
    glUniform4fv(m_sky_u_bands, (GLsizei)info.bands.size(), info.bands[0].data());
    glUniform1i(m_sky_u_mode, info.opaque_override ? 4 :
                              info.mode == EnvironmentSphereMode::FullSphere ? 2 :
                              info.mode == EnvironmentSphereMode::Ground    ? 3 :
                              info.mode == EnvironmentSphereMode::SkyOnly   ? 1 : 0);
    glBindVertexArray(m_sky_vao);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glCullFace(GL_FRONT);
    glEnable(GL_CULL_FACE);
    glDrawArrays(GL_TRIANGLES, 0, m_sky_vertex_count);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// Render one eye
// ---------------------------------------------------------------------------

void GlesRenderer::render_eye(const EyeFbo& fbo,
                               const Mat4& view,
                               const Mat4& proj,
                               const std::vector<LayerFrame*>& frames,
                               const PsxDepthFrame* psx_depth,
                               const VrState& state,
                               float canvas_x,
                               float canvas_y,
                               float canvas_az,
                               float canvas_el,
                               float canvas_scale,
                               const OverlayInfo* overlay,
                               const SkyDomeInfo* sky_dome,
                               float bg_r,
                               float bg_g,
                               float bg_b,
                               float bg_a,
                               bool passthrough_alpha,
                               float parallax_yaw,
                               float parallax_pitch,
                               float hmd_x,
                               float hmd_y,
                               float hmd_z,
                               float fade_alpha,
                               const std::vector<int>& layer_deck_slots,
                               int layer_deck_slot_count,
                               float layer_deck_spread) {
    const bool layer_deck_active = !layer_deck_slots.empty() &&
        layer_deck_slots.size() == frames.size() && layer_deck_slot_count > 1;
    using Clock = std::chrono::steady_clock;
    const Mat4 vp = Mat4::mul(proj, view);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
    glViewport(0, 0, fbo.width, fbo.height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glClearColor(bg_r, bg_g, bg_b, bg_a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (sky_dome && sky_dome->enabled) {
        draw_sky_dome(view, proj, *sky_dome);
    }

    // PSX is its own presentation: one depth-displaced screen instead of the
    // layered canvas. It draws here, in the normal render_eye flow, so overlays,
    // UI panels and the laser still composite over it exactly as they do for
    // every other backend — the isolation is in what gets drawn, not in
    // bypassing the renderer.
    bool native_scene_drawn = false;
    if (psx_depth) {
        const LayerFrame* psx_frame = nullptr;
        for (const LayerFrame* frame : frames) {
            if (frame && frame->width > 0 && frame->height > 0 && !frame->rgba.empty()) {
                psx_frame = frame;
                break;
            }
        }
        // Hardware path: textures come straight from the emulator's context, so
        // nothing is uploaded here. Falls back to the CPU buffers when the
        // hardware renderer is not running.
        const PsxGpuFrame* gpu = qrd::psx_gpu_frame_acquire_latest(m_psx_gpu_frame)
                                     ? &m_psx_gpu_frame : nullptr;
        if (psx_frame || gpu) {
            static const LayerFrame kPlacementDefaults;
            const LayerFrame& placement = psx_frame ? *psx_frame : kPlacementDefaults;
            native_scene_drawn = draw_psx_screen(placement, psx_depth, gpu, view, proj,
                                                 canvas_x, canvas_y, canvas_az,
                                                 canvas_el, canvas_scale);
        }
    }

    float upload_ms_total = 0.0f;
    float draw_ms_total = 0.0f;
    int uploaded_layers = 0;

    // Game layers (skip if no program or no frames, but still draw overlay below)
    if (m_program && !frames.empty() && !native_scene_drawn) {

    // Ambilight behind everything
    if (state.ambilight) draw_ambilight(frames, vp, state);

    // Upload textures and render layers back-to-front
    resize_layers((int)frames.size());

    const bool immersive_active = (state.immersive_beta_enabled || state.permacurve) &&
                                  m_immersive_program != 0 &&
                                  m_curve_vao != 0 &&
                                  m_curve_vertex_count > 0;
    const GLuint layer_program = immersive_active ? m_immersive_program : m_program;
    const GLint u_vp           = immersive_active ? m_i_u_vp           : m_u_vp;
    const GLint u_depth        = immersive_active ? m_i_u_depth        : m_u_depth;
    const GLint u_quad_w       = immersive_active ? m_i_u_quad_w       : m_u_quad_w;
    const GLint u_quad_h       = immersive_active ? m_i_u_quad_h       : m_u_quad_h;
    const GLint u_rotate90     = immersive_active ? m_i_u_rotate90     : m_u_rotate90;
    const GLint u_table_mode   = immersive_active ? m_i_u_table_mode   : m_u_table_mode;
    const GLint u_quad_y       = immersive_active ? m_i_u_quad_y       : m_u_quad_y;
    const GLint u_roundness    = immersive_active ? m_i_u_roundness    : m_u_roundness;
    const GLint u_copy_count   = immersive_active ? m_i_u_copy_count   : m_u_copy_count;
    const GLint u_copy_span    = immersive_active ? m_i_u_copy_span    : m_u_copy_span;
    const GLint u_screen_curve = immersive_active ? m_i_u_screen_curve : m_u_screen_curve;
    const GLint u_upscale      = immersive_active ? m_i_u_upscale      : m_u_upscale;
    const GLint u_depthmap     = immersive_active ? m_i_u_depthmap     : m_u_depthmap;
    const GLint u_gamma        = immersive_active ? m_i_u_gamma        : m_u_gamma;
    const GLint u_contrast     = immersive_active ? m_i_u_contrast     : m_u_contrast;
    const GLint u_saturation   = immersive_active ? m_i_u_saturation   : m_u_saturation;
    const GLint u_brightness   = immersive_active ? m_i_u_brightness   : m_u_brightness;
    const GLint u_pixel_light  = immersive_active ? m_i_u_pixel_light  : m_u_pixel_light;
    const GLint u_light_dir    = immersive_active ? m_i_u_light_dir    : m_u_light_dir;
    const GLint u_light_ambient = immersive_active ? m_i_u_light_ambient : m_u_light_ambient;
    const GLint u_light_flicker = immersive_active ? m_i_u_light_flicker : m_u_light_flicker;
    const GLint u_fog_factor    = immersive_active ? m_i_u_fog_factor : m_u_fog_factor;
    const GLint u_fog_color     = immersive_active ? m_i_u_fog_color : m_u_fog_color;
    const GLint u_texture      = immersive_active ? m_i_u_texture      : m_u_texture;
    const GLint u_canvas_x     = immersive_active ? m_i_u_canvas_x     : m_u_canvas_x;
    const GLint u_canvas_y     = immersive_active ? m_i_u_canvas_y     : m_u_canvas_y;
    const GLint u_canvas_az    = immersive_active ? m_i_u_canvas_az    : m_u_canvas_az;
    const GLint u_layer_yaw    = immersive_active ? m_i_u_layer_yaw    : m_u_layer_yaw;
    const GLint u_canvas_el    = immersive_active ? m_i_u_canvas_el    : m_u_canvas_el;
    const GLint u_canvas_scale = immersive_active ? m_i_u_canvas_scale : m_u_canvas_scale;
    const GLint u_has_y_depth = immersive_active ? m_i_u_has_y_depth : m_u_has_y_depth;
    const GLint u_y_depth_spread = immersive_active ? m_i_u_y_depth_spread : m_u_y_depth_spread;
    const GLint u_solid_stack  = immersive_active ? m_i_u_solid_stack  : m_u_solid_stack;
    const GLint u_force_opaque_alpha = immersive_active ? m_i_u_force_opaque_alpha : m_u_force_opaque_alpha;
    const GLint u_bbox_mode    = immersive_active ? m_i_u_bbox_mode : m_u_bbox_mode;
    const GLint u_bbox_debug   = immersive_active ? m_i_u_bbox_debug : m_u_bbox_debug;
    const GLint u_subrect_enable = immersive_active ? m_i_u_subrect_enable : m_u_subrect_enable;
    const GLint u_subrect = immersive_active ? m_i_u_subrect : m_u_subrect;
    const GLint u_instance_base = immersive_active ? m_i_u_instance_base : m_u_instance_base;
    const GLint u_object_box_count = immersive_active ? m_i_u_object_box_count : m_u_object_box_count;
    const GLint u_allow_behind = immersive_active ? m_i_u_allow_behind : m_u_allow_behind;
    const GLint u_edge_lr = immersive_active ? m_i_u_edge_lr : m_u_edge_lr;
    const GLint u_edge_tb = immersive_active ? m_i_u_edge_tb : m_u_edge_tb;
    const GLint u_has_edge_profile = immersive_active ? m_i_u_has_edge_profile : m_u_has_edge_profile;
    const int layer_vertex_count = immersive_active ? m_curve_vertex_count : 6;

    // Integer-scale + pixel-grid snap (upscale mode only).
    // All layers share the same canvas_x/y/scale uniforms, so we snap once using
    // layer 0's pixel size as the reference grid.  The goal:
    //   • canvas_scale is rounded to the nearest integer pixel multiplier
    //     (x1, x2, x3 … — no fractional scales that blur GL_NEAREST output)
    //   • canvas_x/y are snapped to the nearest whole source-pixel boundary
    //     so the quad never sits between pixels during head movement
    // Table Mode shrinks the canvas to cabinet scale. The normal canvas is
    // ~2.56m wide, which reads fine as a screen across the room but becomes a
    // dining table you are standing in the middle of once laid flat. Folded
    // into canvas_scale so every dependent term (quad size, footprint,
    // subrect offsets) scales together; Zoom still works on top of it.
    constexpr float kTableFit = 0.42f;
    float snapped_canvas_x     = canvas_x;
    float snapped_canvas_y     = canvas_y;
    float snapped_canvas_scale = canvas_scale * (state.surface_mode != 0 ? kTableFit : 1.0f);
    if (state.upscale_mode != UpscaleMode::Off) {
        // Find first valid frame to derive the pixel-size grid.
        for (const LayerFrame* fp : frames) {
            if (!fp || fp->width <= 0 || fp->quad_width_meters <= 0.0f) continue;
            // px_size: physical width of one source pixel in metres
            const float px_size = fp->quad_width_meters / (float)fp->width;
            // Snap canvas_scale: round to nearest integer multiplier.
            // scale * qw / qw = scale; integer multiplier = round(scale)
            // Guard against round-to-zero.
            const float int_scale = std::max(1.0f, std::round(canvas_scale));
            snapped_canvas_scale = int_scale * (state.surface_mode != 0 ? kTableFit : 1.0f);
            // Snap canvas_x/y to multiples of the scaled pixel size.
            const float grid = px_size * int_scale;
            if (grid > 0.0f) {
                snapped_canvas_x = std::round(canvas_x / grid) * grid;
                snapped_canvas_y = std::round(canvas_y / grid) * grid;
            }
            break;
        }
    }

    glUseProgram(layer_program);
    glUniformMatrix4fv(u_vp, 1, GL_FALSE, vp.data());
    glUniform1f(u_upscale,      (float)(int)state.upscale_mode);
    glUniform1f(u_depthmap,     state.depth_mode == DepthMode::WholeLayer ? 1.0f : 0.0f);
    glUniform1f(u_gamma,        state.gamma);
    glUniform1f(u_contrast,     state.contrast);
    glUniform1f(u_saturation,   state.saturation);
    glUniform1f(u_brightness,   state.brightness);
    if (u_pixel_light >= 0) glUniform1f(u_pixel_light, is_pixel_geometry_mode(state.depth_mode) && state.depth_mode == DepthMode::PixelFx ? 1.0f : 0.0f);
    if (u_light_dir >= 0) glUniform3f(u_light_dir, -0.35f, 0.65f, 0.68f);
    if (u_light_ambient >= 0) glUniform1f(u_light_ambient, 0.30f);
    const double now_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const float light_flicker = 1.0f + 0.035f * std::sinf((float)now_seconds * 7.0f)
                                      + 0.018f * std::sinf((float)now_seconds * 13.7f);
    if (u_light_flicker >= 0) glUniform1f(u_light_flicker, light_flicker);
    if (u_fog_factor >= 0) glUniform1f(u_fog_factor, 0.0f);
    if (u_fog_color >= 0) glUniform3f(u_fog_color, 0.015f, 0.020f, 0.040f);
    glUniform1f(u_roundness,    0.0f);
    glUniform1f(u_screen_curve, immersive_active ? state.screen_curve : 0.0f);
    glUniform1f(u_force_opaque_alpha, passthrough_alpha ? 1.0f : 0.0f);
    if (immersive_active) {
        glUniform1f(m_i_u_tilt_x, state.tilt_x);
        glUniform1f(m_i_u_tilt_y, state.tilt_y);
    }
    glUniform1f(u_solid_stack,  0.0f);
    glUniform1f(u_canvas_x,     snapped_canvas_x);
    glUniform1f(u_canvas_y,     snapped_canvas_y);
    glUniform1f(u_canvas_az,    canvas_az);
    // Neutral by default; the per-layer draw below sets the real bookshelf
    // yaw. GL uniform state persists on the program, so this must be cleared
    // here or a stale deck yaw bleeds into a later non-deck frame.
    if (u_layer_yaw >= 0) glUniform1f(u_layer_yaw, 0.0f);
    glUniform1f(u_canvas_el,    canvas_el);
    glUniform1f(u_canvas_scale, snapped_canvas_scale);
    glUniform1i(u_texture, 0);
    glUniform1f(u_bbox_mode, 0.0f);
    glUniform1f(u_bbox_debug, 0.0f);
    glUniform1f(u_subrect_enable, 0.0f);
    glUniform4f(u_subrect, 0.0f, 0.0f, 1.0f, 1.0f);
    glUniform1f(u_instance_base, 0.0f);
    glUniform1i(u_object_box_count, 0);
    glUniform1f(u_allow_behind, 0.0f);
    if (u_has_edge_profile >= 0) glUniform1f(u_has_edge_profile, 0.0f);
    if (u_edge_lr >= 0) glUniform1i(u_edge_lr, 2);
    if (u_edge_tb >= 0) glUniform1i(u_edge_tb, 3);

    // Sprite Y-depth uniforms default off. ZBUF uses the same dense mesh path, but with
    // the emulator's per-pixel map instead of the optional sprite map.
    if (u_has_y_depth >= 0) {
        glUniform1i(u_has_y_depth, 0);
        if (u_y_depth_spread >= 0)
            glUniform1f(u_y_depth_spread, state.sprite_y_depth_spread);
    }

    glBindVertexArray(immersive_active ? m_curve_vao : m_vao);
    glActiveTexture(GL_TEXTURE0);

    // Parallax peek: compute depth range once for per-layer az/el offsets.
    float parallax_min_d = 1e9f, parallax_depth_range = 0.0f;
    if (parallax_yaw != 0.0f || parallax_pitch != 0.0f) {
        float max_d = -1e9f;
        for (const LayerFrame* fp : frames) {
            if (!fp) continue;
            parallax_min_d = std::min(parallax_min_d, fp->depth_meters);
            max_d          = std::max(max_d,           fp->depth_meters);
        }
        if (max_d > parallax_min_d + 0.001f)
            parallax_depth_range = max_d - parallax_min_d;
    }

    // Table Mode stacking range: nearest layer sits at the table (t=0),
    // farthest rises toward the ceiling (t=1). Reuses the same frame-wide
    // depth scan as the parallax range above, just gated separately since
    // parallax_min_d/max_d are only populated when state.parallax_ratio > 0.
    float table_min_d = 1e9f, table_max_d = -1e9f;
    if (state.surface_mode != 0) {
        for (const LayerFrame* fp : frames) {
            if (!fp) continue;
            table_min_d = std::min(table_min_d, fp->depth_meters);
            table_max_d = std::max(table_max_d, fp->depth_meters);
        }
    }
    const float table_depth_range = (table_max_d > table_min_d + 0.001f) ? (table_max_d - table_min_d) : 0.0f;
    // Total vertical spread of the layer stack, in metres. This is the screen's
    // internal depth on a cocktail cabinet -- a hand's width, not a storey. It
    // used to be 1.3m ("table to ceiling"), which spread the layers through
    // head height and put the viewer inside the stack instead of in front of a
    // machine they could look down at.
    constexpr float kTableStackHeight = 0.14f;

    // Compute reference screen height from first non-ui-bar frame so we can
    // position the ScummVM UI bar below the game screen.
    float scene_qh_ref = 0.0f;
    for (const LayerFrame* fp : frames) {
        if (!fp || fp->is_ui_bar || fp->width <= 0 || fp->height <= 0) continue;
        scene_qh_ref = fp->quad_width_meters * (float)fp->height / (float)fp->width;
        break;
    }

    // L1 reference depth: the nearest layer's own depth_meters. Symmetric geometry mode mirrors
    // a layer's distance from this point — a layer at depth d gets a copy at d and a copy at
    // (2*ref_l1_depth - d), i.e. as far on the other side of L1 as it already is on this side.
    float ref_l1_depth = 1e9f;
    for (const LayerFrame* fp : frames) {
        if (!fp || fp->is_ui_bar) continue;
        ref_l1_depth = std::min(ref_l1_depth, fp->depth_meters);
    }
    if (ref_l1_depth > 1e8f) ref_l1_depth = 0.0f;

    // Build draw order: farthest layer first so depth buffer is populated from back
    // to front. Closer layers then pass GL_LESS and overwrite — transparent pixels
    // (discarded in the fragment shader) leave the farther depth intact.
    std::vector<int> order(frames.size());
    for (int i = 0; i < (int)frames.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const float da = frames[a] ? frames[a]->depth_meters : 0.0f;
        const float db = frames[b] ? frames[b]->depth_meters : 0.0f;
        return da > db; // farthest first
    });

    for (int i : order) {
        if (!frames[i]) continue;
        const LayerFrame& fr = *frames[i];
        // "neogeo_fix" (insert-coin/HUD text) is force-included in the layer
        // list every frame regardless of has_pixels (see openxr_shell.cpp's
        // mame_neogeo depth block) specifically so it's always issued as a
        // draw call and its own per-pixel alpha -- not this frame-level
        // has_pixels flag -- decides visibility. Without this exception the
        // draw call itself still got skipped here whenever has_pixels read
        // false, which is most frames, so the layer never actually rendered
        // long enough to be seen as "always on top" -- it just flickered in
        // and out on whichever frames has_pixels happened to be true.
        const bool force_draw = fr.id == "neogeo_fix";
        if (fr.width <= 0 || fr.height <= 0 || fr.rgba.empty()) continue;
        if (!fr.has_pixels && !force_draw) continue;
        // Layer-deck bookshelf: the layer keeps its exact canvas position and
        // instead turns in place about its own vertical axis, by slot
        // (position in `frames`, i.e. stacking order) -- like opening the
        // covers of a book standing on a shelf. Purely a viewing aid -- depth
        // order and every other uniform are untouched.
        const float layer_canvas_az = canvas_az;
        float layer_yaw = 0.0f;
        if (layer_deck_active) {
            const int slot = layer_deck_slots[i];
            if (slot >= 0) {
                layer_yaw = qrd::presentation::layer_deck_yaw(
                    slot, layer_deck_slot_count, layer_deck_spread);
            }
        }
        glUniform1f(u_canvas_az, layer_canvas_az);
        if (u_layer_yaw >= 0) glUniform1f(u_layer_yaw, layer_yaw);
        const bool pixel_fx = state.depth_mode == DepthMode::PixelFx;
        const float fog_factor = pixel_fx
            ? std::clamp((fr.depth_meters - 1.5f) / 8.5f, 0.0f, 0.55f)
            : 0.0f;
        if (u_fog_factor >= 0) glUniform1f(u_fog_factor, fog_factor);
        // Contact shadows disabled by request; retain the alpha-aware helper for later use.
        glUseProgram(layer_program);
        glBindVertexArray(immersive_active ? m_curve_vao : m_vao);
        const bool wedge_active = state.depth_mode == DepthMode::WholeLayer && fr.wedge_eligible;
        // PixelExtrude reuses the exact same per-box real-geometry draw path as BoundingBox —
        // the only difference is what populated fr.object_boxes (one thin box per opaque
        // pixel-run instead of one box per detected blob).
        const bool bbox_active = (state.depth_mode == DepthMode::BoundingBox ||
                                   is_pixel_geometry_mode(state.depth_mode)) &&
                                 fr.bbox_eligible &&
                                 !fr.object_boxes.empty();
        // ZBUF is a dense per-pixel surface. It must never enter the object-extrusion
        // path: if its depth texture is unavailable, the safe fallback is a flat card.
        const bool force_real_geometry = is_pixel_geometry_mode(state.depth_mode);

        const auto upload_start = Clock::now();
        const std::uint64_t prev_revision = (i < (int)m_layers.size()) ? m_layers[i].uploaded_revision : 0;
        update_layer(i, fr);
        upload_ms_total += std::chrono::duration<float, std::milli>(Clock::now() - upload_start).count();
        if (i < (int)m_layers.size() && m_layers[i].uploaded_revision != prev_revision) {
            ++uploaded_layers;
        }

        // Rotate Screen: swap the quad's aspect ratio to match the rotated
        // texture sampling (see uRotateMode in kLayerVS/kImmersiveLayerVS) so
        // a natively-portrait board (e.g. 1941) renders upright at the
        // correct proportions instead of squished into the normal landscape
        // aspect. Only odd multiples of 90 degrees change the aspect ratio.
        const int rotate_mode = state.rotate_screen & 3;
        const bool rotate_odd = (rotate_mode == 1 || rotate_mode == 3);
        const float qw = fr.quad_width_meters;
        const float qh = rotate_odd ? (qw * (float)fr.width / (float)fr.height)
                                     : (qw * (float)fr.height / (float)fr.width);

        int   copy_count;
        float copy_span;
        if (fr.copies.empty() || fr.copies.back() <= 0.0f) {
            copy_count = k_max_copies;
            copy_span  = (float)k_max_copies * k_default_copy_step;
        } else {
            copy_count = (int)fr.copies.size();
            copy_span  = fr.copies.back();
        }

        // ScummVM UI bar: detach below the game screen with a small gap.
        float eff_quad_y = 0.0f; // eye level (app_space origin = HMD position)
        if (fr.is_ui_bar && scene_qh_ref > 1e-5f) {
            constexpr float gap = 0.02f;
            eff_quad_y = -scene_qh_ref * 0.5f - qh * 0.5f - gap;
        }

        // Table Mode: real per-layer extrusion (see uTableMode in
        // kLayerVS/kImmersiveLayerVS -- the panel itself lies flat there,
        // not just tilted). Scaled to this frame's own actual depth spread
        // rather than an assumed absolute range, so it works consistently
        // across every system.
        //
        // The FARTHEST layer (largest depth_meters, e.g. the background) sits
        // lowest, at the bottom of the cabinet, and nearer layers rise toward
        // the glass -- the same front-to-back order the normal vertical canvas
        // has, just turned on its side. The ordering used to be inverted, which
        // put the background on top: looking down into the cabinet you saw only
        // the backdrop, with every other layer hidden underneath it.
        if (state.surface_mode != 0) {
            const float depth_t = table_depth_range > 0.0f
                ? std::clamp((fr.depth_meters - table_min_d) / table_depth_range, 0.0f, 1.0f)
                : 0.0f;
            eff_quad_y = (1.0f - depth_t) * kTableStackHeight;
        }

        glUniform1f(u_depth,      fr.depth_meters);
        glUniform1f(u_quad_w,     qw);
        glUniform1f(u_quad_h,     qh);
        glUniform1f(u_quad_y,     eff_quad_y);
        if (u_rotate90 >= 0) glUniform1f(u_rotate90, (float)rotate_mode);
        if (u_table_mode >= 0) glUniform1f(u_table_mode, (float)state.surface_mode);
        glUniform1f(u_copy_count, (float)copy_count);
        glUniform1f(u_copy_span,  copy_span);
        glUniform1f(u_roundness,  wedge_active ? 1.0f : 0.0f);
        glUniform1f(u_bbox_mode, 0.0f);
        glUniform1f(u_bbox_debug, 0.0f);
        // EXPERIMENT: fill transparent silhouette holes in WholeLayer copies with a solid
        // extrusion shade instead of discarding, so the layer reads as a solid 3D block.
        // Revert: change `wedge_active ? 1.0f : 0.0f` back to `0.0f`.
        glUniform1f(u_solid_stack, wedge_active ? 1.0f : 0.0f);
        const bool has_edge_profile_i = wedge_active && i < (int)m_layers.size() &&
            m_layers[i].edge_lr_tex && m_layers[i].edge_tb_tex;
        if (u_has_edge_profile >= 0) glUniform1f(u_has_edge_profile, has_edge_profile_i ? 1.0f : 0.0f);
        if (has_edge_profile_i) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, m_layers[i].edge_lr_tex);
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, m_layers[i].edge_tb_tex);
            glActiveTexture(GL_TEXTURE0);
        }

        // Perspective compensation: zoom into the texture centre by 1/scale.
        // uSubrect shrinks the rendered quad by sub_w=1/S, so we pre-scale uQuadW/H
        // by S to keep the physical quad size unchanged. Net quad = qw*S*(1/S) = qw.
        if (fr.persp_comp_scale > 1.001f) {
            const float S = fr.persp_comp_scale;
            const float half = 0.5f / S;
            glUniform1f(u_quad_w, qw * S);
            glUniform1f(u_quad_h, qh * S);
            glUniform1f(u_subrect_enable, 1.0f);
            glUniform4f(u_subrect, 0.5f - half, 0.5f - half, 0.5f + half, 0.5f + half);
        } else {
            glUniform1f(u_subrect_enable, 0.0f);
            glUniform4f(u_subrect, 0.0f, 0.0f, 1.0f, 1.0f);
        }

        // Parallax peek: shift deeper layers toward current gaze direction.
        if (parallax_depth_range > 0.0f) {
            const float t = (fr.depth_meters - parallax_min_d) / parallax_depth_range;
            glUniform1f(u_canvas_az, layer_canvas_az + parallax_yaw   * t);
            glUniform1f(u_canvas_el, canvas_el + parallax_pitch * t);
        }

        glUniform1f(u_instance_base, 0.0f);
        glUniform1i(u_object_box_count, 0);

        glBindTexture(GL_TEXTURE_2D, m_layers[i].tex);
        // Upscale shader needs smooth input; otherwise keep crisp nearest-neighbour.
        {
            GLint f = state.upscale_mode != UpscaleMode::Off ? GL_LINEAR : GL_NEAREST;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        }

        // All layers are composited blend-only (no depth writes) in farthest-first order.
        // This matches the original retrodepth behaviour: layers are independent Photoshop-
        // style planes — closer layers alpha-composite over farther ones without punching
        // holes into them via the depth buffer.
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        if (passthrough_alpha) {
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }

        // Sprite Y-depth: tessellated mesh with per-vertex Z from depth texture.
        // Only active in flat (non-immersive) mode.
        const bool zbuffer_pixel_active = state.depth_mode == DepthMode::ZBuffer &&
            fr.zbuffer_depth_valid && !fr.depth_map.empty();
        const bool ydepth_active = (!immersive_active || zbuffer_pixel_active)
            && (state.sprite_y_depth || zbuffer_pixel_active)
            && !fr.depth_map.empty()
            && u_has_y_depth >= 0
            && i < (int)m_layers.size()
            && m_layers[i].depth_tex != 0;

        const auto draw_start = Clock::now();
        if (ydepth_active) {
            ensure_depth_mesh(fr.width, fr.height);
            glUniform1i(u_has_y_depth, 1);
            if (u_y_depth_spread >= 0)
                glUniform1f(u_y_depth_spread, zbuffer_pixel_active ? 2.5f : state.sprite_y_depth_spread);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, m_layers[i].depth_tex);
            glActiveTexture(GL_TEXTURE0);
            glBindVertexArray(m_dm_vao);
            glDrawElements(GL_TRIANGLES, m_dm_index_count, GL_UNSIGNED_INT, nullptr);
            glBindVertexArray(m_vao);
            glUniform1i(u_has_y_depth, 0);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE0);
        } else if (bbox_active && fr.geometry_mode != LayerGeometryMode::Symmetric &&
                   fr.geometry_mode != LayerGeometryMode::SplitFloor &&
                   fr.geometry_mode != LayerGeometryMode::SplitCeiling &&
                   fr.geometry_mode != LayerGeometryMode::Repeat &&
                   fr.geometry_mode != LayerGeometryMode::Room) {
            // Box/Floor/Ceiling/DepthScatter/AutoYDepth/Billboard all share this per-object path —
            // they only differ in which uOrientation code and which of the scatter/y-depth/hmd
            // uniforms get a nonzero value below, not in the draw structure itself.
            // Draw per-object extrusion copies FIRST (they go deeper into the screen).
            // The original front face is drawn LAST so it composites on top — it is the
            // closest layer to the viewer and must win the alpha blend.
            //
            // All boxes x all copies for this layer are packed into ONE instanced draw:
            // the vertex shader derives (boxIndex, copyIndex) from gl_InstanceID and fetches
            // the box's UV rect from the uBoxRects SSBO. This avoids a CPU draw call (and
            // uniform upload) per detected object, so raising the number of boxes or copies
            // costs GPU fill-rate only, not CPU/driver overhead.
            const bool use_real_geometry = (state.real_geometry_boxes || force_real_geometry) && m_box_program != 0;
            if (!fr.object_boxes.empty() && (use_real_geometry || copy_count > 0)) {
                const std::size_t box_count = fr.object_boxes.size();
                std::vector<float> box_rects;
                box_rects.reserve(box_count * 4);
                const float inv_w = 1.0f / (float)fr.width;
                const float inv_h = 1.0f / (float)fr.height;
                for (const ObjectBoundingBox& box : fr.object_boxes) {
                    box_rects.push_back((float)box.min_x * inv_w);
                    box_rects.push_back((float)box.min_y * inv_h);
                    box_rects.push_back((float)(box.max_x + 1) * inv_w);
                    box_rects.push_back((float)(box.max_y + 1) * inv_h);
                }
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_object_box_ssbo);
                glBufferData(GL_SHADER_STORAGE_BUFFER,
                             (GLsizeiptr)(box_rects.size() * sizeof(float)),
                             box_rects.data(), GL_DYNAMIC_DRAW);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_object_box_ssbo);
                std::vector<float> box_depths;
                box_depths.reserve(box_count);
                for (const ObjectBoundingBox& box : fr.object_boxes)
                    box_depths.push_back((state.depth_mode == DepthMode::ZBuffer && box.depth_meters > 0.0f)
                                         ? box.depth_meters : fr.depth_meters);
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_object_depth_ssbo);
                glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)(box_depths.size() * sizeof(float)),
                             box_depths.data(), GL_DYNAMIC_DRAW);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_object_depth_ssbo);
                if (m_u_zbuffer_depths >= 0) glUniform1f(m_u_zbuffer_depths,
                    (state.depth_mode == DepthMode::ZBuffer && fr.zbuffer_depth_valid) ? 1.0f : 0.0f);

                if (use_real_geometry) {
                    // Real 5-face box mesh: one instance per detected object/pixel-run (no copy
                    // stack — the box itself supplies the volume). Honors the per-layer THICK
                    // +/- override (fr.box_thickness_meters) the same way the WholeLayer path
                    // does, falling back to the layer's own copy span (or a small default) so
                    // boxes stay proportioned to whatever depth the whole-layer wedge would have
                    // used.
                    const float thickness = (fr.box_thickness_meters != 0.0f) ? fr.box_thickness_meters
                                           : (copy_span > 0.0f ? copy_span : (k_max_copies * k_default_copy_step));
                    const float orientation = (fr.geometry_mode == LayerGeometryMode::Floor)     ? 1.0f
                                             : (fr.geometry_mode == LayerGeometryMode::Ceiling)   ? 2.0f
                                             : (fr.geometry_mode == LayerGeometryMode::Billboard) ? 5.0f
                                                                                                   : 0.0f;
                    glUseProgram(m_box_program);
                    glBindVertexArray(m_box_vao);
                    glUniformMatrix4fv(m_box_u_vp, 1, GL_FALSE, vp.data());
                    glUniform1f(m_box_u_depth,        fr.depth_meters);
                    if (m_box_u_zbuffer_depths >= 0) glUniform1f(m_box_u_zbuffer_depths,
                        (state.depth_mode == DepthMode::ZBuffer && fr.zbuffer_depth_valid) ? 1.0f : 0.0f);
                    glUniform1f(m_box_u_quad_w,       qw);
                    glUniform1f(m_box_u_quad_h,       qh);
                    glUniform1f(m_box_u_quad_y,       eff_quad_y);
                    if (m_box_u_table_mode >= 0)
                        glUniform1f(m_box_u_table_mode, (float)state.surface_mode);
                    glUniform1f(m_box_u_thickness,    thickness);
                    glUniform1f(m_box_u_screen_curve, immersive_active ? state.screen_curve : 0.0f);
                    glUniform1f(m_box_u_orientation,  orientation);
                    glUniform1f(m_box_u_ref_l1_depth, ref_l1_depth);
                    // Per-object boxes: clamp each box's own extrusion to its footprint so tiny
                    // (e.g. 1px star) objects don't get the same absolute thickness as large
                    // sprites — see uAutoThickness's declaration above for why that matters.
                    // Only applies to the AUTOMATIC fallback thickness (box_thickness_meters <= 0,
                    // derived from copy_span) — once the user sets an explicit THICK +/- value,
                    // that's a deliberate choice and must never be silently capped, or Thick+
                    // would visibly stop doing anything past the object's own footprint size.
                    glUniform1f(m_box_u_auto_thickness, fr.box_thickness_meters != 0.0f ? 0.0f : 1.0f);
                    // DepthScatter/AutoYDepth: only nonzero for their own mode, 0 for every other
                    // geometry_mode sharing this branch (Box/Floor/Ceiling/Billboard) — GL uniform
                    // state persists on the program across draws, so this must be explicit every
                    // time, not just left at whatever the previous layer's draw call set it to.
                    glUniform1f(m_box_u_scatter_range,
                                fr.geometry_mode == LayerGeometryMode::DepthScatter ? fr.scatter_range : 0.0f);
                    glUniform1f(m_box_u_y_depth_range,
                                fr.geometry_mode == LayerGeometryMode::AutoYDepth ? fr.y_depth_range : 0.0f);
                    glUniform3f(m_box_u_hmd_pos, hmd_x, hmd_y, hmd_z);
                    glUniform1f(m_box_u_size_thickness_mode,
                                fr.geometry_mode == LayerGeometryMode::SizeThickness ? 1.0f : 0.0f);
                    glUniform1f(m_box_u_tilt_x,       state.tilt_x);
                    glUniform1f(m_box_u_tilt_y,       state.tilt_y);
                    glUniform1f(m_box_u_canvas_x,     snapped_canvas_x);
                    glUniform1f(m_box_u_canvas_y,     snapped_canvas_y);
                    glUniform1f(m_box_u_canvas_az,    layer_canvas_az);
                    if (m_box_u_layer_yaw >= 0) glUniform1f(m_box_u_layer_yaw, layer_yaw);
                    glUniform1f(m_box_u_canvas_el,    canvas_el);
                    glUniform1f(m_box_u_canvas_scale, snapped_canvas_scale);
                    glUniform1f(m_box_u_gamma,        state.gamma);
                    glUniform1f(m_box_u_contrast,     state.contrast);
                    glUniform1f(m_box_u_saturation,   state.saturation);
                    glUniform1f(m_box_u_brightness,   state.brightness);
                    glUniform1f(m_box_u_pixel_light,  state.depth_mode == DepthMode::PixelFx ? 1.0f : 0.0f);
                    glUniform3f(m_box_u_light_dir,    -0.35f, 0.65f, 0.68f);
                    glUniform1f(m_box_u_light_ambient, 0.30f);
                    glUniform1f(m_box_u_light_flicker, light_flicker);
                    glUniform1f(m_box_u_fog_factor, fog_factor);
                    glUniform3f(m_box_u_fog_color, 0.015f, 0.020f, 0.040f);
                    glUniform1f(m_box_u_force_opaque_alpha, passthrough_alpha ? 1.0f : 0.0f);
                    glUniform1i(m_box_u_texture, 0);
                    {
                        float scr, scg, scb;
                        const bool side_darken = fr.side_color_mode == 6;
                        side_color_mode_to_rgb(side_darken ? 0 : fr.side_color_mode, scr, scg, scb);
                        glUniform1f(m_box_u_side_color_mode, (fr.side_color_mode > 0 && !side_darken) ? 1.0f : 0.0f);
                        glUniform1f(m_box_u_side_color_darken, side_darken ? 1.0f : 0.0f);
                        glUniform3f(m_box_u_side_color_rgb, scr, scg, scb);
                    }

                    glDepthMask(GL_TRUE);  // self-occlude overlapping box faces (safe: box fragments are opaque)
                    glDrawArraysInstanced(GL_TRIANGLES, 0, m_box_vertex_count, (GLsizei)box_count);
                    glDepthMask(GL_FALSE);

                    glUseProgram(layer_program);
                    glBindVertexArray(immersive_active ? m_curve_vao : m_vao);
                } else {
                    glUniform1f(u_depthmap, 1.0f);
                    glUniform1f(u_roundness, 1.0f);
                    glUniform1f(u_bbox_mode, 1.0f);
                    glUniform1f(u_bbox_debug, 0.0f);
                    glUniform1f(u_instance_base, 1.0f);
                    glUniform1i(u_object_box_count, (int)box_count);

                    glDrawArraysInstanced(GL_TRIANGLES, 0, layer_vertex_count,
                                           (GLsizei)(box_count * (std::size_t)copy_count));

                    glUniform1f(u_depthmap, state.depth_mode == DepthMode::WholeLayer ? 1.0f : 0.0f);
                    glUniform1f(u_roundness, wedge_active ? 1.0f : 0.0f);
                    glUniform1f(u_bbox_mode, 0.0f);
                    glUniform1f(u_bbox_debug, 0.0f);
                    glUniform1f(u_instance_base, 0.0f);
                    glUniform1i(u_object_box_count, 0);
                }
            }

            // ZBUF boxes already carry the emulator-derived object depth. Do not add the
            // legacy full-layer card on top of them, or it would collapse every object back
            // onto the layer's single fixed depth.
            if (state.depth_mode != DepthMode::ZBuffer)
                glDrawArrays(GL_TRIANGLES, 0, layer_vertex_count);
        } else if (state.layers_3d && state.depth_mode != DepthMode::ZBuffer) {
            glDrawArraysInstanced(GL_TRIANGLES, 0, layer_vertex_count, copy_count + 1);
        } else if (state.depth_mode == DepthMode::ZBuffer) {
            // No usable dense depth map: safe flat-card fallback, never an extrusion.
            glDrawArrays(GL_TRIANGLES, 0, layer_vertex_count);
        } else if ((wedge_active || bbox_active) && state.real_geometry_boxes && fr.geometry_mode == LayerGeometryMode::Symmetric && m_box_program != 0) {
            // Symmetric: two full boxes (not flat planes), mirrored around L1 (the nearest layer,
            // ref_l1_depth). A layer at distance d from L1 gets a normal box at d and a second box
            // at (2*ref_l1_depth - d) -- as far behind L1 as it sits in front of it. Each box
            // extrudes AWAY from L1 (the near one extrudes to larger depth as usual; the mirrored
            // one extrudes to even more negative depth, via a negated uThickness), so both ends
            // stay solid whichever direction the player approaches from.
            // Per-object mode (PixelExtrude/BoundingBox): mirror EACH detected object/pixel-run
            // individually instead of one box over the whole layer's content_bounds — otherwise
            // sparse content (e.g. a starfield) collapses into one big near-empty box spanning
            // its scattered footprint and loses the per-star real geometry entirely. Falls back
            // to the single content-bounds box only for WholeLayer depth mode, where there's no
            // per-object data to begin with.
            const bool sym_per_object = bbox_active && !fr.object_boxes.empty();
            std::size_t sym_box_count = 1;
            if (sym_per_object) {
                sym_box_count = fr.object_boxes.size();
                std::vector<float> box_rects;
                box_rects.reserve(sym_box_count * 4);
                const float inv_w = 1.0f / (float)fr.width;
                const float inv_h = 1.0f / (float)fr.height;
                for (const ObjectBoundingBox& box : fr.object_boxes) {
                    box_rects.push_back((float)box.min_x * inv_w);
                    box_rects.push_back((float)box.min_y * inv_h);
                    box_rects.push_back((float)(box.max_x + 1) * inv_w);
                    box_rects.push_back((float)(box.max_y + 1) * inv_h);
                }
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_object_box_ssbo);
                glBufferData(GL_SHADER_STORAGE_BUFFER,
                             (GLsizeiptr)(box_rects.size() * sizeof(float)),
                             box_rects.data(), GL_DYNAMIC_DRAW);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_object_box_ssbo);
            } else {
                // Trim the footprint to the layer's actual content (see LayerFrame::content_bounds)
                // instead of the full padded frame, so side faces sample real edge pixels of a small
                // sprite rather than the mostly-empty frame border.
                const std::array<float, 4> kFullRectSym = fr.content_bounds_valid
                    ? std::array<float, 4>{
                          (float)fr.content_bounds.min_x / (float)fr.width,
                          (float)fr.content_bounds.min_y / (float)fr.height,
                          (float)(fr.content_bounds.max_x + 1) / (float)fr.width,
                          (float)(fr.content_bounds.max_y + 1) / (float)fr.height}
                    : std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f};
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_object_box_ssbo);
                glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(float) * 4, kFullRectSym.data(), GL_DYNAMIC_DRAW);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_object_box_ssbo);
            }

            const float thickness = (fr.box_thickness_meters != 0.0f) ? fr.box_thickness_meters
                                   : (copy_span > 0.0f ? copy_span : (k_max_copies * k_default_copy_step));
            const float mirrored_depth = 2.0f * ref_l1_depth - fr.depth_meters;

            glUseProgram(m_box_program);
            glBindVertexArray(m_box_vao);
            glUniformMatrix4fv(m_box_u_vp, 1, GL_FALSE, vp.data());
            glUniform1f(m_box_u_quad_w,       qw);
            glUniform1f(m_box_u_quad_h,       qh);
            glUniform1f(m_box_u_quad_y,       eff_quad_y);
            if (m_box_u_table_mode >= 0)
                glUniform1f(m_box_u_table_mode, (float)state.surface_mode);
            glUniform1f(m_box_u_screen_curve, immersive_active ? state.screen_curve : 0.0f);
            glUniform1f(m_box_u_orientation,  0.0f);
            // Same rule as the plain per-object path: only auto-clamp the automatic fallback
            // thickness, never an explicit user-set THICK value.
            glUniform1f(m_box_u_auto_thickness,
                        (sym_per_object && fr.box_thickness_meters == 0.0f) ? 1.0f : 0.0f);
            glUniform1f(m_box_u_scatter_range, 0.0f);
            glUniform1f(m_box_u_y_depth_range, 0.0f);
            glUniform1f(m_box_u_size_thickness_mode, 0.0f);
            glUniform1f(m_box_u_tilt_x,       state.tilt_x);
            glUniform1f(m_box_u_tilt_y,       state.tilt_y);
            glUniform1f(m_box_u_canvas_x,     snapped_canvas_x);
            glUniform1f(m_box_u_canvas_y,     snapped_canvas_y);
            glUniform1f(m_box_u_canvas_az,    layer_canvas_az);
            if (m_box_u_layer_yaw >= 0) glUniform1f(m_box_u_layer_yaw, layer_yaw);
            glUniform1f(m_box_u_canvas_el,    canvas_el);
            glUniform1f(m_box_u_canvas_scale, snapped_canvas_scale);
            glUniform1f(m_box_u_gamma,        state.gamma);
            glUniform1f(m_box_u_contrast,     state.contrast);
            glUniform1f(m_box_u_saturation,   state.saturation);
            glUniform1f(m_box_u_brightness,   state.brightness);
            glUniform1f(m_box_u_force_opaque_alpha, passthrough_alpha ? 1.0f : 0.0f);
            glUniform1i(m_box_u_texture, 0);
            const bool use_silhouette_sym = state.silhouette_sides
                && i < (int)m_layers.size() && m_layers[i].edge_lr_tex && m_layers[i].edge_tb_tex;
            glUniform1f(m_box_u_silhouette, use_silhouette_sym ? 1.0f : 0.0f);
            {
                float scr, scg, scb;
                const bool side_darken = fr.side_color_mode == 6;
                side_color_mode_to_rgb(side_darken ? 0 : fr.side_color_mode, scr, scg, scb);
                glUniform1f(m_box_u_side_color_mode, (fr.side_color_mode > 0 && !side_darken) ? 1.0f : 0.0f);
                glUniform1f(m_box_u_side_color_darken, side_darken ? 1.0f : 0.0f);
                glUniform3f(m_box_u_side_color_rgb, scr, scg, scb);
            }
            if (use_silhouette_sym) {
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, m_layers[i].edge_lr_tex);
                glActiveTexture(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_2D, m_layers[i].edge_tb_tex);
                glActiveTexture(GL_TEXTURE0);
                glUniform1i(m_box_u_edge_lr, 2);
                glUniform1i(m_box_u_edge_tb, 3);
            }

            // Near-side box(es): extrudes to larger depth, same as normal Box mode.
            glUniform1f(m_box_u_depth,        fr.depth_meters);
            glUniform1f(m_box_u_thickness,    thickness);
            glUniform1f(m_box_u_allow_behind, 0.0f);
            glDepthMask(GL_TRUE);  // self-occlude overlapping box faces (safe: box fragments are opaque)
            glDrawArraysInstanced(GL_TRIANGLES, 0, m_box_vertex_count, (GLsizei)sym_box_count);
            glDepthMask(GL_FALSE);

            // Mirrored box(es): negated thickness reverses the extrusion direction so it also
            // grows away from L1 (more negative), instead of back toward it; allow_behind bypasses
            // the usual >=0.01 clamp since this depth is genuinely negative/behind the viewer origin.
            glUniform1f(m_box_u_depth,        mirrored_depth);
            glUniform1f(m_box_u_thickness,    -thickness);
            glUniform1f(m_box_u_allow_behind, 1.0f);
            glDepthMask(GL_TRUE);  // self-occlude overlapping box faces (safe: box fragments are opaque)
            glDrawArraysInstanced(GL_TRIANGLES, 0, m_box_vertex_count, (GLsizei)sym_box_count);
            glDepthMask(GL_FALSE);

            glUseProgram(layer_program);
            glBindVertexArray(immersive_active ? m_curve_vao : m_vao);
            // Front faces (the actual sprite) for both copies, composited on top of their boxes.
            glUniform1f(u_depth, fr.depth_meters);
            glDrawArrays(GL_TRIANGLES, 0, layer_vertex_count);
            glUniform1f(u_allow_behind, 1.0f);
            glUniform1f(u_depth, mirrored_depth);
            glDrawArrays(GL_TRIANGLES, 0, layer_vertex_count);
            glUniform1f(u_allow_behind, 0.0f);
            glUniform1f(u_depth, fr.depth_meters);
        } else if ((wedge_active || bbox_active) && state.real_geometry_boxes && fr.geometry_mode == LayerGeometryMode::Repeat && m_box_program != 0) {
            // Repeat: Symmetric generalized from exactly 2 boxes to N alternating copies spaced
            // around L1 (hall-of-mirrors/tunnel effect) — same per-object-vs-whole-layer box_rects
            // construction as Symmetric, just looped N times with alternating extrusion direction
            // instead of drawn twice.
            const bool rep_per_object = bbox_active && !fr.object_boxes.empty();
            std::size_t rep_box_count = 1;
            if (rep_per_object) {
                rep_box_count = fr.object_boxes.size();
                std::vector<float> box_rects;
                box_rects.reserve(rep_box_count * 4);
                const float inv_w = 1.0f / (float)fr.width;
                const float inv_h = 1.0f / (float)fr.height;
                for (const ObjectBoundingBox& box : fr.object_boxes) {
                    box_rects.push_back((float)box.min_x * inv_w);
                    box_rects.push_back((float)box.min_y * inv_h);
                    box_rects.push_back((float)(box.max_x + 1) * inv_w);
                    box_rects.push_back((float)(box.max_y + 1) * inv_h);
                }
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_object_box_ssbo);
                glBufferData(GL_SHADER_STORAGE_BUFFER,
                             (GLsizeiptr)(box_rects.size() * sizeof(float)),
                             box_rects.data(), GL_DYNAMIC_DRAW);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_object_box_ssbo);
            } else {
                const std::array<float, 4> kFullRectRep = fr.content_bounds_valid
                    ? std::array<float, 4>{
                          (float)fr.content_bounds.min_x / (float)fr.width,
                          (float)fr.content_bounds.min_y / (float)fr.height,
                          (float)(fr.content_bounds.max_x + 1) / (float)fr.width,
                          (float)(fr.content_bounds.max_y + 1) / (float)fr.height}
                    : std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f};
                glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_object_box_ssbo);
                glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(float) * 4, kFullRectRep.data(), GL_DYNAMIC_DRAW);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_object_box_ssbo);
            }

            const float thickness = (fr.box_thickness_meters != 0.0f) ? fr.box_thickness_meters
                                   : (copy_span > 0.0f ? copy_span : (k_max_copies * k_default_copy_step));
            constexpr int k_repeat_count_max = 8;
            const int N = std::clamp(fr.repeat_count > 0 ? fr.repeat_count : 3, 2, k_repeat_count_max);
            const float span = fr.depth_meters - ref_l1_depth;

            glUseProgram(m_box_program);
            glBindVertexArray(m_box_vao);
            glUniformMatrix4fv(m_box_u_vp, 1, GL_FALSE, vp.data());
            glUniform1f(m_box_u_quad_w,       qw);
            glUniform1f(m_box_u_quad_h,       qh);
            glUniform1f(m_box_u_quad_y,       eff_quad_y);
            if (m_box_u_table_mode >= 0)
                glUniform1f(m_box_u_table_mode, (float)state.surface_mode);
            glUniform1f(m_box_u_screen_curve, immersive_active ? state.screen_curve : 0.0f);
            glUniform1f(m_box_u_orientation,  0.0f);
            glUniform1f(m_box_u_auto_thickness,
                        (rep_per_object && fr.box_thickness_meters == 0.0f) ? 1.0f : 0.0f);
            glUniform1f(m_box_u_scatter_range, 0.0f);
            glUniform1f(m_box_u_y_depth_range, 0.0f);
            glUniform1f(m_box_u_size_thickness_mode, 0.0f);
            glUniform1f(m_box_u_tilt_x,       state.tilt_x);
            glUniform1f(m_box_u_tilt_y,       state.tilt_y);
            glUniform1f(m_box_u_canvas_x,     snapped_canvas_x);
            glUniform1f(m_box_u_canvas_y,     snapped_canvas_y);
            glUniform1f(m_box_u_canvas_az,    layer_canvas_az);
            if (m_box_u_layer_yaw >= 0) glUniform1f(m_box_u_layer_yaw, layer_yaw);
            glUniform1f(m_box_u_canvas_el,    canvas_el);
            glUniform1f(m_box_u_canvas_scale, snapped_canvas_scale);
            glUniform1f(m_box_u_gamma,        state.gamma);
            glUniform1f(m_box_u_contrast,     state.contrast);
            glUniform1f(m_box_u_saturation,   state.saturation);
            glUniform1f(m_box_u_brightness,   state.brightness);
            glUniform1f(m_box_u_force_opaque_alpha, passthrough_alpha ? 1.0f : 0.0f);
            glUniform1i(m_box_u_texture, 0);
            const bool use_silhouette_rep = state.silhouette_sides
                && i < (int)m_layers.size() && m_layers[i].edge_lr_tex && m_layers[i].edge_tb_tex;
            glUniform1f(m_box_u_silhouette, use_silhouette_rep ? 1.0f : 0.0f);
            {
                float scr, scg, scb;
                const bool side_darken = fr.side_color_mode == 6;
                side_color_mode_to_rgb(side_darken ? 0 : fr.side_color_mode, scr, scg, scb);
                glUniform1f(m_box_u_side_color_mode, (fr.side_color_mode > 0 && !side_darken) ? 1.0f : 0.0f);
                glUniform1f(m_box_u_side_color_darken, side_darken ? 1.0f : 0.0f);
                glUniform3f(m_box_u_side_color_rgb, scr, scg, scb);
            }
            if (use_silhouette_rep) {
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, m_layers[i].edge_lr_tex);
                glActiveTexture(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_2D, m_layers[i].edge_tb_tex);
                glActiveTexture(GL_TEXTURE0);
                glUniform1i(m_box_u_edge_lr, 2);
                glUniform1i(m_box_u_edge_tb, 3);
            }

            glUseProgram(layer_program);
            glBindVertexArray(immersive_active ? m_curve_vao : m_vao);
            for (int k = 0; k < N; ++k) {
                const float depth_k = ref_l1_depth + ((float)k - (float)(N - 1) * 0.5f) * span;
                const float thickness_k = (k % 2 == 0) ? thickness : -thickness;
                const bool  allow_behind_k = depth_k < 0.01f;
                glUseProgram(m_box_program);
                glBindVertexArray(m_box_vao);
                glUniform1f(m_box_u_depth,        depth_k);
                glUniform1f(m_box_u_thickness,    thickness_k);
                glUniform1f(m_box_u_allow_behind, allow_behind_k ? 1.0f : 0.0f);
                glDepthMask(GL_TRUE);  // self-occlude overlapping box faces (safe: box fragments are opaque)
                glDrawArraysInstanced(GL_TRIANGLES, 0, m_box_vertex_count, (GLsizei)rep_box_count);
                glDepthMask(GL_FALSE);

                glUseProgram(layer_program);
                glBindVertexArray(immersive_active ? m_curve_vao : m_vao);
                glUniform1f(u_allow_behind, allow_behind_k ? 1.0f : 0.0f);
                glUniform1f(u_depth, depth_k);
                glDrawArrays(GL_TRIANGLES, 0, layer_vertex_count);
            }
            glUniform1f(u_allow_behind, 0.0f);
            glUniform1f(u_depth, fr.depth_meters);
        } else if ((wedge_active || bbox_active) && state.real_geometry_boxes && fr.geometry_mode == LayerGeometryMode::SplitFloor && m_box_program != 0) {
            // SplitFloor: one layer, two regions drawn simultaneously — bottom split_pixels rows
            // as a Floor, the rest standing up as a Box wall. For backgrounds that bake floor+wall
            // into a single plane (beat-em-ups) with no clean per-layer separation to work with.
            // Reuses the same "manual subrect" mechanism as the per-object bbox path, just with
            // two hand-picked rects instead of connected-component-detected ones.
            int split_px = (fr.split_pixels > 0) ? fr.split_pixels : 1;
            split_px = std::clamp(split_px, 1, std::max(1, fr.height - 1));
            // v=0 at top of image, v=1 at bottom (matches the mesh's own UV convention).
            const float floor_v0 = 1.0f - (float)split_px / (float)fr.height;

            const float thickness = (fr.box_thickness_meters != 0.0f) ? fr.box_thickness_meters
                                   : (copy_span > 0.0f ? copy_span : (k_max_copies * k_default_copy_step));

            glUseProgram(m_box_program);
            glBindVertexArray(m_box_vao);
            glUniformMatrix4fv(m_box_u_vp, 1, GL_FALSE, vp.data());
            glUniform1f(m_box_u_depth,        fr.depth_meters);
            glUniform1f(m_box_u_quad_w,       qw);
            glUniform1f(m_box_u_quad_h,       qh);
            glUniform1f(m_box_u_quad_y,       eff_quad_y);
            if (m_box_u_table_mode >= 0)
                glUniform1f(m_box_u_table_mode, (float)state.surface_mode);
            glUniform1f(m_box_u_thickness,    thickness);
            glUniform1f(m_box_u_screen_curve, immersive_active ? state.screen_curve : 0.0f);
            glUniform1f(m_box_u_ref_l1_depth, ref_l1_depth);
            glUniform1f(m_box_u_tilt_x,       state.tilt_x);
            glUniform1f(m_box_u_tilt_y,       state.tilt_y);
            glUniform1f(m_box_u_canvas_x,     snapped_canvas_x);
            glUniform1f(m_box_u_canvas_y,     snapped_canvas_y);
            glUniform1f(m_box_u_canvas_az,    layer_canvas_az);
            if (m_box_u_layer_yaw >= 0) glUniform1f(m_box_u_layer_yaw, layer_yaw);
            glUniform1f(m_box_u_canvas_el,    canvas_el);
            glUniform1f(m_box_u_canvas_scale, snapped_canvas_scale);
            glUniform1f(m_box_u_gamma,        state.gamma);
            glUniform1f(m_box_u_contrast,     state.contrast);
            glUniform1f(m_box_u_saturation,   state.saturation);
            glUniform1f(m_box_u_brightness,   state.brightness);
            glUniform1f(m_box_u_force_opaque_alpha, passthrough_alpha ? 1.0f : 0.0f);
            glUniform1f(m_box_u_allow_behind, 0.0f);
            glUniform1f(m_box_u_auto_thickness, 0.0f);
            glUniform1f(m_box_u_scatter_range, 0.0f);
            glUniform1f(m_box_u_y_depth_range, 0.0f);
            glUniform1f(m_box_u_size_thickness_mode, 0.0f);
            glUniform1i(m_box_u_texture, 0);
            const bool use_silhouette_sf = state.silhouette_sides
                && i < (int)m_layers.size() && m_layers[i].edge_lr_tex && m_layers[i].edge_tb_tex;
            glUniform1f(m_box_u_silhouette, use_silhouette_sf ? 1.0f : 0.0f);
            {
                float scr, scg, scb;
                const bool side_darken = fr.side_color_mode == 6;
                side_color_mode_to_rgb(side_darken ? 0 : fr.side_color_mode, scr, scg, scb);
                glUniform1f(m_box_u_side_color_mode, (fr.side_color_mode > 0 && !side_darken) ? 1.0f : 0.0f);
                glUniform1f(m_box_u_side_color_darken, side_darken ? 1.0f : 0.0f);
                glUniform3f(m_box_u_side_color_rgb, scr, scg, scb);
            }
            if (use_silhouette_sf) {
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, m_layers[i].edge_lr_tex);
                glActiveTexture(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_2D, m_layers[i].edge_tb_tex);
                glActiveTexture(GL_TEXTURE0);
                glUniform1i(m_box_u_edge_lr, 2);
                glUniform1i(m_box_u_edge_tb, 3);
            }

            // Wall region: the FULL image, standing Box orientation — the floor region below is a
            // continuation growing from this box's own front face (see is_region in the shader),
            // not a separate disconnected plane, so it must not be cropped to exclude the floor
            // band or the two pieces won't share an edge.
            const float wallRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_object_box_ssbo);
            glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(wallRect), wallRect, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_object_box_ssbo);
            glUniform1f(m_box_u_orientation, 0.0f);
            glDepthMask(GL_TRUE);  // self-occlude overlapping box faces (safe: box fragments are opaque)
            glDrawArraysInstanced(GL_TRIANGLES, 0, m_box_vertex_count, 1);
            glDepthMask(GL_FALSE);

            // Floor region: bottom of the image (v: floor_v0 .. 1), laid-flat Floor orientation.
            // Orientation 3.0 (not the pure-Floor 1.0) — stretches this already-cropped row band
            // across the depth span instead of tiling a single edge row, since the user picked a
            // specific band size via split_pixels and expects that whole band to show, not just
            // its outer-most row.
            const float floorRect[4] = {0.0f, floor_v0, 1.0f, 1.0f};
            glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(floorRect), floorRect, GL_DYNAMIC_DRAW);
            glUniform1f(m_box_u_orientation, 3.0f);
            glDepthMask(GL_TRUE);  // self-occlude overlapping box faces (safe: box fragments are opaque)
            glDrawArraysInstanced(GL_TRIANGLES, 0, m_box_vertex_count, 1);
            glDepthMask(GL_FALSE);

            glUseProgram(layer_program);
            glBindVertexArray(immersive_active ? m_curve_vao : m_vao);
            // Front face last: the original flat composite (floor+wall as originally drawn),
            // closest to the viewer, on top of both regions' boxes.
            glDrawArrays(GL_TRIANGLES, 0, layer_vertex_count);
        } else if ((wedge_active || bbox_active) && state.real_geometry_boxes && fr.geometry_mode == LayerGeometryMode::SplitCeiling && m_box_program != 0) {
            // SplitCeiling: mirror of SplitFloor — top split_pixels rows become a Ceiling, the
            // rest stands up as a Box wall. For backgrounds that bake wall+ceiling into one
            // plane (e.g. a corridor/room BG with no separate ceiling layer).
            int split_px = (fr.split_pixels > 0) ? fr.split_pixels : 1;
            split_px = std::clamp(split_px, 1, std::max(1, fr.height - 1));
            // v=0 at top of image, v=1 at bottom. Ceiling occupies the top split_pixels rows.
            const float ceil_v1 = (float)split_px / (float)fr.height;

            const float thickness = (fr.box_thickness_meters != 0.0f) ? fr.box_thickness_meters
                                   : (copy_span > 0.0f ? copy_span : (k_max_copies * k_default_copy_step));

            glUseProgram(m_box_program);
            glBindVertexArray(m_box_vao);
            glUniformMatrix4fv(m_box_u_vp, 1, GL_FALSE, vp.data());
            glUniform1f(m_box_u_depth,        fr.depth_meters);
            glUniform1f(m_box_u_quad_w,       qw);
            glUniform1f(m_box_u_quad_h,       qh);
            glUniform1f(m_box_u_quad_y,       eff_quad_y);
            if (m_box_u_table_mode >= 0)
                glUniform1f(m_box_u_table_mode, (float)state.surface_mode);
            glUniform1f(m_box_u_thickness,    thickness);
            glUniform1f(m_box_u_screen_curve, immersive_active ? state.screen_curve : 0.0f);
            glUniform1f(m_box_u_ref_l1_depth, ref_l1_depth);
            glUniform1f(m_box_u_tilt_x,       state.tilt_x);
            glUniform1f(m_box_u_tilt_y,       state.tilt_y);
            glUniform1f(m_box_u_canvas_x,     snapped_canvas_x);
            glUniform1f(m_box_u_canvas_y,     snapped_canvas_y);
            glUniform1f(m_box_u_canvas_az,    layer_canvas_az);
            if (m_box_u_layer_yaw >= 0) glUniform1f(m_box_u_layer_yaw, layer_yaw);
            glUniform1f(m_box_u_canvas_el,    canvas_el);
            glUniform1f(m_box_u_canvas_scale, snapped_canvas_scale);
            glUniform1f(m_box_u_gamma,        state.gamma);
            glUniform1f(m_box_u_contrast,     state.contrast);
            glUniform1f(m_box_u_saturation,   state.saturation);
            glUniform1f(m_box_u_brightness,   state.brightness);
            glUniform1f(m_box_u_force_opaque_alpha, passthrough_alpha ? 1.0f : 0.0f);
            glUniform1f(m_box_u_allow_behind, 0.0f);
            glUniform1f(m_box_u_auto_thickness, 0.0f);
            glUniform1f(m_box_u_scatter_range, 0.0f);
            glUniform1f(m_box_u_y_depth_range, 0.0f);
            glUniform1f(m_box_u_size_thickness_mode, 0.0f);
            glUniform1i(m_box_u_texture, 0);
            const bool use_silhouette_sc = state.silhouette_sides
                && i < (int)m_layers.size() && m_layers[i].edge_lr_tex && m_layers[i].edge_tb_tex;
            glUniform1f(m_box_u_silhouette, use_silhouette_sc ? 1.0f : 0.0f);
            {
                float scr, scg, scb;
                const bool side_darken = fr.side_color_mode == 6;
                side_color_mode_to_rgb(side_darken ? 0 : fr.side_color_mode, scr, scg, scb);
                glUniform1f(m_box_u_side_color_mode, (fr.side_color_mode > 0 && !side_darken) ? 1.0f : 0.0f);
                glUniform1f(m_box_u_side_color_darken, side_darken ? 1.0f : 0.0f);
                glUniform3f(m_box_u_side_color_rgb, scr, scg, scb);
            }
            if (use_silhouette_sc) {
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, m_layers[i].edge_lr_tex);
                glActiveTexture(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_2D, m_layers[i].edge_tb_tex);
                glActiveTexture(GL_TEXTURE0);
                glUniform1i(m_box_u_edge_lr, 2);
                glUniform1i(m_box_u_edge_tb, 3);
            }

            // Ceiling region: top of the image (v: 0 .. ceil_v1), laid-flat Ceiling orientation.
            // Orientation 4.0 (not the pure-Ceiling 2.0) — same reasoning as SplitFloor's 3.0
            // above: stretch the chosen band, don't collapse it to a single tiled row.
            const float ceilRect[4] = {0.0f, 0.0f, 1.0f, ceil_v1};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_object_box_ssbo);
            glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(ceilRect), ceilRect, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_object_box_ssbo);
            glUniform1f(m_box_u_orientation, 4.0f);
            glDepthMask(GL_TRUE);  // self-occlude overlapping box faces (safe: box fragments are opaque)
            glDrawArraysInstanced(GL_TRIANGLES, 0, m_box_vertex_count, 1);
            glDepthMask(GL_FALSE);

            // Wall region: the FULL image, standing Box orientation — see the SplitFloor wall
            // comment above for why this must not be cropped (the ceiling band is a continuation
            // growing from this box's own front face, not a disconnected plane).
            const float wallRect2[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(wallRect2), wallRect2, GL_DYNAMIC_DRAW);
            glUniform1f(m_box_u_orientation, 0.0f);
            glDepthMask(GL_TRUE);  // self-occlude overlapping box faces (safe: box fragments are opaque)
            glDrawArraysInstanced(GL_TRIANGLES, 0, m_box_vertex_count, 1);
            glDepthMask(GL_FALSE);

            glUseProgram(layer_program);
            glBindVertexArray(immersive_active ? m_curve_vao : m_vao);
            // Front face last: the original flat composite (wall+ceiling as originally drawn),
            // closest to the viewer, on top of both regions' boxes.
            glDrawArrays(GL_TRIANGLES, 0, layer_vertex_count);
        } else if ((wedge_active || bbox_active) && state.real_geometry_boxes && fr.geometry_mode == LayerGeometryMode::Room && m_box_program != 0) {
            // Room: SplitFloor + SplitCeiling combined — top split_pixels rows become a Ceiling,
            // bottom split_pixels rows become a Floor, the middle remainder stands as a Box wall,
            // all from one layer. Same split_pixels value applied symmetrically to both edges.
            // Always whole-layer (single content_bounds rect, like SplitFloor/SplitCeiling already
            // are) — no per-object variant, keeping 3-region dispatch simple.
            int split_px = (fr.split_pixels > 0) ? fr.split_pixels : 1;
            split_px = std::clamp(split_px, 1, std::max(1, (fr.height - 1) / 2));
            // v=0 at top of image, v=1 at bottom.
            const float ceil_v1  = (float)split_px / (float)fr.height;
            const float floor_v0 = 1.0f - (float)split_px / (float)fr.height;
            // u=0 at left of image, u=1 at right — same split_px row count reused as a column
            // width for the left/right walls, clamped against width instead of height.
            int split_px_x = std::clamp(split_px, 1, std::max(1, (fr.width - 1) / 2));
            const float left_u1   = (float)split_px_x / (float)fr.width;
            const float right_u0  = 1.0f - (float)split_px_x / (float)fr.width;

            const float thickness = (fr.box_thickness_meters != 0.0f) ? fr.box_thickness_meters
                                   : (copy_span > 0.0f ? copy_span : (k_max_copies * k_default_copy_step));

            glUseProgram(m_box_program);
            glBindVertexArray(m_box_vao);
            glUniformMatrix4fv(m_box_u_vp, 1, GL_FALSE, vp.data());
            glUniform1f(m_box_u_depth,        fr.depth_meters);
            glUniform1f(m_box_u_quad_w,       qw);
            glUniform1f(m_box_u_quad_h,       qh);
            glUniform1f(m_box_u_quad_y,       eff_quad_y);
            if (m_box_u_table_mode >= 0)
                glUniform1f(m_box_u_table_mode, (float)state.surface_mode);
            glUniform1f(m_box_u_thickness,    thickness);
            glUniform1f(m_box_u_screen_curve, immersive_active ? state.screen_curve : 0.0f);
            glUniform1f(m_box_u_ref_l1_depth, ref_l1_depth);
            glUniform1f(m_box_u_tilt_x,       state.tilt_x);
            glUniform1f(m_box_u_tilt_y,       state.tilt_y);
            glUniform1f(m_box_u_canvas_x,     snapped_canvas_x);
            glUniform1f(m_box_u_canvas_y,     snapped_canvas_y);
            glUniform1f(m_box_u_canvas_az,    layer_canvas_az);
            if (m_box_u_layer_yaw >= 0) glUniform1f(m_box_u_layer_yaw, layer_yaw);
            glUniform1f(m_box_u_canvas_el,    canvas_el);
            glUniform1f(m_box_u_canvas_scale, snapped_canvas_scale);
            glUniform1f(m_box_u_gamma,        state.gamma);
            glUniform1f(m_box_u_contrast,     state.contrast);
            glUniform1f(m_box_u_saturation,   state.saturation);
            glUniform1f(m_box_u_brightness,   state.brightness);
            glUniform1f(m_box_u_force_opaque_alpha, passthrough_alpha ? 1.0f : 0.0f);
            glUniform1f(m_box_u_allow_behind, 0.0f);
            glUniform1f(m_box_u_auto_thickness, 0.0f);
            glUniform1f(m_box_u_scatter_range, 0.0f);
            glUniform1f(m_box_u_y_depth_range, 0.0f);
            glUniform1f(m_box_u_size_thickness_mode, 0.0f);
            glUniform1i(m_box_u_texture, 0);
            const bool use_silhouette_room = state.silhouette_sides
                && i < (int)m_layers.size() && m_layers[i].edge_lr_tex && m_layers[i].edge_tb_tex;
            glUniform1f(m_box_u_silhouette, use_silhouette_room ? 1.0f : 0.0f);
            {
                float scr, scg, scb;
                const bool side_darken = fr.side_color_mode == 6;
                side_color_mode_to_rgb(side_darken ? 0 : fr.side_color_mode, scr, scg, scb);
                glUniform1f(m_box_u_side_color_mode, (fr.side_color_mode > 0 && !side_darken) ? 1.0f : 0.0f);
                glUniform1f(m_box_u_side_color_darken, side_darken ? 1.0f : 0.0f);
                glUniform3f(m_box_u_side_color_rgb, scr, scg, scb);
            }
            if (use_silhouette_room) {
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, m_layers[i].edge_lr_tex);
                glActiveTexture(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_2D, m_layers[i].edge_tb_tex);
                glActiveTexture(GL_TEXTURE0);
                glUniform1i(m_box_u_edge_lr, 2);
                glUniform1i(m_box_u_edge_tb, 3);
            }

            // Wall region: the FULL image, standing Box orientation. Drawn first — the ceiling/
            // floor/left/right regions are continuations growing from this box's own front face
            // (see is_region in the shader), not disconnected planes, so this must not be cropped.
            const float wallRect3[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_object_box_ssbo);
            glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(wallRect3), wallRect3, GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_object_box_ssbo);
            glUniform1f(m_box_u_orientation, 0.0f);
            glDepthMask(GL_TRUE);  // self-occlude overlapping box faces (safe: box fragments are opaque)
            glDrawArraysInstanced(GL_TRIANGLES, 0, m_box_vertex_count, 1);
            glDepthMask(GL_FALSE);

            // Ceiling region: top split_pixels rows.
            const float ceilRect2[4] = {0.0f, 0.0f, 1.0f, ceil_v1};
            glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(ceilRect2), ceilRect2, GL_DYNAMIC_DRAW);
            glUniform1f(m_box_u_orientation, 4.0f);
            glDepthMask(GL_TRUE);  // self-occlude overlapping box faces (safe: box fragments are opaque)
            glDrawArraysInstanced(GL_TRIANGLES, 0, m_box_vertex_count, 1);
            glDepthMask(GL_FALSE);

            // Floor region: bottom split_pixels rows.
            const float floorRect2[4] = {0.0f, floor_v0, 1.0f, 1.0f};
            glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(floorRect2), floorRect2, GL_DYNAMIC_DRAW);
            glUniform1f(m_box_u_orientation, 3.0f);
            glDepthMask(GL_TRUE);  // self-occlude overlapping box faces (safe: box fragments are opaque)
            glDrawArraysInstanced(GL_TRIANGLES, 0, m_box_vertex_count, 1);
            glDepthMask(GL_FALSE);

            // Left wall region: left split_px_x columns.
            const float leftRect[4] = {0.0f, 0.0f, left_u1, 1.0f};
            glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(leftRect), leftRect, GL_DYNAMIC_DRAW);
            glUniform1f(m_box_u_orientation, 6.0f);
            glDepthMask(GL_TRUE);  // self-occlude overlapping box faces (safe: box fragments are opaque)
            glDrawArraysInstanced(GL_TRIANGLES, 0, m_box_vertex_count, 1);
            glDepthMask(GL_FALSE);

            // Right wall region: right split_px_x columns.
            const float rightRect[4] = {right_u0, 0.0f, 1.0f, 1.0f};
            glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(rightRect), rightRect, GL_DYNAMIC_DRAW);
            glUniform1f(m_box_u_orientation, 7.0f);
            glDepthMask(GL_TRUE);  // self-occlude overlapping box faces (safe: box fragments are opaque)
            glDrawArraysInstanced(GL_TRIANGLES, 0, m_box_vertex_count, 1);
            glDepthMask(GL_FALSE);

            glUseProgram(layer_program);
            glBindVertexArray(immersive_active ? m_curve_vao : m_vao);
            // Front face last: the original flat composite (wall+ceiling+floor+left+right as
            // originally drawn), closest to the viewer, on top of all five regions' boxes.
            glDrawArrays(GL_TRIANGLES, 0, layer_vertex_count);
        } else if (wedge_active && state.real_geometry_boxes && m_box_program != 0) {
            // Whole-layer real geometry: one "box" for the entire layer instead of per-detected-
            // object rects — no connected-component border detection needed. The footprint is
            // trimmed to the layer's actual content (LayerFrame::content_bounds, a single global
            // min/max scan — not per-object clustering) so a small sprite on an otherwise-empty
            // full-frame layer gets a box hugging its silhouette, not the padded frame; side
            // faces then sample real edge pixels instead of empty transparency. Transparent
            // pixels still discard in the fragment shader exactly as the front face does.
            const std::array<float, 4> kFullRect = fr.content_bounds_valid
                ? std::array<float, 4>{
                      (float)fr.content_bounds.min_x / (float)fr.width,
                      (float)fr.content_bounds.min_y / (float)fr.height,
                      (float)(fr.content_bounds.max_x + 1) / (float)fr.width,
                      (float)(fr.content_bounds.max_y + 1) / (float)fr.height}
                : std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f};
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_object_box_ssbo);
            glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(float) * 4, kFullRect.data(), GL_DYNAMIC_DRAW);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_object_box_ssbo);

            const float thickness = (fr.box_thickness_meters != 0.0f) ? fr.box_thickness_meters
                                   : (copy_span > 0.0f ? copy_span : (k_max_copies * k_default_copy_step));
            const float orientation = (fr.geometry_mode == LayerGeometryMode::Floor)   ? 1.0f
                                     : (fr.geometry_mode == LayerGeometryMode::Ceiling) ? 2.0f
                                                                                         : 0.0f;
            glUseProgram(m_box_program);
            glBindVertexArray(m_box_vao);
            glUniformMatrix4fv(m_box_u_vp, 1, GL_FALSE, vp.data());
            glUniform1f(m_box_u_depth,        fr.depth_meters);
            glUniform1f(m_box_u_quad_w,       qw);
            glUniform1f(m_box_u_quad_h,       qh);
            glUniform1f(m_box_u_quad_y,       eff_quad_y);
            if (m_box_u_table_mode >= 0)
                glUniform1f(m_box_u_table_mode, (float)state.surface_mode);
            glUniform1f(m_box_u_thickness,    thickness);
            glUniform1f(m_box_u_screen_curve, immersive_active ? state.screen_curve : 0.0f);
            glUniform1f(m_box_u_orientation,  orientation);
            glUniform1f(m_box_u_ref_l1_depth, ref_l1_depth);
            glUniform1f(m_box_u_auto_thickness, 0.0f);
            glUniform1f(m_box_u_scatter_range, 0.0f);
            glUniform1f(m_box_u_y_depth_range, 0.0f);
            glUniform1f(m_box_u_size_thickness_mode, 0.0f);
            glUniform1f(m_box_u_tilt_x,       state.tilt_x);
            glUniform1f(m_box_u_tilt_y,       state.tilt_y);
            glUniform1f(m_box_u_canvas_x,     snapped_canvas_x);
            glUniform1f(m_box_u_canvas_y,     snapped_canvas_y);
            glUniform1f(m_box_u_canvas_az,    layer_canvas_az);
            if (m_box_u_layer_yaw >= 0) glUniform1f(m_box_u_layer_yaw, layer_yaw);
            glUniform1f(m_box_u_canvas_el,    canvas_el);
            glUniform1f(m_box_u_canvas_scale, snapped_canvas_scale);
            glUniform1f(m_box_u_gamma,        state.gamma);
            glUniform1f(m_box_u_contrast,     state.contrast);
            glUniform1f(m_box_u_saturation,   state.saturation);
            glUniform1f(m_box_u_brightness,   state.brightness);
            glUniform1f(m_box_u_force_opaque_alpha, passthrough_alpha ? 1.0f : 0.0f);
            glUniform1i(m_box_u_texture, 0);
            const bool use_silhouette_wl = state.silhouette_sides
                && i < (int)m_layers.size() && m_layers[i].edge_lr_tex && m_layers[i].edge_tb_tex;
            glUniform1f(m_box_u_silhouette, use_silhouette_wl ? 1.0f : 0.0f);
            {
                float scr, scg, scb;
                const bool side_darken = fr.side_color_mode == 6;
                side_color_mode_to_rgb(side_darken ? 0 : fr.side_color_mode, scr, scg, scb);
                glUniform1f(m_box_u_side_color_mode, (fr.side_color_mode > 0 && !side_darken) ? 1.0f : 0.0f);
                glUniform1f(m_box_u_side_color_darken, side_darken ? 1.0f : 0.0f);
                glUniform3f(m_box_u_side_color_rgb, scr, scg, scb);
            }
            if (use_silhouette_wl) {
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, m_layers[i].edge_lr_tex);
                glActiveTexture(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_2D, m_layers[i].edge_tb_tex);
                glActiveTexture(GL_TEXTURE0);
                glUniform1i(m_box_u_edge_lr, 2);
                glUniform1i(m_box_u_edge_tb, 3);
            }

            glDepthMask(GL_TRUE);  // self-occlude overlapping box faces (safe: box fragments are opaque)
            glDrawArraysInstanced(GL_TRIANGLES, 0, m_box_vertex_count, 1);
            glDepthMask(GL_FALSE);

            glUseProgram(layer_program);
            glBindVertexArray(immersive_active ? m_curve_vao : m_vao);
            // Front face last: original layer at uDepth (closest to viewer), composites on top.
            glDrawArrays(GL_TRIANGLES, 0, layer_vertex_count);
        } else if (wedge_active) {
            glDrawArraysInstanced(GL_TRIANGLES, 0, layer_vertex_count, copy_count + 1);
        } else {
            if (copy_count > 0) {
                glDrawArraysInstanced(GL_TRIANGLES, 0, layer_vertex_count, copy_count + 1);
            } else {
                glDrawArrays(GL_TRIANGLES, 0, layer_vertex_count);
            }
        }
        draw_ms_total += std::chrono::duration<float, std::milli>(Clock::now() - draw_start).count();

    }

    static int genesis_render_log_ctr = 0;
    const bool looks_like_genesis =
        frames.size() == 7 && frames[0] != nullptr && frames[0]->id == "background";
    if (looks_like_genesis &&
        (++genesis_render_log_ctr % 240 == 0 || upload_ms_total > 2.0f || draw_ms_total > 6.0f)) {
        LOGI("Genesis perf: xr_upload=%.2f ms xr_draw=%.2f ms uploaded_layers=%d visible_layers=%zu",
             upload_ms_total, draw_ms_total, uploaded_layers, frames.size());
    }

    glBindVertexArray(0);

    } // end game-layers block

    // Overlay: panels + lasers (always drawn, even when no game frame)
    if (overlay) {
        if (!overlay->live_layer_guides.empty()) draw_live_layer_guides(*overlay, vp);
        if (overlay->panel_count > 0) draw_panel(*overlay, vp, hmd_x, hmd_y, hmd_z);
        if (overlay->show_laser)      draw_laser(*overlay, vp);
        if (overlay->show_laser2)     draw_laser2(*overlay, vp);
        if (overlay->show_gun)        draw_gun_model(*overlay, vp);
        if (overlay->show_gun2)       draw_gun_model(*overlay, vp, &overlay->gun2_pose);
        if (overlay->show_gun)        draw_scope_zoom(*overlay, vp, fbo, hmd_x, hmd_y, hmd_z);
        if (overlay->show_calibration_target) draw_calibration_target(*overlay, vp);
        if (overlay->show_controller_models)  draw_controller_model(*overlay, vp);
    }

    // Keep this last: a ROM transition must cover both the old menu and the
    // first frame of the new core, including controller lasers and panels.
    draw_fade(fbo, fade_alpha);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GlesRenderer::draw_fade(const EyeFbo& fbo, float alpha) {
    if (alpha <= 0.001f || !m_flat_prog || !m_flat_vao || !m_flat_vbo ||
        fbo.width <= 0 || fbo.height <= 0) return;

    // Reuse the flat shader's dynamic position/alpha buffer for a cheap
    // fullscreen pass. Its VP uniform is identity, so these are clip-space
    // coordinates and do not depend on the user's world/canvas placement.
    static constexpr float kQuad[] = {
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 0.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 0.0f, 1.0f,
    };
    const Mat4 identity = Mat4::identity();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
    glViewport(0, 0, fbo.width, fbo.height);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(m_flat_prog);
    glUniformMatrix4fv(m_flat_u_vp, 1, GL_FALSE, identity.data());
    glUniform4f(m_flat_u_color, 0.0f, 0.0f, 0.0f, std::clamp(alpha, 0.0f, 1.0f));
    glBindVertexArray(m_flat_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_flat_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(kQuad), kQuad);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}
