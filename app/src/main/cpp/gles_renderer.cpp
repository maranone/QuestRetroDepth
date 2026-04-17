#include "gles_renderer.h"
#include <android/log.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>

#define LOG_TAG "QrdGles"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// GLSL shaders
// ---------------------------------------------------------------------------

static const char* kLayerVS = R"GLSL(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;

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
uniform float uScreenCurve;
uniform float uSubrectEnable;
uniform vec4  uSubrect;
uniform float uInstanceBase;
// Canvas placement (edit mode)
uniform float uCanvasX;   // horizontal translation (metres)
uniform float uCanvasY;   // vertical translation (metres)
uniform float uCanvasAz;  // azimuth arc angle (radians)
uniform float uCanvasEl;  // elevation arc angle (radians)
uniform float uCanvasScale;

out vec2  vUV;
out float vCopyT;

void main() {
    float copy_count = max(1.0, uCopyCount);
    // BBox: reverse instance order so the deepest copy (gl_InstanceID=0) is drawn first
    // and the shallowest last — correct back-to-front order for alpha compositing.
    // Normal: standard front-to-back (closest to viewer last so it composites on top).
    float inst = uBboxMode > 0.5 ? (copy_count - float(gl_InstanceID))
                                 : (float(gl_InstanceID) + uInstanceBase);
    float t = min(inst, copy_count) / copy_count;
    float offset = t * uCopySpan;
    // BBox mode: copies go DEEPER (into the screen) so the front face stays closest to
    // the viewer and the extrusion recedes away, making the object look 3D going inward.
    // Normal mode: copies come toward the viewer (retrodepth pop-out effect).
    float d = uBboxMode > 0.5 ? max(0.01, uDepth + offset) : max(0.01, uDepth - offset);

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

    // Spherical arc: canvas centre moves on sphere of radius d and rotates to
    // face the viewer (like a screen on the inside of a sphere).
    float cos_el = cos(uCanvasEl);
    float sin_el = sin(uCanvasEl);
    float cos_az = cos(uCanvasAz);
    float sin_az = sin(uCanvasAz);

    // Canvas centre position on sphere
    vec3 center = vec3(d * sin_az * cos_el + uCanvasX,
                       d * sin_el          + uQuadY + uCanvasY,
                      -d * cos_az * cos_el);

    // Outward normal (HMD → canvas centre)
    vec3 normal = vec3(sin_az * cos_el, sin_el, -cos_az * cos_el);

    // Tangent axes: right and up in the canvas plane
    vec3 right = vec3(cos_az,           0.0,     sin_az);
    vec3 up    = vec3(-sin_az * sin_el, cos_el, -cos_az * sin_el);

    float sub_w = mix(1.0, uSubrect.z - uSubrect.x, uSubrectEnable);
    float sub_h = mix(1.0, uSubrect.w - uSubrect.y, uSubrectEnable);
    float sub_cx = mix(0.5, 0.5 * (uSubrect.x + uSubrect.z), uSubrectEnable);
    float sub_cy = mix(0.5, 0.5 * (uSubrect.y + uSubrect.w), uSubrectEnable);
    float center_dx = (sub_cx - 0.5) * uQuadW * uCanvasScale;
    float center_dy = (0.5 - sub_cy) * uQuadH * uCanvasScale;

    float vx = aPos.x * uQuadW * scale_x * scale * uCanvasScale * sub_w;
    float vy = aPos.y * uQuadH *           scale * uCanvasScale * sub_h;

    // Screen curve pushes vertices along the outward normal
    gl_Position = uVP * vec4(center + right * (center_dx + vx) + up * (center_dy + vy) + normal * curve_offset, 1.0);
    vUV    = mix(aUV, mix(uSubrect.xy, uSubrect.zw, aUV), uSubrectEnable);
    vCopyT = t;
}
)GLSL";

static const char* kImmersiveLayerVS = R"GLSL(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;

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
uniform float uScreenCurve;
uniform float uTiltX;
uniform float uTiltY;
uniform float uSubrectEnable;
uniform vec4  uSubrect;
uniform float uInstanceBase;
uniform float uCanvasX;
uniform float uCanvasY;
uniform float uCanvasAz;
uniform float uCanvasEl;
uniform float uCanvasScale;

out vec2  vUV;
out float vCopyT;

void main() {
    float copy_count = max(1.0, uCopyCount);
    float inst = uBboxMode > 0.5 ? (copy_count - float(gl_InstanceID))
                                 : (float(gl_InstanceID) + uInstanceBase);
    float t = min(inst, copy_count) / copy_count;
    float offset = t * uCopySpan;
    float d = uBboxMode > 0.5 ? max(0.01, uDepth + offset) : max(0.01, uDepth - offset);

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

    float ctx = cos(uTiltX);
    float stx = sin(uTiltX);
    vec3 pitched_up     = up * ctx + normal * stx;
    vec3 pitched_normal = normal * ctx - up * stx;

    float cty = cos(uTiltY);
    float sty = sin(uTiltY);
    vec3 tilted_right  = right * cty - pitched_normal * sty;
    vec3 tilted_normal = right * sty + pitched_normal * cty;
    vec3 tilted_up     = pitched_up;

    float sub_w = mix(1.0, uSubrect.z - uSubrect.x, uSubrectEnable);
    float sub_h = mix(1.0, uSubrect.w - uSubrect.y, uSubrectEnable);
    float sub_cx = mix(0.5, 0.5 * (uSubrect.x + uSubrect.z), uSubrectEnable);
    float sub_cy = mix(0.5, 0.5 * (uSubrect.y + uSubrect.w), uSubrectEnable);
    float center_dx = (sub_cx - 0.5) * uQuadW * uCanvasScale;
    float center_dy = (0.5 - sub_cy) * uQuadH * uCanvasScale;

    float vx = aPos.x * uQuadW * scale_x * scale * uCanvasScale * sub_w;
    float vy = aPos.y * uQuadH *           scale * uCanvasScale * sub_h;

    // The strip mesh supplies enough vertices for this depth shift to read as a curve.
    float edge_t = aPos.x * 2.0;
    float curve_offset = uScreenCurve * uQuadW * 0.18 * edge_t * edge_t;

    gl_Position = uVP * vec4(center + tilted_right * (center_dx + vx) + tilted_up * (center_dy + vy) + tilted_normal * curve_offset, 1.0);
    vUV    = mix(aUV, mix(uSubrect.xy, uSubrect.zw, aUV), uSubrectEnable);
    vCopyT = t;
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
uniform float uCopyCount;
uniform float uSolidStack;  // 1.0 = fill transparent copy pixels with dark extrusion colour
uniform float uForceOpaqueAlpha; // 1.0 = visible game pixels write full compositor alpha
uniform float uBboxMode; // 1.0 = apply bbox-centered width shrink on copy instances
uniform float uBboxDebug; // 1.0 = tint bbox copy instances per detected object
uniform int uObjectBoxCount;
uniform vec4 uObjectBoxes[64];

in vec2  vUV;
in float vCopyT;
out vec4 fragColor;

vec4 sampleLayer(vec2 uv) {
    if (uUpscale > 0.5) {
        // Sharp-bilinear ("pixel-perfect") upscale:
        // Stay on the source pixel center for most of the pixel area; only blend
        // in a 1-output-pixel-wide border region at each edge.  This gives crisp
        // pixel art without the blocky hard edge of GL_NEAREST.
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
        vec2 suv   = (floor(p) + sharp) / tsz;
        return texture(uTexture, suv);
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
in  vec2 vUV;
out vec4 fragColor;
void main() {
    vec4 c = texture(uTexture, vUV);
    fragColor = vec4(c.rgb, c.a * uAlpha);
}
)GLSL";

static const char* kSkyVS = R"GLSL(#version 300 es
layout(location = 0) in vec3 aPos;
uniform mat4 uProj;
out float vSphereY;
void main() {
    gl_Position = uProj * vec4(aPos, 1.0);
    vSphereY = clamp(aPos.y / 18.0, -1.0, 1.0);
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

void main() {
    float t = vSphereY * 0.5 + 0.5;
    vec4 env = sampleBands(t);
    float abs_y = abs(vSphereY);
    float horizon_fade = smoothstep(0.10, 0.35, abs_y);
    float upper_boost = mix(0.70, 1.0, smoothstep(0.15, 1.0, max(vSphereY, 0.0)));
    float alpha = env.a * horizon_fade * upper_boost;
    if (uMode == 1 && vSphereY < 0.0) {
        alpha = 0.0;
    }
    fragColor = vec4(env.rgb, alpha);
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
    m_u_texture      = glGetUniformLocation(m_program, "uTexture");
    m_u_canvas_x     = glGetUniformLocation(m_program, "uCanvasX");
    m_u_canvas_y     = glGetUniformLocation(m_program, "uCanvasY");
    m_u_canvas_az    = glGetUniformLocation(m_program, "uCanvasAz");
    m_u_canvas_el    = glGetUniformLocation(m_program, "uCanvasEl");
    m_u_canvas_scale = glGetUniformLocation(m_program, "uCanvasScale");
    m_u_solid_stack  = glGetUniformLocation(m_program, "uSolidStack");
    m_u_force_opaque_alpha = glGetUniformLocation(m_program, "uForceOpaqueAlpha");
    m_u_bbox_mode    = glGetUniformLocation(m_program, "uBboxMode");
    m_u_bbox_debug   = glGetUniformLocation(m_program, "uBboxDebug");
    m_u_subrect_enable = glGetUniformLocation(m_program, "uSubrectEnable");
    m_u_subrect      = glGetUniformLocation(m_program, "uSubrect");
    m_u_instance_base = glGetUniformLocation(m_program, "uInstanceBase");
    m_u_object_box_count = glGetUniformLocation(m_program, "uObjectBoxCount");
    m_u_object_boxes = glGetUniformLocation(m_program, "uObjectBoxes[0]");
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
    m_i_u_texture      = glGetUniformLocation(m_immersive_program, "uTexture");
    m_i_u_canvas_x     = glGetUniformLocation(m_immersive_program, "uCanvasX");
    m_i_u_canvas_y     = glGetUniformLocation(m_immersive_program, "uCanvasY");
    m_i_u_canvas_az    = glGetUniformLocation(m_immersive_program, "uCanvasAz");
    m_i_u_canvas_el    = glGetUniformLocation(m_immersive_program, "uCanvasEl");
    m_i_u_canvas_scale = glGetUniformLocation(m_immersive_program, "uCanvasScale");
    m_i_u_solid_stack  = glGetUniformLocation(m_immersive_program, "uSolidStack");
    m_i_u_force_opaque_alpha = glGetUniformLocation(m_immersive_program, "uForceOpaqueAlpha");
    m_i_u_bbox_mode    = glGetUniformLocation(m_immersive_program, "uBboxMode");
    m_i_u_bbox_debug   = glGetUniformLocation(m_immersive_program, "uBboxDebug");
    m_i_u_subrect_enable = glGetUniformLocation(m_immersive_program, "uSubrectEnable");
    m_i_u_subrect      = glGetUniformLocation(m_immersive_program, "uSubrect");
    m_i_u_instance_base = glGetUniformLocation(m_immersive_program, "uInstanceBase");
    m_i_u_object_box_count = glGetUniformLocation(m_immersive_program, "uObjectBoxCount");
    m_i_u_object_boxes = glGetUniformLocation(m_immersive_program, "uObjectBoxes[0]");
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
    m_sky_u_bands = glGetUniformLocation(m_sky_prog, "uBands[0]");
    m_sky_u_mode = glGetUniformLocation(m_sky_prog, "uMode");
    return true;
}

bool GlesRenderer::init(std::string& error_out) {
    if (!init_layer_program(error_out)) return false;
    if (!init_flat_program(error_out))  return false;
    if (!init_sky_program(error_out))   return false;
    if (!init_ui_program(error_out))    return false;
    std::string immersive_err;
    if (!init_immersive_layer_program(immersive_err)) {
        LOGE("Immersive layer program unavailable; using flat fallback: %s", immersive_err.c_str());
        if (m_immersive_program) { glDeleteProgram(m_immersive_program); m_immersive_program = 0; }
    }

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

void GlesRenderer::shutdown() {
    for (auto& lt : m_layers) {
        if (lt.tex) { glDeleteTextures(1, &lt.tex); lt.tex = 0; }
    }
    m_layers.clear();
    if (m_vbo)       { glDeleteBuffers(1, &m_vbo);       m_vbo       = 0; }
    if (m_vao)       { glDeleteVertexArrays(1, &m_vao);  m_vao       = 0; }
    if (m_curve_vbo) { glDeleteBuffers(1, &m_curve_vbo); m_curve_vbo = 0; }
    if (m_curve_vao) { glDeleteVertexArrays(1, &m_curve_vao); m_curve_vao = 0; }
    if (m_flat_vbo)  { glDeleteBuffers(1, &m_flat_vbo);  m_flat_vbo  = 0; }
    if (m_flat_vao)  { glDeleteVertexArrays(1, &m_flat_vao); m_flat_vao = 0; }
    if (m_sky_vbo)   { glDeleteBuffers(1, &m_sky_vbo);   m_sky_vbo   = 0; }
    if (m_sky_vao)   { glDeleteVertexArrays(1, &m_sky_vao); m_sky_vao = 0; }
    if (m_program)   { glDeleteProgram(m_program);       m_program   = 0; }
    if (m_immersive_program) { glDeleteProgram(m_immersive_program); m_immersive_program = 0; }
    if (m_flat_prog) { glDeleteProgram(m_flat_prog);     m_flat_prog = 0; }
    if (m_sky_prog)  { glDeleteProgram(m_sky_prog);      m_sky_prog  = 0; }
    if (m_ui_vbo)    { glDeleteBuffers(1, &m_ui_vbo);    m_ui_vbo    = 0; }
    if (m_ui_vao)    { glDeleteVertexArrays(1, &m_ui_vao); m_ui_vao  = 0; }
    if (m_ui_prog)   { glDeleteProgram(m_ui_prog);       m_ui_prog   = 0; }
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

void GlesRenderer::update_layer(int idx, const LayerFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.rgba.empty()) return;
    resize_layers(idx + 1);
    auto& lt = m_layers[idx];
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
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    glBindTexture(GL_TEXTURE_2D, 0);
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
    const float cx = 0.0f, cy = qy, cz = -depth;
    const float far = 40.0f;         // extend 40 m → covers ~87° from centre at 2 m depth
    const float base_alpha = 0.38f;  // peak glow strength at screen centre

    // 4 triangles as a fan (centre → adjacent corner pairs); 12 verts × 4 floats (xyz+a)
    float verts[] = {
        // top fan
        cx,     cy,     cz, base_alpha,
        cx-far, cy+far, cz, 0.0f,
        cx+far, cy+far, cz, 0.0f,
        // right fan
        cx,     cy,     cz, base_alpha,
        cx+far, cy+far, cz, 0.0f,
        cx+far, cy-far, cz, 0.0f,
        // bottom fan
        cx,     cy,     cz, base_alpha,
        cx+far, cy-far, cz, 0.0f,
        cx-far, cy-far, cz, 0.0f,
        // left fan
        cx,     cy,     cz, base_alpha,
        cx-far, cy-far, cz, 0.0f,
        cx-far, cy+far, cz, 0.0f,
    };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glUniform4f(m_flat_u_color, r, g, b, 1.0f);  // rgb colour; alpha comes from per-vertex
    glDrawArrays(GL_TRIANGLES, 0, 12);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// Shadow helper — dark gradient below each layer
// ---------------------------------------------------------------------------

void GlesRenderer::draw_shadow(const LayerFrame& frame, const Mat4& vp, const VrState& /*state*/) {
    if (m_flat_prog == 0 || frame.width <= 0) return;
    const float qw  = frame.quad_width_meters;
    const float qh  = qw * (float)frame.height / (float)frame.width;
    const float qy  = 0.0f; // eye level
    const float z   = -(frame.depth_meters + 0.01f); // just behind
    const float bot = qy - qh * 0.5f;
    const float hw  = qw * 0.5f * 1.05f;
    const float sh  = qh * 0.18f;

    glUseProgram(m_flat_prog);
    glUniformMatrix4fv(m_flat_u_vp, 1, GL_FALSE, vp.data());
    glBindVertexArray(m_flat_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_flat_vbo);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    float verts[] = {
        -hw, bot,       z, 1.f,
         hw, bot,       z, 1.f,
        -hw, bot - sh,  z, 1.f,
        -hw, bot - sh,  z, 1.f,
         hw, bot,       z, 1.f,
         hw, bot - sh,  z, 1.f,
    };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glUniform4f(m_flat_u_color, 0.0f, 0.0f, 0.0f, 0.35f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// draw_panel — world-space textured quad for the ROM browser
// ---------------------------------------------------------------------------
void GlesRenderer::draw_panel(const OverlayInfo& ov, const Mat4& vp) {
    if (!m_ui_prog || ov.panel_count == 0) return;

    glUseProgram(m_ui_prog);
    glUniformMatrix4fv(m_ui_u_vp, 1, GL_FALSE, vp.data());
    glUniform1i(m_ui_u_texture, 0);
    glUniform1f(m_ui_u_alpha,   1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glBindVertexArray(m_ui_vao);
    glActiveTexture(GL_TEXTURE0);

    for (int pi = 0; pi < ov.panel_count; ++pi) {
        const PanelInfo& p = ov.panels[pi];
        if (!p.tex) continue;

        Mat4 scale;
        scale.m[0] = p.w;
        scale.m[5] = p.h;
        const Mat4 model = Mat4::mul(Mat4::from_pose(p.pose), scale);
        glUniformMatrix4fv(m_ui_u_model, 1, GL_FALSE, model.data());
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

void GlesRenderer::draw_sky_dome(const Mat4& proj, const SkyDomeInfo& info) {
    if (!info.enabled || !m_sky_prog || !m_sky_vao || m_sky_vertex_count <= 0) return;
    glUseProgram(m_sky_prog);
    glUniformMatrix4fv(m_sky_u_proj, 1, GL_FALSE, proj.data());
    glUniform4fv(m_sky_u_bands, (GLsizei)info.bands.size(), info.bands[0].data());
    glUniform1i(m_sky_u_mode, info.mode == EnvironmentSphereMode::FullSphere ? 2 :
                              info.mode == EnvironmentSphereMode::SkyOnly ? 1 : 0);
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
                               bool passthrough_alpha) {
    const Mat4 vp = Mat4::mul(proj, view);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo.fbo);
    glViewport(0, 0, fbo.width, fbo.height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glClearColor(bg_r, bg_g, bg_b, bg_a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (sky_dome && sky_dome->enabled) {
        draw_sky_dome(proj, *sky_dome);
    }

    // Game layers (skip if no program or no frames, but still draw overlay below)
    if (m_program && !frames.empty()) {

    // Ambilight behind everything
    if (state.ambilight) draw_ambilight(frames, vp, state);

    // Upload textures and render layers back-to-front
    resize_layers((int)frames.size());

    const bool immersive_active = state.immersive_beta_enabled &&
                                  m_immersive_program != 0 &&
                                  m_curve_vao != 0 &&
                                  m_curve_vertex_count > 0;
    const GLuint layer_program = immersive_active ? m_immersive_program : m_program;
    const GLint u_vp           = immersive_active ? m_i_u_vp           : m_u_vp;
    const GLint u_depth        = immersive_active ? m_i_u_depth        : m_u_depth;
    const GLint u_quad_w       = immersive_active ? m_i_u_quad_w       : m_u_quad_w;
    const GLint u_quad_h       = immersive_active ? m_i_u_quad_h       : m_u_quad_h;
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
    const GLint u_texture      = immersive_active ? m_i_u_texture      : m_u_texture;
    const GLint u_canvas_x     = immersive_active ? m_i_u_canvas_x     : m_u_canvas_x;
    const GLint u_canvas_y     = immersive_active ? m_i_u_canvas_y     : m_u_canvas_y;
    const GLint u_canvas_az    = immersive_active ? m_i_u_canvas_az    : m_u_canvas_az;
    const GLint u_canvas_el    = immersive_active ? m_i_u_canvas_el    : m_u_canvas_el;
    const GLint u_canvas_scale = immersive_active ? m_i_u_canvas_scale : m_u_canvas_scale;
    const GLint u_solid_stack  = immersive_active ? m_i_u_solid_stack  : m_u_solid_stack;
    const GLint u_force_opaque_alpha = immersive_active ? m_i_u_force_opaque_alpha : m_u_force_opaque_alpha;
    const GLint u_bbox_mode    = immersive_active ? m_i_u_bbox_mode : m_u_bbox_mode;
    const GLint u_bbox_debug   = immersive_active ? m_i_u_bbox_debug : m_u_bbox_debug;
    const GLint u_subrect_enable = immersive_active ? m_i_u_subrect_enable : m_u_subrect_enable;
    const GLint u_subrect = immersive_active ? m_i_u_subrect : m_u_subrect;
    const GLint u_instance_base = immersive_active ? m_i_u_instance_base : m_u_instance_base;
    const GLint u_object_box_count = immersive_active ? m_i_u_object_box_count : m_u_object_box_count;
    const GLint u_object_boxes = immersive_active ? m_i_u_object_boxes : m_u_object_boxes;
    const int layer_vertex_count = immersive_active ? m_curve_vertex_count : 6;

    // Integer-scale + pixel-grid snap (upscale mode only).
    // All layers share the same canvas_x/y/scale uniforms, so we snap once using
    // layer 0's pixel size as the reference grid.  The goal:
    //   • canvas_scale is rounded to the nearest integer pixel multiplier
    //     (x1, x2, x3 … — no fractional scales that blur GL_NEAREST output)
    //   • canvas_x/y are snapped to the nearest whole source-pixel boundary
    //     so the quad never sits between pixels during head movement
    float snapped_canvas_x     = canvas_x;
    float snapped_canvas_y     = canvas_y;
    float snapped_canvas_scale = canvas_scale;
    if (state.upscale) {
        // Find first valid frame to derive the pixel-size grid.
        for (const LayerFrame* fp : frames) {
            if (!fp || fp->width <= 0 || fp->quad_width_meters <= 0.0f) continue;
            // px_size: physical width of one source pixel in metres
            const float px_size = fp->quad_width_meters / (float)fp->width;
            // Snap canvas_scale: round to nearest integer multiplier.
            // scale * qw / qw = scale; integer multiplier = round(scale)
            // Guard against round-to-zero.
            const float int_scale = std::max(1.0f, std::round(canvas_scale));
            snapped_canvas_scale = int_scale;
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
    glUniform1f(u_upscale,      state.upscale ? 1.0f : 0.0f);
    glUniform1f(u_depthmap,     state.depth_mode == DepthMode::WholeLayer ? 1.0f : 0.0f);
    glUniform1f(u_gamma,        state.gamma);
    glUniform1f(u_contrast,     state.contrast);
    glUniform1f(u_saturation,   state.saturation);
    glUniform1f(u_brightness,   state.brightness);
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
    glUniform1f(u_canvas_el,    canvas_el);
    glUniform1f(u_canvas_scale, snapped_canvas_scale);
    glUniform1i(u_texture, 0);
    glUniform1f(u_bbox_mode, 0.0f);
    glUniform1f(u_bbox_debug, 0.0f);
    glUniform1f(u_subrect_enable, 0.0f);
    glUniform4f(u_subrect, 0.0f, 0.0f, 1.0f, 1.0f);
    glUniform1f(u_instance_base, 0.0f);
    glUniform1i(u_object_box_count, 0);

    glBindVertexArray(immersive_active ? m_curve_vao : m_vao);
    glActiveTexture(GL_TEXTURE0);

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
        if (fr.width <= 0 || fr.height <= 0 || fr.rgba.empty() || !fr.has_pixels) continue;
        const bool wedge_active = state.depth_mode == DepthMode::WholeLayer && fr.wedge_eligible;
        const bool bbox_active = state.depth_mode == DepthMode::BoundingBox &&
                                 fr.bbox_eligible &&
                                 !fr.object_boxes.empty();

        update_layer(i, fr);

        const float qw = fr.quad_width_meters;
        const float qh = qw * (float)fr.height / (float)fr.width;

        int   copy_count;
        float copy_span;
        if (fr.copies.empty()) {
            copy_count = k_max_copies;
            copy_span  = (float)k_max_copies * k_default_copy_step;
        } else {
            copy_count = (int)fr.copies.size();
            copy_span  = fr.copies.back();
        }

        glUniform1f(u_depth,      fr.depth_meters);
        glUniform1f(u_quad_w,     qw);
        glUniform1f(u_quad_h,     qh);
        glUniform1f(u_quad_y,     0.0f);   // eye level (app_space origin = HMD position)
        glUniform1f(u_copy_count, (float)copy_count);
        glUniform1f(u_copy_span,  copy_span);
        glUniform1f(u_roundness,  wedge_active ? 1.0f : 0.0f);
        glUniform1f(u_bbox_mode, 0.0f);
        glUniform1f(u_bbox_debug, 0.0f);

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

        glUniform1f(u_instance_base, 0.0f);
        glUniform1i(u_object_box_count, 0);

        glBindTexture(GL_TEXTURE_2D, m_layers[i].tex);
        // Upscale shader needs smooth input; otherwise keep crisp nearest-neighbour.
        {
            GLint f = state.upscale ? GL_LINEAR : GL_NEAREST;
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

        if (bbox_active) {
            // Draw per-object extrusion copies FIRST (they go deeper into the screen).
            // The original front face is drawn LAST so it composites on top — it is the
            // closest layer to the viewer and must win the alpha blend.
            if (copy_count > 0) {
                glUniform1f(u_depthmap, 1.0f);
                glUniform1f(u_roundness, 1.0f);
                glUniform1f(u_bbox_mode, 1.0f);
                glUniform1f(u_bbox_debug, 0.0f);
                glUniform1f(u_instance_base, 1.0f);
                glUniform1i(u_object_box_count, 1);

                for (std::size_t bi = 0; bi < fr.object_boxes.size() && bi < GlesRenderer::k_max_object_boxes; ++bi) {
                    const ObjectBoundingBox& box = fr.object_boxes[bi];
                    const float inv_w = 1.0f / (float)fr.width;
                    const float inv_h = 1.0f / (float)fr.height;
                    const float u0 = (float)box.min_x * inv_w;
                    const float v0 = (float)box.min_y * inv_h;
                    const float u1 = (float)(box.max_x + 1) * inv_w;
                    const float v1 = (float)(box.max_y + 1) * inv_h;
                    glUniform1f(u_subrect_enable, 1.0f);
                    glUniform4f(u_subrect, u0, v0, u1, v1);
                    glDrawArraysInstanced(GL_TRIANGLES, 0, layer_vertex_count, copy_count);
                }

                glUniform1f(u_depthmap, state.depth_mode == DepthMode::WholeLayer ? 1.0f : 0.0f);
                glUniform1f(u_roundness, wedge_active ? 1.0f : 0.0f);
                glUniform1f(u_bbox_mode, 0.0f);
                glUniform1f(u_bbox_debug, 0.0f);
                glUniform1f(u_subrect_enable, 0.0f);
                glUniform4f(u_subrect, 0.0f, 0.0f, 1.0f, 1.0f);
                glUniform1f(u_instance_base, 0.0f);
                glUniform1i(u_object_box_count, 0);
            }

            // Front face last: original layer at uDepth (closest to viewer), composites on top.
            glDrawArrays(GL_TRIANGLES, 0, layer_vertex_count);
        } else if (state.layers_3d) {
            glDrawArraysInstanced(GL_TRIANGLES, 0, layer_vertex_count, copy_count + 1);
        } else if (wedge_active) {
            glDrawArraysInstanced(GL_TRIANGLES, 0, layer_vertex_count, copy_count + 1);
        } else {
            if (copy_count > 0) {
                glDrawArraysInstanced(GL_TRIANGLES, 0, layer_vertex_count, copy_count + 1);
            } else {
                glDrawArrays(GL_TRIANGLES, 0, layer_vertex_count);
            }
        }

    }

    glBindVertexArray(0);

    } // end game-layers block

    // Overlay: panels + lasers (always drawn, even when no game frame)
    if (overlay) {
        if (overlay->panel_count > 0) draw_panel(*overlay, vp);
        if (overlay->show_laser)      draw_laser(*overlay, vp);
        if (overlay->show_laser2)     draw_laser2(*overlay, vp);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
