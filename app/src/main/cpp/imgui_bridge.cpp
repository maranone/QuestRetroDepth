#include "imgui_bridge.h"

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#include <chrono>

// No platform/windowing backend (imgui_impl_android etc.) is used — there is
// no live OS window to read input from. Input is injected manually each frame
// from the laser-pointer raycast (see begin_frame()); only the GLES3 render
// backend (imgui_impl_opengl3) is used, the same one a desktop OpenGL3 app
// would use, since the project already links GLESv3 directly.

// One ImGuiStyle color set per real UiThemeId (ui_theme.h) — PremiumRetroTech
// keeps the original dark cyan/magenta neon look from the infra test; the other
// three are new, approximating what "Classic"/"Glass"/"Arcade" suggest, since
// the old Kotlin panels never had an ImGui equivalent to copy exact values from.
static void apply_style_colors(const ImVec4& bg, const ImVec4& bg_dim, const ImVec4& accent,
                                const ImVec4& accent_dim, const ImVec4& accent2, const ImVec4& text) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]         = bg;
    c[ImGuiCol_ChildBg]          = bg;
    c[ImGuiCol_PopupBg]          = bg;
    c[ImGuiCol_Border]           = accent;
    c[ImGuiCol_TitleBg]          = bg_dim;
    c[ImGuiCol_TitleBgActive]    = bg_dim;
    c[ImGuiCol_Text]             = text;
    c[ImGuiCol_Button]           = bg_dim;
    c[ImGuiCol_ButtonHovered]    = accent_dim;
    c[ImGuiCol_ButtonActive]     = accent2;
    c[ImGuiCol_FrameBg]          = bg_dim;
    c[ImGuiCol_FrameBgHovered]   = accent_dim;
    c[ImGuiCol_FrameBgActive]    = accent_dim;
    c[ImGuiCol_CheckMark]        = accent2;
    c[ImGuiCol_SliderGrab]       = accent_dim;
    c[ImGuiCol_SliderGrabActive] = accent2;
    // Header (a Selectable's "selected" background) and HeaderHovered (the
    // hover highlight) were both `accent_dim` — an already-selected option and
    // one you're merely pointing at read as the same color, which made it
    // impossible to tell what the laser was actually over vs. what was already
    // chosen. Three distinct brightness steps now: selected < hovered < pressed.
    c[ImGuiCol_Header]           = accent_dim;
    c[ImGuiCol_HeaderHovered]    = accent;
    c[ImGuiCol_HeaderActive]     = accent2;
    c[ImGuiCol_Separator]        = accent_dim;
    c[ImGuiCol_ScrollbarBg]      = bg_dim;
    c[ImGuiCol_ScrollbarGrab]    = accent_dim;
    c[ImGuiCol_ScrollbarGrabHovered] = accent;
    c[ImGuiCol_ScrollbarGrabActive]  = accent2;
}

void ImGuiBridge::apply_theme(int theme_id) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8.0f; s.FrameRounding = 6.0f; s.GrabRounding = 6.0f;
    s.PopupRounding  = 6.0f; s.ScrollbarRounding = 8.0f;
    // Border lines aliased into an alternating "bar, no bar" pattern when this
    // panel is displayed minified in world space at VR viewing distance — the
    // strokes were thin enough (1.0-1.5px at the FBO's native resolution) that
    // downscaling made every other pixel row of the stroke disappear. Tried
    // fixing this with mipmapping the panel texture instead (GL_LINEAR_MIPMAP_LINEAR
    // + per-frame glGenerateMipmap in gles_renderer.cpp/imgui_bridge.cpp), but that
    // visibly blurred the whole panel, not just the borders — reverted. Widening
    // the strokes themselves survives minification without touching overall
    // texture sharpness.
    s.WindowBorderSize = 3.0f; s.FrameBorderSize = 2.0f;
    s.AntiAliasedLines = false; // crisp thick strokes, not thin AA'd ones that alias
    s.WindowPadding = ImVec2(14, 14); s.FramePadding = ImVec2(10, 8); s.ItemSpacing = ImVec2(10, 10);

    switch (theme_id) {
        case 0: // Classic — neutral dark grey, muted blue accent
            apply_style_colors(ImVec4(0.10f, 0.10f, 0.11f, 0.97f), ImVec4(0.07f, 0.07f, 0.08f, 0.97f),
                                ImVec4(0.45f, 0.55f, 0.70f, 1.00f), ImVec4(0.25f, 0.32f, 0.45f, 1.00f),
                                ImVec4(0.55f, 0.65f, 0.85f, 1.00f), ImVec4(0.90f, 0.90f, 0.92f, 1.00f));
            break;
        case 2: // Glass — light, translucent, cool cyan accent
            apply_style_colors(ImVec4(0.85f, 0.88f, 0.92f, 0.75f), ImVec4(0.78f, 0.82f, 0.88f, 0.75f),
                                ImVec4(0.10f, 0.55f, 0.65f, 1.00f), ImVec4(0.55f, 0.75f, 0.82f, 1.00f),
                                ImVec4(0.05f, 0.40f, 0.50f, 1.00f), ImVec4(0.08f, 0.10f, 0.14f, 1.00f));
            break;
        case 3: // Arcade — bold black + saturated red/yellow
            apply_style_colors(ImVec4(0.06f, 0.02f, 0.02f, 0.97f), ImVec4(0.03f, 0.01f, 0.01f, 0.97f),
                                ImVec4(1.00f, 0.20f, 0.10f, 1.00f), ImVec4(0.55f, 0.10f, 0.05f, 1.00f),
                                ImVec4(1.00f, 0.80f, 0.00f, 1.00f), ImVec4(1.00f, 0.95f, 0.85f, 1.00f));
            break;
        // Four new themes, each picked to sit in a hue/mood no existing theme
        // touches: Classic is blue-grey, PremiumRetroTech is cyan/magenta neon,
        // Glass is a light cool cyan, Arcade is black/red/yellow — none of them
        // are green, orange/plum, achromatic, or indigo/gold, so that's what
        // these four are.
        case 4: // Forest — deep green, warm amber accent
            apply_style_colors(ImVec4(0.05f, 0.10f, 0.06f, 0.97f), ImVec4(0.03f, 0.06f, 0.04f, 0.97f),
                                ImVec4(0.85f, 0.60f, 0.20f, 1.00f), ImVec4(0.45f, 0.32f, 0.12f, 1.00f),
                                ImVec4(0.55f, 0.90f, 0.45f, 1.00f), ImVec4(0.90f, 0.95f, 0.88f, 1.00f));
            break;
        case 5: // Sunset — dark plum/maroon, coral + gold accents
            apply_style_colors(ImVec4(0.14f, 0.05f, 0.08f, 0.97f), ImVec4(0.09f, 0.03f, 0.05f, 0.97f),
                                ImVec4(0.95f, 0.45f, 0.35f, 1.00f), ImVec4(0.50f, 0.22f, 0.20f, 1.00f),
                                ImVec4(1.00f, 0.75f, 0.30f, 1.00f), ImVec4(1.00f, 0.92f, 0.88f, 1.00f));
            break;
        case 6: // Mono — true achromatic grayscale, no color anywhere
            apply_style_colors(ImVec4(0.08f, 0.08f, 0.08f, 0.97f), ImVec4(0.04f, 0.04f, 0.04f, 0.97f),
                                ImVec4(0.80f, 0.80f, 0.80f, 1.00f), ImVec4(0.40f, 0.40f, 0.40f, 1.00f),
                                ImVec4(1.00f, 1.00f, 1.00f, 1.00f), ImVec4(0.92f, 0.92f, 0.92f, 1.00f));
            break;
        case 7: // Royal — deep indigo, gold accent, lavender highlight
            apply_style_colors(ImVec4(0.08f, 0.06f, 0.16f, 0.97f), ImVec4(0.05f, 0.04f, 0.11f, 0.97f),
                                ImVec4(0.85f, 0.70f, 0.25f, 1.00f), ImVec4(0.35f, 0.28f, 0.14f, 1.00f),
                                ImVec4(0.75f, 0.65f, 0.95f, 1.00f), ImVec4(0.93f, 0.90f, 0.98f, 1.00f));
            break;
        case 1: // PremiumRetroTech (default) — original dark cyan/magenta neon
        default:
            apply_style_colors(ImVec4(0.05f, 0.05f, 0.09f, 0.95f), ImVec4(0.03f, 0.03f, 0.06f, 0.95f),
                                ImVec4(0.00f, 0.95f, 1.00f, 1.00f), ImVec4(0.00f, 0.55f, 0.60f, 1.00f),
                                ImVec4(1.00f, 0.10f, 0.85f, 1.00f), ImVec4(0.85f, 1.00f, 1.00f, 1.00f));
            break;
    }
}

bool ImGuiBridge::init(std::string& error_out) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // no on-disk imgui.ini — panel state lives in VrState/settings_io
    io.BackendPlatformName = "questretrodepth_vr_laser";
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // ImGui's default font atlas rasterizes at 13px, tuned for a desktop monitor —
    // at typical VR viewing distance that reads as blurry/mushy rather than crisp,
    // no matter how high the panel's own render-target resolution is (stretching a
    // low-res glyph atlas just blurs it further). Rebuild the atlas at a much larger
    // base size instead, before the GL3 backend creates its font texture.
    ImFontConfig font_cfg;
    font_cfg.SizePixels = 32.0f;
    io.Fonts->AddFontDefault(&font_cfg);

    apply_theme(1); // PremiumRetroTech — matches UiThemeId's own default; menu re-applies the real saved theme once it starts drawing

    if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
        error_out = "ImGui_ImplOpenGL3_Init failed";
        ImGui::DestroyContext();
        return false;
    }

    m_initialized = true;
    return true;
}

void ImGuiBridge::shutdown() {
    if (!m_initialized) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
}

void ImGuiBridge::begin_frame(int fbo_w, int fbo_h, float mouse_x, float mouse_y, bool mouse_down,
                               float mouse_wheel) {
    if (!m_initialized || fbo_w <= 0 || fbo_h <= 0) return;

    static auto s_last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - s_last).count();
    s_last = now;
    if (dt <= 0.0f || dt > 1.0f) dt = 1.0f / 72.0f; // sane fallback for first frame / hitches

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)fbo_w, (float)fbo_h);
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = dt;
    io.AddMousePosEvent(mouse_x, mouse_y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouse_down);
    if (mouse_wheel != 0.0f) io.AddMouseWheelEvent(0.0f, mouse_wheel);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    m_frame_open = true;
}

GLuint ImGuiBridge::end_frame(GlesRenderer& renderer, int fbo_w, int fbo_h, int slot) {
    if (!m_initialized || !m_frame_open) return 0;
    m_frame_open = false;
    if (slot < 0 || slot >= k_max_slots) slot = 0;

    ImGui::Render();

    // Scratch (unflipped) FBO: shared across slots since it's fully consumed
    // (rendered into, then blitted out below) within this single call — never
    // read back once end_frame() returns, so nothing from a different slot's
    // call can still be relying on its contents.
    m_fbo = renderer.make_or_resize_ui_fbo(m_fbo, fbo_w, fbo_h);
    if (!m_fbo.fbo) return 0;

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    GLint prev_viewport[4];
    glGetIntegerv(GL_VIEWPORT, prev_viewport);

    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo.fbo);
    glViewport(0, 0, fbo_w, fbo_h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // transparent — panel quad alpha-blends over the world
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // ImGui (like any normal GL render) produces a texture in GL's native
    // bottom-left-origin convention. The shared panel quad shader expects the
    // Kotlin-bitmap convention instead (V=0 = top of the image, since Android
    // Bitmap rows are uploaded top-first with no reversal) — so flip vertically
    // via a blit into a PER-SLOT FBO before handing the texture to the quad,
    // so this slot's output persists independently of whatever other slot's
    // end_frame() call runs next (see the k_max_slots comment in the header).
    m_fbo_flipped[slot] = renderer.make_or_resize_ui_fbo(m_fbo_flipped[slot], fbo_w, fbo_h);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo_flipped[slot].fbo);
    glBlitFramebuffer(0, 0, fbo_w, fbo_h, 0, fbo_h, fbo_w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);

    return m_fbo_flipped[slot].color_tex;
}
