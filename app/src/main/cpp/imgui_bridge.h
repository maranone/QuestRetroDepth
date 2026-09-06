#pragma once
#include <GLES3/gl3.h>
#include <string>
#include "gles_renderer.h"

// Owns the single Dear ImGui context used for every VR panel and the
// offscreen FBO it renders into. One ImGuiBridge per app; panels are built as
// ordinary ImGui windows each frame (see docs/claude_xr.md for the migration
// this is part of).
//
// Usage per XR frame, once (not per-eye):
//   bridge.begin_frame(fbo_w, fbo_h, mouse_x_px, mouse_y_px, mouse_down);
//   ... ImGui::Begin("Settings"); ImGui::Checkbox(...); ImGui::End(); ...
//   GLuint tex = bridge.end_frame(renderer);
// `tex` is then dropped straight into an OverlayInfo::panels[] PanelInfo,
// same as any Kotlin-bitmap-sourced panel texture.
class ImGuiBridge {
public:
    ImGuiBridge() = default;
    ~ImGuiBridge() { shutdown(); }

    // Must be called once while the EGL context is current, after GlesRenderer::init().
    bool init(std::string& error_out);
    void shutdown();

    // Re-skins every ImGui color to match one of the app's real UiThemeId values
    // (ui_theme.h — Classic/PremiumRetroTech/Glass/Arcade, the same 4 themes the
    // old Kotlin panels use), so the new ImGui menu and the old panel system stay
    // visually consistent under one theme choice instead of a second, disconnected
    // ImGui-only palette. Safe to call every frame; only touches ImGuiStyle colors.
    void apply_theme(int theme_id);

    // Starts a new ImGui frame sized to (fbo_w, fbo_h) pixels. mouse_x/y are in
    // that same pixel space (convert from the laser-hit panel UV before calling:
    // x = u * fbo_w, y = v * fbo_h). mouse_down maps straight from the existing
    // right-trigger edge-detect state. mouse_wheel drives ImGui's normal
    // scroll-wheel handling (one "notch" per unit, positive = scroll up/content
    // moves down) — feed it from the active hand's thumbstick Y each frame so
    // long option lists can be scrolled without a laser drag gesture.
    void begin_frame(int fbo_w, int fbo_h, float mouse_x, float mouse_y, bool mouse_down,
                      float mouse_wheel = 0.0f);

    // Ends the frame, renders all queued ImGui draw commands into the offscreen
    // FBO for `slot` (resizing it if fbo_w/fbo_h changed since last call), and
    // returns that slot's resulting color texture. Returns 0 if init() wasn't
    // called or wasn't successful.
    //
    // `slot` selects which of a small fixed set of independent output FBOs to
    // render into (see k_max_slots) — every slot shares the ONE ImGui context
    // (there is only ever one real ImGui "frame" open at a time, sequenced by
    // begin_frame/end_frame pairs), but each gets its OWN persistent texture.
    // This matters when more than one world-space panel needs to show DIFFERENT
    // ImGui content simultaneously (e.g. the two automatic gameplay side
    // panels): with a single shared output texture, the second panel's
    // end_frame() call would silently overwrite the first's texture in place,
    // so both quads would end up displaying whichever slot was drawn LAST that
    // frame — not two independent panels. Per-slot FBOs fix that. It also lets
    // a slot that wasn't re-driven this frame (see the "only drive the
    // actively-interacted panel" rule in render_frame()) keep showing its own
    // last real content as a static placeholder, rather than a torn/foreign one.
    GLuint end_frame(GlesRenderer& renderer, int fbo_w, int fbo_h, int slot = 0);

    static constexpr int k_max_slots = 3;

    bool is_initialized() const { return m_initialized; }

private:
    bool  m_initialized = false;
    bool  m_frame_open   = false;
    UiFbo m_fbo;                          // ImGui renders here (GL's native bottom-left-origin convention)
    UiFbo m_fbo_flipped[k_max_slots];     // per-slot vertically-flipped copies — match the panel
                                          // quad's texture convention (V=0 = top), same as
                                          // Kotlin-bitmap panel textures.
};
