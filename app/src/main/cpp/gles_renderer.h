#pragma once
#include <GLES3/gl3.h>
#include <GLES3/gl31.h>
#include <vector>
#include <string>
#include "layer_processor.h"
#include "vr_state.h"
#include "mat4.h"
#include <openxr/openxr.h>

// One panel in world space (ROM browser, layer list, settings, …)
struct PanelInfo {
    GLuint     tex  = 0;              // 0 = skip
    XrPosef    pose = {{0,0,0,1},{0,0,0}};
    float      w    = 0.60f;          // metres
    float      h    = 0.50f;
};

// Highlight overlay on a panel (hovered row)
struct PanelHighlight {
    int    panel_idx = -1;       // which panel in OverlayInfo::panels
    float  u0 = 0, v0 = 0;       // UV bounds of highlight
    float  u1 = 0, v1 = 0;
    float  r = 0.18f, g = 0.39f, b = 0.75f; // highlight color (default blue)
    float  alpha = 0.35f;
};

// All overlay elements drawn on top of the game layers
struct OverlayInfo {
    static constexpr int k_max_panels = 6;
    PanelInfo  panels[k_max_panels];
    int        panel_count = 0;

    PanelHighlight highlight;    // single hover highlight quad

    bool       show_laser   = false;
    XrVector3f laser_origin = {0,0,0};
    XrVector3f laser_end    = {0,0,0};
    XrVector3f laser_eye    = {0,0,0};  // camera position (for billboard)
    bool       laser_hit    = false;    // laser intersected a panel?
    float      laser_hit_u  = 0.0f;    // UV on hit panel (0-1)
    float      laser_hit_v  = 0.0f;
    int        laser_hit_panel = -1;   // which panel index was hit

    // Second laser (edit mode: left controller translation / right controller sphere)
    bool       show_laser2    = false;
    XrVector3f laser2_origin  = {0,0,0};
    XrVector3f laser2_end     = {0,0,0};
};

// One GPU texture per game layer
struct LayerTex {
    GLuint tex    = 0;
    int    width  = 0;
    int    height = 0;
};

// Per-eye resources: FBO + colour (from swapchain) + depth renderbuffer
struct EyeFbo {
    GLuint fbo       = 0;
    GLuint depth_rbo = 0;
    GLuint color_tex = 0;  // owned by swapchain, not us
    int    width     = 0;
    int    height    = 0;
};

class GlesRenderer {
public:
    static constexpr int   k_max_copies      = 20;
    static constexpr float k_default_copy_step = 0.003f;

    GlesRenderer() = default;
    ~GlesRenderer() { shutdown(); }

    // Must be called once while the EGL context is current.
    bool init(std::string& error_out);
    void shutdown();

    // Upload / refresh a layer texture from a LayerFrame.
    // idx is the slot index (0-based); slots are auto-expanded.
    void update_layer(int idx, const LayerFrame& frame);

    // Ensure exactly n layer slots exist (creates/reuses GL textures).
    void resize_layers(int n);

    // Create an EyeFbo wrapping an existing swapchain colour texture.
    EyeFbo make_eye_fbo(GLuint color_tex, int width, int height);
    void   destroy_eye_fbo(EyeFbo& fbo);

    // Render all layers for one eye, then optional ROM browser panel + laser.
    // canvas_x/y: horizontal/vertical translation of the whole canvas (metres).
    // canvas_az/el: azimuth/elevation arc angles (radians); canvas swings on a sphere.
    void render_eye(const EyeFbo& fbo,
                    const Mat4& view,
                    const Mat4& proj,
                    const std::vector<LayerFrame*>& frames,
                    const VrState& state,
                    float canvas_x  = 0.0f,
                    float canvas_y  = 0.0f,
                    float canvas_az = 0.0f,
                    float canvas_el = 0.0f,
                    const OverlayInfo* overlay = nullptr,
                    float bg_r = 0.01f,
                    float bg_g = 0.01f,
                    float bg_b = 0.02f,
                    float bg_a = 1.0f);

    bool ok() const { return m_program != 0; }

private:
    // Main layer program (instanced depth copies + effects)
    GLuint m_program  = 0;
    GLuint m_vao      = 0;
    GLuint m_vbo      = 0;

    GLuint m_immersive_program = 0;
    GLuint m_curve_vao         = 0;
    GLuint m_curve_vbo         = 0;
    int    m_curve_vertex_count = 0;

    // Simple flat-colour program (ambilight shells, shadow quads)
    GLuint m_flat_prog = 0;
    GLuint m_flat_vao  = 0;
    GLuint m_flat_vbo  = 0;

    std::vector<LayerTex> m_layers;

    // Cached uniform locations
    GLint m_u_vp           = -1;
    GLint m_u_depth        = -1;
    GLint m_u_quad_w       = -1;
    GLint m_u_quad_h       = -1;
    GLint m_u_quad_y       = -1;
    GLint m_u_roundness    = -1;
    GLint m_u_copy_count   = -1;
    GLint m_u_copy_span    = -1;
    GLint m_u_screen_curve = -1;
    GLint m_u_upscale      = -1;
    GLint m_u_depthmap     = -1;
    GLint m_u_gamma        = -1;
    GLint m_u_contrast     = -1;
    GLint m_u_saturation   = -1;
    GLint m_u_brightness   = -1;
    GLint m_u_texture      = -1;

    GLint m_u_canvas_x   = -1;
    GLint m_u_canvas_y   = -1;
    GLint m_u_canvas_az  = -1;
    GLint m_u_canvas_el  = -1;
    GLint m_u_solid_stack = -1;

    GLint m_i_u_vp           = -1;
    GLint m_i_u_depth        = -1;
    GLint m_i_u_quad_w       = -1;
    GLint m_i_u_quad_h       = -1;
    GLint m_i_u_quad_y       = -1;
    GLint m_i_u_roundness    = -1;
    GLint m_i_u_copy_count   = -1;
    GLint m_i_u_copy_span    = -1;
    GLint m_i_u_screen_curve = -1;
    GLint m_i_u_tilt_x       = -1;
    GLint m_i_u_tilt_y       = -1;
    GLint m_i_u_upscale      = -1;
    GLint m_i_u_depthmap     = -1;
    GLint m_i_u_gamma        = -1;
    GLint m_i_u_contrast     = -1;
    GLint m_i_u_saturation   = -1;
    GLint m_i_u_brightness   = -1;
    GLint m_i_u_texture      = -1;
    GLint m_i_u_canvas_x     = -1;
    GLint m_i_u_canvas_y     = -1;
    GLint m_i_u_canvas_az    = -1;
    GLint m_i_u_canvas_el    = -1;
    GLint m_i_u_solid_stack  = -1;

    GLint m_flat_u_vp    = -1;
    GLint m_flat_u_color = -1;

    // UI program — world-space textured quad for ROM panel
    GLuint m_ui_prog = 0;
    GLuint m_ui_vao  = 0;
    GLuint m_ui_vbo  = 0;
    GLint  m_ui_u_vp      = -1;
    GLint  m_ui_u_model   = -1;
    GLint  m_ui_u_texture = -1;
    GLint  m_ui_u_alpha   = -1;

    bool init_layer_program(std::string& err);
    bool init_immersive_layer_program(std::string& err);
    bool init_flat_program(std::string& err);
    bool init_ui_program(std::string& err);
    void draw_ambilight(const std::vector<LayerFrame*>& frames,
                        const Mat4& vp, const VrState& state);
    void draw_shadow(const LayerFrame& frame, const Mat4& vp, const VrState& state);
    void draw_panel(const OverlayInfo& ov, const Mat4& vp);
    void draw_laser(const OverlayInfo& ov, const Mat4& vp);
    void draw_laser2(const OverlayInfo& ov, const Mat4& vp);
};
