#pragma once
#include "common/rectangle.h"
#include "common/window_info.h"
#include "types.h"
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

enum class HostDisplayPixelFormat : uint32_t
{
  Unknown,
  RGBA8,
  BGRA8,
  RGB565,
  RGBA5551,
  Count
};

// An abstracted RGBA8 texture.
class HostDisplayTexture
{
public:
  virtual ~HostDisplayTexture();

  virtual void* GetHandle() const = 0;
  virtual uint32_t GetWidth() const = 0;
  virtual uint32_t GetHeight() const = 0;
  virtual uint32_t GetSamples() const = 0;
};

// Interface to the frontend's renderer.
class HostDisplay
{
public:
  enum class RenderAPI
  {
    None,
    D3D11,
    D3D12,
    Vulkan,
    OpenGL,
    OpenGLES
  };

  virtual ~HostDisplay();

  ALWAYS_INLINE int32_t GetWindowWidth() const { return static_cast<int32_t>(m_window_info.surface_width); }
  ALWAYS_INLINE int32_t GetWindowHeight() const { return static_cast<int32_t>(m_window_info.surface_height); }

  // Position is relative to the top-left corner of the window.
  ALWAYS_INLINE int32_t GetMousePositionX() const { return m_mouse_position_x; }
  ALWAYS_INLINE int32_t GetMousePositionY() const { return m_mouse_position_y; }
  ALWAYS_INLINE void SetMousePosition(int32_t x, int32_t y)
  {
    m_mouse_position_x = x;
    m_mouse_position_y = y;
  }

  // Lightgun position cached per frame at controller-update time, in
  // libretro's normalized [-0x7FFF, 0x7FFF] screen coordinates. The HW
  // renderers consume these directly so they don't have to re-poll
  // input from inside the render path - see retro_run_frame() in
  // libretro_host_interface.cpp where they are populated. Reading
  // input twice per frame is undefined per the libretro spec
  // (frontends may return stale or fresh values), so renderers must
  // never call g_retro_input_state_callback themselves.
  // One slot per controller port, so a second gun does not overwrite the
  // first one's cursor position (the renderers draw a crosshair per slot).
  enum : uint32_t { MAX_LIGHTGUN_SLOTS = 2 };

  ALWAYS_INLINE int16_t GetLightgunRawX(uint32_t slot = 0) const { return m_lightgun_raw_x[slot]; }
  ALWAYS_INLINE int16_t GetLightgunRawY(uint32_t slot = 0) const { return m_lightgun_raw_y[slot]; }
  ALWAYS_INLINE bool IsLightgunOffscreen(uint32_t slot = 0) const { return m_lightgun_offscreen[slot]; }
  ALWAYS_INLINE void SetLightgunState(uint32_t slot, int16_t raw_x, int16_t raw_y, bool offscreen)
  {
    if (slot >= MAX_LIGHTGUN_SLOTS)
      return;

    m_lightgun_raw_x[slot] = raw_x;
    m_lightgun_raw_y[slot] = raw_y;
    m_lightgun_offscreen[slot] = offscreen;
  }
  // Ports without a gun this frame must not leave a stale crosshair behind.
  ALWAYS_INLINE void ClearLightgunState(uint32_t slot)
  {
    if (slot < MAX_LIGHTGUN_SLOTS)
      m_lightgun_offscreen[slot] = true;
  }

  virtual RenderAPI GetRenderAPI() const = 0;
  virtual void* GetRenderDevice() const = 0;
  virtual void* GetRenderContext() const = 0;

  virtual bool CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name, bool debug_device,
                                  bool threaded_presentation) = 0;
  virtual bool InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device,
                                      bool threaded_presentation) = 0;
  virtual void DestroyRenderDevice() = 0;
  virtual bool ChangeRenderWindow(const WindowInfo& wi) = 0;
  virtual bool CreateResources() = 0;
  virtual void DestroyResources() = 0;

  /// Call when the window size changes externally to recreate any resources.
  virtual void ResizeRenderWindow(int32_t new_window_width, int32_t new_window_height) = 0;

  /// Creates an abstracted RGBA8 texture. If dynamic, the texture can be updated with UpdateTexture() below.
  virtual std::unique_ptr<HostDisplayTexture> CreateTexture(uint32_t width, uint32_t height, uint32_t layers, uint32_t levels, uint32_t samples,
                                                            HostDisplayPixelFormat format, const void* data,
                                                            uint32_t data_stride, bool dynamic = false) = 0;

  /// Returns false if the window was completely occluded.
  virtual bool Render() = 0;

  const void* GetDisplayTextureHandle() const { return m_display_texture_handle; }
  int32_t GetDisplayWidth() const { return m_display_width; }
  int32_t GetDisplayHeight() const { return m_display_height; }
  float GetDisplayAspectRatio() const { return m_display_aspect_ratio; }

  void ClearDisplayTexture()
  {
    m_display_texture_handle = nullptr;
    m_display_texture_width = 0;
    m_display_texture_height = 0;
    m_display_texture_view_x = 0;
    m_display_texture_view_y = 0;
    m_display_texture_view_width = 0;
    m_display_texture_view_height = 0;
  }

  void SetDisplayTexture(void* texture_handle, HostDisplayPixelFormat texture_format, int32_t texture_width,
                         int32_t texture_height, int32_t view_x, int32_t view_y, int32_t view_width, int32_t view_height)
  {
    m_display_texture_handle = texture_handle;
    m_display_texture_format = texture_format;
    m_display_texture_width = texture_width;
    m_display_texture_height = texture_height;
    m_display_texture_view_x = view_x;
    m_display_texture_view_y = view_y;
    m_display_texture_view_width = view_width;
    m_display_texture_view_height = view_height;
  }

  void SetDisplayParameters(int32_t display_width, int32_t display_height, int32_t active_left, int32_t active_top, int32_t active_width,
                            int32_t active_height, float display_aspect_ratio)
  {
    m_display_width = display_width;
    m_display_height = display_height;
    m_display_active_left = active_left;
    m_display_active_top = active_top;
    m_display_active_width = active_width;
    m_display_active_height = active_height;
    m_display_aspect_ratio = display_aspect_ratio;
  }

  static uint32_t GetDisplayPixelFormatSize(HostDisplayPixelFormat format);

  virtual bool SupportsDisplayPixelFormat(HostDisplayPixelFormat format) const = 0;

  virtual bool BeginSetDisplayPixels(HostDisplayPixelFormat format, uint32_t width, uint32_t height, void** out_buffer,
                                     uint32_t* out_pitch) = 0;
  virtual void EndSetDisplayPixels() = 0;
  virtual bool SetDisplayPixels(HostDisplayPixelFormat format, uint32_t width, uint32_t height, const void* buffer, uint32_t pitch);

  /// Sets the software cursor to the specified texture. Ownership of the texture is transferred.
  void SetSoftwareCursor(std::unique_ptr<HostDisplayTexture> texture, float scale = 1.0f);

  /// Sets the software cursor to the specified image.
  bool SetSoftwareCursor(const void* pixels, uint32_t width, uint32_t height, uint32_t stride, float scale = 1.0f);

  /// Disables the software cursor.
  void ClearSoftwareCursor();

  /// Helper function for computing the draw rectangle in a larger window.
  std::tuple<int32_t, int32_t, int32_t, int32_t> CalculateDrawRect(int32_t window_width, int32_t window_height, int32_t top_margin,
                                                   bool apply_aspect_ratio = true) const;

  /// Helper function for converting window coordinates to display coordinates.
  std::tuple<float, float> ConvertWindowCoordinatesToDisplayCoordinates(int32_t window_x, int32_t window_y, int32_t window_width,
                                                                        int32_t window_height, int32_t top_margin) const;

protected:
  ALWAYS_INLINE bool HasSoftwareCursor() const { return static_cast<bool>(m_cursor_texture); }
  ALWAYS_INLINE bool HasDisplayTexture() const { return (m_display_texture_handle != nullptr); }

  void CalculateDrawRect(int32_t window_width, int32_t window_height, float* out_left, float* out_top, float* out_width,
                         float* out_height, float* out_left_padding, float* out_top_padding, float* out_scale,
                         float* out_x_scale, bool apply_aspect_ratio = true) const;

  WindowInfo m_window_info;

  int32_t m_mouse_position_x = 0;
  int32_t m_mouse_position_y = 0;

  // Cached per-frame libretro lightgun state, one entry per port - see
  // SetLightgunState().
  int16_t  m_lightgun_raw_x[MAX_LIGHTGUN_SLOTS] = {};
  int16_t  m_lightgun_raw_y[MAX_LIGHTGUN_SLOTS] = {};
  bool m_lightgun_offscreen[MAX_LIGHTGUN_SLOTS] = {true, true};

  int32_t m_display_width = 0;
  int32_t m_display_height = 0;
  int32_t m_display_active_left = 0;
  int32_t m_display_active_top = 0;
  int32_t m_display_active_width = 0;
  int32_t m_display_active_height = 0;
  float m_display_aspect_ratio = 1.0f;

  void* m_display_texture_handle = nullptr;
  HostDisplayPixelFormat m_display_texture_format = HostDisplayPixelFormat::Count;
  int32_t m_display_texture_width = 0;
  int32_t m_display_texture_height = 0;
  int32_t m_display_texture_view_x = 0;
  int32_t m_display_texture_view_y = 0;
  int32_t m_display_texture_view_width = 0;
  int32_t m_display_texture_view_height = 0;

  std::unique_ptr<HostDisplayTexture> m_cursor_texture;
  float m_cursor_texture_scale = 1.0f;
};
