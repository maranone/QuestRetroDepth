#pragma once

#include "emulator_backend.h"
#include "psx_gpu_frame.h"
#include "psx_hw_depth_bridge.h"
#include "platform/libretro/libretro.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qrd {

// Directory reported to SwanStation for BIOS files and memory cards.
void set_psx_system_directory(const std::string& dir);

// SwanStation owns emulation and produces the one true colour frame. QRD adds
// a per-pixel depth map built from the PGXP command capture, which the XR
// renderer uses to displace the screen mesh into stereo. There is no flat mode
// and no layered mode: PSX is always the depth-displaced screen.
class PsxLibretroBackend final : public EmulatorBackend {
public:
    PsxLibretroBackend();
    ~PsxLibretroBackend() override;

    const char* backend_name() const override;
    double frame_rate_hz() const override;
    bool load_content(const std::string& rom_path, std::string& error_out) override;
    bool step_frame(const EmulatorInputState& input, std::string& error_out) override;
    const FrameOutput& frame_output() const override;
    bool save_state(std::vector<uint8_t>& out, std::string& error_out) override;
    bool load_state(const void* data, std::size_t size, std::string& error_out) override;
    void set_auto_frame_skip(bool enabled) override;
    void set_gpu_resolution(int scale);
    void set_layer_capture_mask(uint32_t mask) override;
    void set_psx_render_path(int path) override;
    // SwanStation's texture filter (its internal texture upscaler), by
    // VrState::kPsxTextureFilterValues index. Hardware renderer only; the core
    // recompiles its shaders when it picks the change up between frames.
    void set_texture_filter(int index);
    void set_preview_mode(bool enabled, bool allow_audio = false) override {
        m_preview_mode = enabled;
        m_preview_allow_audio = allow_audio;
    }
    void set_gun_mode(bool enabled, int peripheral = 0) override;
    // Plugs a second GunCon into port 1 so two-player gun titles see a real
    // second player, each controller aiming its own gun.
    void set_dual_gun_mode(bool enabled) override;
    void soft_reset() override;
    RomHeaderInfo get_rom_header_info() const override;
    const uint32_t* get_z_histogram() const override;
    const uint8_t* system_ram_data() const override;
    std::size_t system_ram_size() const override;
    std::string last_load_warning() const override { return m_last_load_warning; }

    bool handle_environment(unsigned cmd, void* data);
    void handle_video_frame(const void* data, unsigned width, unsigned height, std::size_t pitch);
    int16_t handle_input_state(unsigned port, unsigned device, unsigned index, unsigned id) const;

private:
    bool ensure_core_initialized(std::string& error_out);
    void reset_core();
    void ensure_frame_size(unsigned width, unsigned height);
    void write_xrgb8888_frame(const uint32_t* pixels, unsigned width, unsigned height,
                              std::size_t pitch);
    // Rasterises the frame's captured PGXP geometry into a per-pixel depth map
    // aligned with the composited frame. Returns null when the frame carried no
    // usable 3D geometry (menus, FMV), which keeps the screen flat.
    std::shared_ptr<const PsxDepthFrame> build_depth_frame(unsigned width, unsigned height);
    // --- Hardware renderer -------------------------------------------------
    // SwanStation's GPU_HW keeps a real PGXP depth buffer; QRD resolves that
    // instead of rasterising depth on the CPU. It needs a GL context on the
    // emulation thread sharing objects with the XR renderer's, so every step
    // here is conditional: if the context or the render target cannot be made,
    // the core stays on its software renderer and the CPU path takes over.
    // Internal resolution actually used. With the zero-copy handoff the CPU
    // cost no longer scales with this at all: the renderer samples the
    // emulator's texture directly, and the only readback is a fixed
    // native-resolution copy for ambilight/previews. What remains is GPU fill,
    // which is what the setting is supposed to trade against, so the cap is now
    // just the option's own maximum. If a game stutters at 4 the cause is fill
    // rate rather than the frame pipeline, and 2 is the safe step down.
    int effective_gpu_scale() const;
    static constexpr int kMaxHardwareGpuScale = 4;

    static uintptr_t hw_get_current_framebuffer();
    bool ensure_hw_framebuffer(int width, int height);
    bool init_depth_resolve();
    void run_depth_resolve(const PsxHwDepthInfo& info, unsigned target_fbo,
                           int target_w, int target_h, bool normalize);
    bool ensure_readback_resources(int native_w, int native_h);
    void consume_color_readback(const uint8_t* pixels);
    bool consume_range_readback(const uint8_t* pixels);
    void process_hw_frame(unsigned width, unsigned height);
    // Readback path only: pulls the resolved depth back to CPU bytes so the
    // renderer's software path can consume it.
    void consume_depth_readback(const uint8_t* pixels, int w, int h);
    void destroy_hw_resources();
    void apply_controller_ports();

    // One rotating render target per in-flight frame. The emulator draws into
    // one while the renderer samples another, so nothing is copied between them.
    struct HwSlot {
        unsigned fbo = 0;
        unsigned color = 0;
        unsigned depth_rb = 0;
        unsigned depth_fbo = 0;
        unsigned depth_tex = 0;
        int depth_w = 0;
        int depth_h = 0;
    };

    // A resolved depth image, tagged with the VRAM rectangle it was resolved
    // from. Depth is resolved for the region the core just drew, which is not
    // the region being scanned out, so entries are held until their rectangle
    // comes up for display and can be paired with the matching colour.
    struct DepthEntry {
        unsigned fbo = 0;
        unsigned tex = 0;
        int w = 0;
        int h = 0;
        int rect_x = 0;
        int rect_y = 0;
        int rect_w = 0;
        int rect_h = 0;
        bool valid = false;
    };

    // Drops the smoothed depth range so the next frame re-seeds it from scratch.
    // Anything that discontinuously replaces the scene must call this, or the
    // range eases across the cut and the depth reads wrong until it catches up.
    void reset_depth_range() { m_depth_range_valid = false; }

    FrameOutput        m_frame;
    EmulatorInputState m_input;
    std::vector<uint8_t> m_rom_bytes;
    std::string m_loaded_rom_path;
    std::string m_backend_name;
    std::string m_last_load_warning;
    bool m_core_initialized = false;
    bool m_game_loaded = false;
    bool m_preview_mode = false;
    bool m_preview_allow_audio = false;
    std::uint64_t m_video_frame_count = 0;
    bool m_last_frame_had_visible_pixels = false;
    int  m_texture_filter = 0;
    bool m_gun_mode = false;
    bool m_dual_gun_mode = false;
    bool m_auto_frame_skip = false;
    int m_gpu_resolution = 1;
    bool m_variables_dirty = false;
    double m_frame_rate_hz = 59.826;
    // Smoothed 1/w normalisation range carried between frames. See
    // build_depth_frame() for why the raw per-frame percentiles are not used.
    retro_hw_render_callback m_hw_render{};
    // Shared GL context exists and a hardware renderer may be advertised.
    bool m_hw_context_ready = false;
    // The core accepted our hardware render callback.
    bool m_hw_render_valid = false;
    // context_reset() has run and frames now arrive through the FBO.
    bool m_hw_active = false;
    HwSlot m_hw_slots[kPsxGpuFrameSlots];
    int m_hw_current_slot = 0;
    // Same count as the colour slots: an entry must survive until the renderer
    // has finished with the frame that referenced it.
    DepthEntry m_depth_entries[kPsxGpuFrameSlots];
    int m_depth_entry_index = 0;
    // Set when a slot has been handed to the core but not yet published, so a
    // second get_current_framebuffer() within one frame cannot rotate the
    // target out from under a half-drawn image.
    bool m_hw_slot_pending = false;
    int m_hw_fbo_w = 0;
    int m_hw_fbo_h = 0;
    unsigned m_resolve_program = 0;
    unsigned m_resolve_vao = 0;
    int m_resolve_u_depth = -1;
    int m_resolve_u_rect = -1;
    int m_resolve_u_tex_size = -1;
    int m_resolve_u_range = -1;
    int m_resolve_u_normalize = -1;
    int m_resolve_u_flat_value = -1;
    int m_resolve_u_background_value = -1;
    // Native-resolution colour copy for the CPU pipeline, and a tiny raw-depth
    // copy for the percentile range. Both go through a two-deep pixel buffer
    // ring so the readback never stalls the emulation thread.
    unsigned m_small_fbo = 0;
    unsigned m_small_tex = 0;
    int m_small_w = 0;
    int m_small_h = 0;
    unsigned m_range_fbo = 0;
    unsigned m_range_tex = 0;
    unsigned m_pbo_color[2] = {0, 0};
    unsigned m_pbo_range[2] = {0, 0};
    unsigned m_pbo_depth[2] = {0, 0};
    std::size_t m_pbo_depth_bytes = 0;
    // Mirrors VrState::psx_render_path. Only Zero-Copy vs Readback is honoured
    // live; Software is decided when the core boots.
    int m_render_path = 0;
    // Latched at ROM load: the core cannot change renderer at runtime.
    bool m_want_hardware = true;
    std::vector<uint8_t> m_depth_bytes;
    std::size_t m_pbo_color_bytes = 0;
    std::size_t m_pbo_range_bytes = 0;
    int m_pbo_index = 0;
    bool m_pbo_primed = false;
    bool m_hw_range_usable = false;

    bool  m_depth_range_valid = false;
    float m_depth_range_lo = 0.0f;
    float m_depth_range_hi = 0.0f;

};

} // namespace qrd
