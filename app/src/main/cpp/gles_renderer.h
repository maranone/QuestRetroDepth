#pragma once
#include <GLES3/gl3.h>
#include <GLES3/gl31.h>
#include <array>
#include <vector>
#include <string>
#include "layer_processor.h"
#include "psx_depth_frame.h"
#include "psx_gpu_frame.h"
#include "vr_state.h"
#include "mat4.h"
#include <openxr/openxr.h>

using qrd::PsxDepthFrame;
using qrd::PsxGpuFrame;

// One panel in world space (ROM browser, layer list, settings, …)
struct PanelInfo {
    GLuint     tex  = 0;              // 0 = skip
    XrPosef    pose = {{0,0,0,1},{0,0,0}};
    float      w    = 0.60f;          // metres
    float      h    = 0.50f;
    float      alpha = 1.0f;          // per-panel opacity — Interface > Transparency
    // Explicit paint priority, overriding the distance sort in draw_panel().
    // Panels are alpha-blended with depth writes off, so draw order alone
    // decides what covers what, and sorting by centre-to-eye distance gets
    // it wrong for a quad that sits in FRONT of a big panel but is offset
    // sideways from its centre: the lateral offset makes it measure as
    // farther, so it gets painted over by the panel it is standing out of.
    // Higher draws later (on top). Leave at 0 for ordinary panels, which
    // keeps the pure distance ordering they have always had.
    int        draw_order = 0;
};

// Highlight overlay on a panel (hovered row)
struct PanelHighlight {
    int    panel_idx = -1;       // which panel in OverlayInfo::panels
    float  u0 = 0, v0 = 0;       // UV bounds of highlight
    float  u1 = 0, v1 = 0;
    float  r = 0.18f, g = 0.39f, b = 0.75f; // highlight color (default blue)
    float  alpha = 0.35f;
};

// Temporary visual guides used by the live layer-canvas gesture. These are
// world-space quads placed on the actual layer planes; they are deliberately
// not ImGui panels.
struct LayerCanvasGuide {
    XrVector3f center = {0, 0, 0};
    XrVector3f right = {1, 0, 0};
    XrVector3f up = {0, 1, 0};
    XrVector3f normal = {0, 0, -1};
    float width = 0.0f;
    float height = 0.0f;
    float r = 0.55f;
    float g = 0.58f;
    float b = 0.62f;
    float alpha = 0.0f;
};

// All overlay elements drawn on top of the game layers
struct OverlayInfo {
    static constexpr int k_max_panels = 630;
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

    // Lightgun model, attached to the controller aiming it (see draw_gun_model()).
    bool    show_gun  = false;
    XrPosef gun_pose  = {{0,0,0,1},{0,0,0}};
    bool    gun_trigger = false;
    // Second lightgun (dual wield): player two's controller. Drawn with the same
    // model; only the pose differs, so draw_gun_model() takes a pose override
    // rather than this struct being copied -- it holds 630 panels.
    bool    show_gun2 = false;
    XrPosef gun2_pose = {{0,0,0,1},{0,0,0}};
    float   gun_recoil  = 0.0f; // one-shot recoil envelope, 0..1
    float   gun_tilt    = 0.0f; // local upward tilt in radians, used by revolver animation
    // 0=downloaded CC0 pistol, 1=restored low-poly pistol, 2=scope rifle.
    // See VrState::gun_model.
    int     gun_model = 0;
    int     gun2_model = 0; // player two's model, see VrState::gun2_model
    // Player two's own trigger/recoil, so its gun kicks on its own shots
    // rather than on player one's.
    bool    gun2_trigger = false;
    float   gun2_recoil = 0.0f;
    // Muzzle tint for the low-poly pistol, re-rolled on every shot (and once
    // per burst in machinegun mode). Defaults to the original chrome.
    float   gun_muzzle_color[3] = {0.55f, 0.56f, 0.58f};
    // Scope rifle muzzle heat, 0=cold steel .. 1=glowing. Rises and cools once
    // per shot (three times in machinegun mode).
    float   gun_muzzle_heat = 0.0f;

    // Real Quest controller models (XR_FB_render_model), for tutorial/
    // recording visibility -- see draw_controller_model() and
    // GlesRenderer::load_controller_model(). One entry per hand: index 0 =
    // left, 1 = right. Pose is the grip pose (anatomically correct anchor
    // for a controller model, unlike the aim pose used for the laser).
    bool       show_controller_models = false;
    XrPosef    controller_pose[2]   = {{{0,0,0,1},{0,0,0}}, {{0,0,0,1},{0,0,0}}};
    bool       controller_pose_valid[2] = {false, false};
    // Live per-button state, used to nudge named nodes in the loaded glTF
    // (see ControllerModel::animate() in gles_renderer.cpp). Values mirror
    // the real action state polled each frame in openxr_shell.cpp.
    bool       controller_btn_a[2] = {false, false};   // A/X
    bool       controller_btn_b[2] = {false, false};   // B/Y
    float      controller_trigger[2] = {0.0f, 0.0f};   // 0..1
    float      controller_grip[2]    = {0.0f, 0.0f};   // 0..1
    float      controller_stick_x[2] = {0.0f, 0.0f};   // -1..1
    float      controller_stick_y[2] = {0.0f, 0.0f};   // -1..1
    bool       controller_stick_click[2] = {false, false}; // left/right thumbstick click

    // Five-point lightgun calibration marker. The marker is placed on the same
    // curved/tilted surface used by the raycast, so the user is always aiming
    // at the geometry being calibrated.
    bool        show_calibration_target = false;
    XrVector3f calibration_target_center = {0,0,0};
    XrVector3f calibration_target_right  = {1,0,0};
    XrVector3f calibration_target_up     = {0,1,0};
    float       calibration_target_radius = 0.04f;

    std::vector<LayerCanvasGuide> live_layer_guides;
};

// One GPU texture per game layer
struct LayerTex {
    GLuint tex       = 0;
    GLuint depth_tex = 0;  // GL_R8 sprite Y-depth texture; 0 when unused
    // Silhouette edge profiles (see LayerFrame::edge_lr/edge_tb), used by real-geometry box
    // side faces when VrState::silhouette_sides is on. Nx1 GL_RG8 textures: 0 when unused.
    GLuint edge_lr_tex = 0; // height x 1, R=left_u, G=right_u per row
    GLuint edge_tb_tex = 0; // width x 1,  R=top_v,  G=bottom_v per column
    int    width  = 0;
    int    height = 0;
    std::uint64_t uploaded_revision = 0;
};

// Per-eye resources: FBO + colour (from swapchain) + depth renderbuffer
struct EyeFbo {
    GLuint fbo       = 0;
    GLuint depth_rbo = 0;
    GLuint color_tex = 0;  // owned by swapchain, not us
    int    width     = 0;
    int    height    = 0;
};

// Standalone offscreen FBO + owned color texture (unlike EyeFbo, which wraps an
// existing OpenXR swapchain texture it doesn't own). Used as the render target
// for ImGui-drawn panels; no depth buffer needed (2D UI, no depth test).
struct UiFbo {
    GLuint fbo       = 0;
    GLuint color_tex = 0;  // owned by us; this is what feeds PanelInfo::tex
    int    width     = 0;
    int    height    = 0;
};

struct SkyDomeInfo {
    bool enabled = false;
    EnvironmentSphereMode mode = EnvironmentSphereMode::Off;
    std::array<std::array<float, 4>, 12> bands{};
    // When true, renders fully opaque (shader mode 4) instead of the Full mode's fixed 50%
    // overlay alpha — for a user-chosen Background Color preset that should REPLACE the
    // background rather than blend subtly into it. `mode` is still FullSphere in this case;
    // this flag is what actually changes the shader's alpha behavior.
    bool opaque_override = false;
};

// One node of a parsed XR_FB_render_model glTF scene graph (see
// GlesRenderer::load_controller_model() in gles_renderer.cpp, which uses
// cgltf to fill this in). Kept GL/cgltf-agnostic here so this header doesn't
// need to pull in cgltf.h.
struct ControllerModelNode {
    std::string name;
    int   parent = -1;          // index into ControllerModel::nodes, -1 = root
    float translation[3] = {0, 0, 0};
    float rotation[4]    = {0, 0, 0, 1}; // quaternion, xyzw
    float scale[3]       = {1, 1, 1};
    // Baked scene exports (which is what this render model turned out to be)
    // commonly store a node's transform as a single 4x4 matrix instead of
    // separate TRS -- when has_matrix is set, use `matrix` (column-major,
    // same layout as Mat4) directly instead of translation/rotation/scale,
    // which stay at their identity defaults and would otherwise silently
    // drop this node's real transform.
    bool  has_matrix = false;
    float matrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    // Indices into ControllerModel::meshes -- one per glTF primitive on this
    // node's mesh (a node's mesh can have several primitives, e.g. separate
    // materials for the black top plate vs. the main body; rendering only
    // primitive[0] silently drops the rest). Empty = no geometry here.
    std::vector<int> mesh_indices;
    // True for a "*_world" reference node (e.g. this asset's
    // left/right_oculus_controller_world, parent of every interactive
    // button/trigger/thumbstick anchor) -- confirmed on-device to carry a
    // baked 180 deg rotation that its sibling body-mesh branch does not,
    // which mirrors every button anchor once the body is correctly posed.
    // draw_controller_model() drops such a node's own rotation (keeping its
    // translation) when accumulating world transforms, so its subtree shares
    // the body's frame instead of an extra, asset-specific flip.
    bool is_world_ref = false;
};

struct ControllerModelMesh {
    GLuint vao = 0, vbo = 0, ebo = 0;
    int    index_count = 0;
    float  base_color[4] = {0.62f, 0.62f, 0.66f, 1.0f};
    // Decoded from the primitive material's base-color texture when present
    // (see load_controller_model()) -- the black top plate, logo, and grip
    // pattern are painted detail in this texture, not separate geometry.
    GLuint tex = 0;
    bool   has_texture = false;
};

// One loaded/parsed controller (left or right). Interactive node indices are
// resolved once at load time by matching node names against the runtime's
// actual naming (logged via LOGI on first load so the real names can be
// confirmed on-device -- see load_controller_model()); each is -1 until a
// match is found, so animate() can no-op safely on an unrecognized name.
struct ControllerModel {
    bool loaded = false;
    std::vector<ControllerModelNode> nodes;
    std::vector<ControllerModelMesh> meshes;
    int node_trigger    = -1; // index (front) trigger anchor
    int node_grip       = -1; // squeeze/hand-grip anchor
    int node_thumbstick = -1;
    int node_button_a   = -1; // A (right) / X (left)
    int node_button_b   = -1; // B (right) / Y (left)
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

    // Real Quest controller models (XR_FB_render_model) -- see
    // OpenXrShell::load_controller_render_models(). hand: 0=left, 1=right.
    // Parses the glTF (GLB) buffer with cgltf and uploads GPU buffers;
    // returns false (and leaves that hand's model unloaded) on parse failure.
    bool load_controller_model(int hand, const std::vector<uint8_t>& glb);

    // GPU frame-time (ms), via GL_EXT_disjoint_timer_query when available. Wrap the whole
    // frame's rendering (both eyes) between begin/end — call begin before the eye loop, end
    // right after. Double-buffered internally so reading back a query result never stalls the
    // pipeline (we read last frame's result, not this frame's). get_last_gpu_ms() returns -1.0
    // when the extension isn't supported on this device, so callers can show "N/A" honestly
    // instead of a fabricated number.
    void  begin_gpu_timer();
    void  end_gpu_timer();
    float get_last_gpu_ms() const { return m_last_gpu_ms; }

    // Upload / refresh a layer texture from a LayerFrame.
    // idx is the slot index (0-based); slots are auto-expanded.
    void update_layer(int idx, const LayerFrame& frame);

    // Ensure exactly n layer slots exist (creates/reuses GL textures).
    void resize_layers(int n);

    // Create an EyeFbo wrapping an existing swapchain colour texture.
    EyeFbo make_eye_fbo(GLuint color_tex, int width, int height);
    void   destroy_eye_fbo(EyeFbo& fbo);

    // Standalone offscreen FBO (owned color texture, resizable in place) — render
    // target for the ImGui panel pass. Pass an existing UiFbo to resize it; pass a
    // default-constructed one to create fresh.
    UiFbo make_or_resize_ui_fbo(UiFbo fbo, int width, int height);
    void  destroy_ui_fbo(UiFbo& fbo);
    // Per-layer textures for the Library preview's world-space depth diorama:
    // one quad per emulated hardware layer, each pushed a little closer to the
    // headset, so a layered ROM previews with real depth instead of as the
    // flat composite the ImGui sidebar image can show (ImGui renders one flat
    // texture, so depth has to be world-space quads).
    static constexpr int k_max_library_preview_layers = 7;
    void update_library_preview_layer_texture(int layer, const std::vector<uint8_t>& rgba,
                                              int width, int height);
    void clear_library_preview_layers();
    GLuint library_preview_layer_texture(int layer) const {
        return (layer >= 0 && layer < k_max_library_preview_layers)
            ? m_library_preview_layer_tex[layer] : 0;
    }

    // Render all layers for one eye, then optional ROM browser panel + laser.
    // canvas_x/y: horizontal/vertical translation of the whole canvas (metres).
    // canvas_az/el: azimuth/elevation arc angles (radians); canvas swings on a sphere.
    void render_eye(const EyeFbo& fbo,
                    const Mat4& view,
                    const Mat4& proj,
                    const std::vector<LayerFrame*>& frames,
                    const PsxDepthFrame* psx_depth,
                    const VrState& state,
                    float canvas_x  = 0.0f,
                    float canvas_y  = 0.0f,
                    float canvas_az = 0.0f,
                    float canvas_el = 0.0f,
                    float canvas_scale = 1.0f,
                    const OverlayInfo* overlay = nullptr,
                    const SkyDomeInfo* sky_dome = nullptr,
                    float bg_r = 0.01f,
                    float bg_g = 0.01f,
                    float bg_b = 0.02f,
                    float bg_a = 1.0f,
                    bool passthrough_alpha = false,
                    float parallax_yaw   = 0.0f,
                    float parallax_pitch = 0.0f,
                    // HMD world position (app_space), for Billboard geometry mode's per-box
                    // facing. Defaults to origin when the caller has no tracked pose yet.
                    float hmd_x = 0.0f,
                    float hmd_y = 0.0f,
                    float hmd_z = 0.0f,
                    float fade_alpha = 0.0f,
                    // Layer-deck bookshelf: layer_deck_slots[i], when non-empty
                    // and matching frames.size(), gives frames[i]'s real stack
                    // slot (-1 = none); each layer then gets a per-slot yaw
                    // (see presentation::layer_deck_yaw) that turns it in place
                    // about its own vertical axis. Positions are NOT changed --
                    // the stack opens like the covers of a book on a shelf
                    // rather than swinging sideways. layer_deck_slot_count is
                    // the total slot count used to normalize the opening angle.
                    // Empty vector = bookshelf disabled.
                    const std::vector<int>& layer_deck_slots = {},
                    int layer_deck_slot_count = 0,
                    // Live spread multiplier (stick-adjustable, see
                    // OpenXrShell::m_layer_deck_spread), applied on top of
                    // layer_deck_yaw's base per-slot angle.
                    float layer_deck_spread = 1.0f);

    bool ok() const { return m_program != 0; }

private:
    // Full-view fade used during ROM handoff. Drawn after the world and panel
    // overlays so the transition covers the entire VR view.
    void draw_fade(const EyeFbo& fbo, float alpha);

    // GPU frame-time via GL_EXT_disjoint_timer_query (see begin_gpu_timer/end_gpu_timer).
    bool   m_gpu_timer_supported     = false;
    GLuint m_gpu_query[2]            = {0, 0};
    bool   m_gpu_query_has_result[2] = {false, false};
    int    m_gpu_query_write_idx     = 0;
    float  m_last_gpu_ms             = -1.0f;
    void (*m_glGetQueryObjectui64vEXT)(GLuint id, GLenum pname, uint64_t* params) = nullptr;

    // Main layer program (instanced depth copies + effects)
    GLuint m_program  = 0;
    GLuint m_vao      = 0;
    GLuint m_vbo      = 0;

    // Object-box rects (bbox extrusion) — SSBO, one draw call per layer regardless
    // of how many boxes are detected. Shared between m_program and m_immersive_program
    // (both bind it at layout(binding=0)); also shared with m_box_program below.
    GLuint m_object_box_ssbo = 0;
    GLuint m_object_depth_ssbo = 0;

    // Real-geometry box mesh: back face + 4 side faces (5 faces × 2 tris × 3 verts = 30
    // vertices) for one unit box, instanced once per detected object via m_object_box_ssbo.
    // The existing front-face quad draw (m_program/m_immersive_program) still draws the
    // visible sprite face on top; this mesh only supplies the sides/back behind it.
    GLuint m_box_program = 0;
    GLuint m_box_vao     = 0;
    GLuint m_box_vbo     = 0;
    int    m_box_vertex_count = 0;
    GLint  m_box_u_vp             = -1;
    GLint  m_box_u_depth          = -1;
    GLint  m_box_u_zbuffer_depths = -1;
    GLint  m_box_u_quad_w         = -1;
    GLint  m_box_u_quad_h         = -1;
    GLint  m_box_u_quad_y         = -1;
    GLint  m_box_u_table_mode     = -1; // Table Mode: flat tabletop basis, extrusion runs up
    GLint  m_box_u_thickness      = -1;
    GLint  m_box_u_screen_curve   = -1;
    GLint  m_box_u_orientation    = -1;
    GLint  m_box_u_allow_behind   = -1;
    GLint  m_box_u_ref_l1_depth   = -1;
    GLint  m_box_u_auto_thickness = -1;
    GLint  m_box_u_scatter_range  = -1; // DepthScatter: per-object depth jitter magnitude, metres
    GLint  m_box_u_y_depth_range  = -1; // AutoYDepth: per-object depth-from-row range, metres
    GLint  m_box_u_hmd_pos        = -1; // Billboard: HMD world position, for per-box facing
    GLint  m_box_u_size_thickness_mode = -1; // SizeThickness: 1.0 = scale extrusion by box area
    GLint  m_box_u_silhouette     = -1;
    GLint  m_box_u_edge_lr        = -1;
    GLint  m_box_u_edge_tb        = -1;
    GLint  m_box_u_side_color_mode = -1; // 0=Ori 1=Black 2=White 3=Red 4=Green 5=Blue 6=Darker (back/side faces only)
    GLint  m_box_u_side_color_rgb  = -1;
    GLint  m_box_u_side_color_darken = -1; // mode 6 only: dim the real sampled texture instead of a flat color
    GLint  m_box_u_tilt_x         = -1;
    GLint  m_box_u_tilt_y         = -1;
    GLint  m_box_u_canvas_x       = -1;
    GLint  m_box_u_canvas_y       = -1;
    GLint  m_box_u_canvas_az      = -1;
    GLint  m_box_u_layer_yaw      = -1;
    GLint  m_box_u_canvas_el      = -1;
    GLint  m_box_u_canvas_scale   = -1;
    GLint  m_box_u_gamma          = -1;
    GLint  m_box_u_contrast       = -1;
    GLint  m_box_u_saturation     = -1;
    GLint  m_box_u_brightness     = -1;
    GLint  m_box_u_pixel_light    = -1;
    GLint  m_box_u_light_dir      = -1;
    GLint  m_box_u_light_ambient  = -1;
    GLint  m_box_u_light_flicker  = -1;
    GLint  m_box_u_fog_factor     = -1;
    GLint  m_box_u_fog_color      = -1;
    GLint  m_box_u_texture        = -1;
    GLint  m_box_u_force_opaque_alpha = -1;

    GLuint m_immersive_program = 0;
    GLuint m_curve_vao         = 0;
    GLuint m_curve_vbo         = 0;
    int    m_curve_vertex_count = 0;

    // Simple flat-colour program (ambilight shells, shadow quads)
    GLuint m_flat_prog = 0;
    GLuint m_flat_vao  = 0;
    GLuint m_flat_vbo  = 0;

    // Lit flat-colour program for the lightgun model (pos+normal, single
    // directional light) — kept separate from m_flat_prog/m_flat_vao so the
    // unlit calibration-ring/ambilight paths are untouched.
    GLuint m_gun_prog = 0;
    GLuint m_gun_vao  = 0;
    GLuint m_gun_vbo  = 0;
    // Baked low-poly replacement for the old procedural pistol (position,
    // normal, and palette color per vertex).
    GLuint m_pistol_vao = 0;
    GLuint m_pistol_vbo = 0;
    int    m_pistol_vertex_count = 0;

    // Real controller models loaded at runtime from XR_FB_render_model (see
    // OpenXrShell::load_controller_render_models()) instead of a baked mesh
    // -- see ControllerModel/load_controller_model()/draw_controller_model()
    // in gles_renderer.cpp. Index 0 = left, 1 = right.
    ControllerModel m_controller_model[2];
    GLuint m_controller_prog = 0;
    GLint  m_controller_u_model = -1;
    GLint  m_controller_u_vp    = -1;
    GLint  m_controller_u_color = -1;
    GLint  m_controller_u_light_dir     = -1;
    GLint  m_controller_u_light_ambient = -1;
    GLint  m_controller_u_tex         = -1;
    GLint  m_controller_u_has_texture = -1;
    // Tiny baked bitmap-font atlas + dynamic quad buffer for the "LEFT+GRIP"
    // style press-feedback label (replaces the octahedron marker) -- see
    // init_controller_label_font()/controller_draw_label() in
    // gles_renderer.cpp. Reuses m_ui_prog (the generic textured-quad shader
    // already used for panels) instead of a new shader.
    GLuint m_controller_label_tex = 0;
    GLuint m_controller_label_vao = 0;
    GLuint m_controller_label_vbo = 0;

    // Scope-rifle "look through the mirilla" 2x zoom: copies the just-rendered
    // eye image into m_scope_copy_tex, then composites a zoomed circular
    // crop back over it wherever the scope's front lens projects on screen.
    GLuint m_scope_zoom_prog = 0;
    GLuint m_scope_vao       = 0;   // empty VAO, attributeless fullscreen triangle
    GLuint m_scope_copy_fbo  = 0;
    GLuint m_scope_copy_tex  = 0;
    int    m_scope_copy_w    = 0;
    int    m_scope_copy_h    = 0;
    GLint  m_scope_u_tex     = -1;
    GLint  m_scope_u_center  = -1;
    GLint  m_scope_u_radius  = -1;
    GLint  m_scope_u_zoom_inv = -1;
    GLint  m_scope_u_aspect  = -1;

    GLuint m_sky_prog  = 0;
    GLuint m_sky_vao   = 0;
    GLuint m_sky_vbo   = 0;
    int    m_sky_vertex_count = 0;

    std::vector<LayerTex> m_layers;

    // Tessellated depth mesh cache (regenerated when layer dimensions change)
    GLuint m_dm_vao         = 0;
    GLuint m_dm_vbo         = 0;
    GLuint m_dm_ebo         = 0;
    int    m_dm_W           = 0;
    int    m_dm_H           = 0;
    int    m_dm_index_count = 0;

    // Sprite Y-depth uniform locations (m_program / flat mode only)
    GLint  m_u_has_y_depth    = -1;
    GLint  m_u_y_depth_tex    = -1;
    GLint  m_u_y_depth_spread = -1;

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
    GLint m_u_pixel_light  = -1;
    GLint m_u_light_dir    = -1;
    GLint m_u_light_ambient = -1;
    GLint m_u_light_flicker = -1;
    GLint m_u_fog_factor    = -1;
    GLint m_u_fog_color     = -1;
    GLint m_u_texture      = -1;

    GLint m_u_canvas_x   = -1;
    GLint m_u_canvas_y   = -1;
    GLint m_u_canvas_az  = -1;
    GLint m_u_layer_yaw  = -1;
    GLint m_u_canvas_el  = -1;
    GLint m_u_canvas_scale = -1;
    GLint m_u_solid_stack = -1;
    GLint m_u_force_opaque_alpha = -1;
    GLint m_u_bbox_mode   = -1;
    GLint m_u_zbuffer_depths = -1;
    GLint m_u_bbox_debug  = -1;
    GLint m_u_subrect_enable = -1;
    GLint m_u_subrect = -1;
    GLint m_u_instance_base = -1;
    GLint m_u_object_box_count = -1;
    GLint m_u_allow_behind = -1;
    GLint m_u_edge_lr = -1;
    GLint m_u_edge_tb = -1;
    GLint m_u_has_edge_profile = -1;
    GLint m_u_rotate90 = -1;
    GLint m_u_table_mode = -1;

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
    GLint m_i_u_pixel_light  = -1;
    GLint m_i_u_light_dir    = -1;
    GLint m_i_u_light_ambient = -1;
    GLint m_i_u_light_flicker = -1;
    GLint m_i_u_fog_factor    = -1;
    GLint m_i_u_fog_color     = -1;
    GLint m_i_u_texture      = -1;
    GLint m_i_u_canvas_x     = -1;
    GLint m_i_u_canvas_y     = -1;
    GLint m_i_u_canvas_az    = -1;
    GLint m_i_u_layer_yaw    = -1;
    GLint m_i_u_canvas_el    = -1;
    GLint m_i_u_canvas_scale = -1;
    GLint m_i_u_solid_stack  = -1;
    GLint m_i_u_force_opaque_alpha = -1;
    GLint m_i_u_bbox_mode   = -1;
    GLint m_i_u_zbuffer_depths = -1;
    GLint m_i_u_bbox_debug  = -1;
    GLint m_i_u_subrect_enable = -1;
    GLint m_i_u_subrect = -1;
    GLint m_i_u_instance_base = -1;
    GLint m_i_u_object_box_count = -1;
    GLint m_i_u_allow_behind = -1;
    GLint m_i_u_edge_lr = -1;
    GLint m_i_u_edge_tb = -1;
    GLint m_i_u_has_edge_profile = -1;
    GLint m_i_u_has_y_depth = -1;
    GLint m_i_u_y_depth_tex = -1;
    GLint m_i_u_y_depth_spread = -1;
    GLint m_i_u_rotate90 = -1;
    GLint m_i_u_table_mode = -1;

    GLint m_flat_u_vp    = -1;
    GLint m_flat_u_color = -1;

    GLint m_gun_u_vp            = -1;
    GLint m_gun_u_model         = -1;
    GLint m_gun_u_color         = -1;
    GLint m_gun_u_vertex_color  = -1;
    GLint m_gun_u_trigger       = -1;
    GLint m_gun_u_recoil        = -1;
    GLint m_gun_u_tilt          = -1;
    GLint m_gun_u_light_dir     = -1;
    GLint m_gun_u_light_ambient = -1;

    GLint m_sky_u_proj   = -1;
    GLint m_sky_u_view   = -1;
    GLint m_sky_u_bands  = -1;
    GLint m_sky_u_mode   = -1;

    // UI program — world-space textured quad for ROM panel
    GLuint m_ui_prog = 0;
    GLuint m_ui_vao  = 0;
    GLuint m_ui_vbo  = 0;
    GLuint m_library_preview_layer_tex[k_max_library_preview_layers]{};
    GLint  m_ui_u_vp      = -1;
    GLint  m_ui_u_model   = -1;
    GLint  m_ui_u_texture = -1;
    GLint  m_ui_u_alpha   = -1;
    GLint  m_ui_u_shadow_mode = -1;
    GLint  m_ui_u_shadow_color = -1;

    bool init_layer_program(std::string& err);
    bool init_immersive_layer_program(std::string& err);
    bool init_box_program(std::string& err);
    bool init_flat_program(std::string& err);
    bool init_gun_program(std::string& err);
    bool init_controller_program(std::string& err);
    void init_controller_label_font();
    bool init_scope_zoom_program(std::string& err);
    void draw_scope_zoom(const OverlayInfo& ov, const Mat4& vp, const EyeFbo& fbo,
                          float hmd_x, float hmd_y, float hmd_z);
    bool init_sky_program(std::string& err);
    bool init_ui_program(std::string& err);
    void ensure_depth_mesh(int W, int H);
    void draw_sky_dome(const Mat4& view, const Mat4& proj, const SkyDomeInfo& info);
    void draw_ambilight(const std::vector<LayerFrame*>& frames,
                        const Mat4& vp, const VrState& state);
    void draw_shadow(int layer_index, const LayerFrame& frame, const Mat4& vp, const VrState& state);
    void draw_panel(const OverlayInfo& ov, const Mat4& vp, float eye_x = 0.0f, float eye_y = 0.0f, float eye_z = 0.0f);
    void draw_laser(const OverlayInfo& ov, const Mat4& vp);
    void draw_live_layer_guides(const OverlayInfo& ov, const Mat4& vp);
    void draw_laser2(const OverlayInfo& ov, const Mat4& vp);
    void draw_gun_model(const OverlayInfo& ov, const Mat4& vp, const XrPosef* pose_override = nullptr);
    void draw_calibration_target(const OverlayInfo& ov, const Mat4& vp);
    void draw_controller_model(const OverlayInfo& ov, const Mat4& vp);
    bool init_psx_screen_program(std::string& err);
    // Draws the PSX screen as a dense mesh displaced by the backend's per-pixel
    // depth map. Returns false if the program is unavailable, in which case the
    // caller falls back to the normal layered path.
    bool draw_psx_screen(const LayerFrame& frame, const PsxDepthFrame* depth,
                         const PsxGpuFrame* gpu,
                         const Mat4& view, const Mat4& proj,
                         float canvas_x, float canvas_y, float canvas_az,
                         float canvas_el, float canvas_scale);

    // Frame currently held from the emulator's share group. Kept across both
    // eyes of an XR frame so the fence is waited on once, not twice.
    PsxGpuFrame m_psx_gpu_frame;

    // Screen mesh density. Built once in normalised screen space, so it is
    // independent of the game's output resolution.
    static constexpr int kPsxScreenGridX = 512;
    static constexpr int kPsxScreenGridY = 448;
    // Total depth the scene spans, split around the screen plane by
    // kPsxDepthPivotDefault — at the default pivot of 0.5 that is half this
    // distance toward the viewer and half away. Tuned on a Quest 2 against
    // Time Crisis. Overridable at runtime with
    //   adb shell setprop debug.qrd.psxdepth 0.5
    // for tuning by eye; unset or 0 uses this default.
    static constexpr float kPsxScreenDepthMetres = 0.2f;
    // Shading strength for faces that bridge a depth discontinuity, where the
    // texture is stretched across a silhouette and shows colour from the wrong
    // side of the edge. A depth step of ~0.15 across one mesh cell reaches full
    // darkening. Tunable with debug.qrd.psxedge; 0 disables.
    static constexpr float kPsxScreenEdgeDarken = 32.0f;
    // How the bridging faces are treated: 0 leaves the raw smear, 1 shades
    // them, 2 discards them so the silhouette is a clean edge (and the scene
    // behind the screen shows through the gap). Tunable with
    // debug.qrd.psxedgemode.
    static constexpr int kPsxScreenEdgeMode = 1;

    GLuint m_psx_screen_program = 0;
    GLuint m_psx_screen_vao = 0;
    GLuint m_psx_screen_vbo = 0;
    GLuint m_psx_screen_ibo = 0;
    GLuint m_psx_screen_color = 0;
    GLuint m_psx_screen_depth_tex = 0;
    GLsizei m_psx_screen_index_count = 0;
    int m_psx_screen_color_w = 0;
    int m_psx_screen_color_h = 0;
    int m_psx_screen_depth_w = 0;
    int m_psx_screen_depth_h = 0;
    GLint m_psx_screen_u_vp = -1;
    GLint m_psx_screen_u_texture = -1;
    GLint m_psx_screen_u_depth_tex = -1;
    GLint m_psx_screen_u_depth = -1;
    GLint m_psx_screen_u_quad_w = -1;
    GLint m_psx_screen_u_quad_h = -1;
    GLint m_psx_screen_u_canvas_x = -1;
    GLint m_psx_screen_u_canvas_y = -1;
    GLint m_psx_screen_u_canvas_az = -1;
    GLint m_psx_screen_u_canvas_el = -1;
    GLint m_psx_screen_u_canvas_scale = -1;
    GLint m_psx_screen_u_depth_scale = -1;
    GLint m_psx_screen_u_has_depth = -1;
    GLint m_psx_screen_u_flip_v = -1;
    GLint m_psx_screen_u_color_uv_scale = -1;
    GLint m_psx_screen_u_color_uv_offset = -1;
    GLint m_psx_screen_u_grid_step = -1;
    GLint m_psx_screen_u_edge_darken = -1;
    GLint m_psx_screen_u_edge_mode = -1;
    GLint m_psx_screen_u_depth_pivot = -1;
    // Live mesh density, defaults to kPsxScreenGridX/Y and rebuildable via
    // debug.qrd.psxgrid.
    int m_psx_screen_grid_x = kPsxScreenGridX;
    int m_psx_screen_grid_y = kPsxScreenGridY;
    void build_psx_screen_mesh(int grid_x, int grid_y);
};
