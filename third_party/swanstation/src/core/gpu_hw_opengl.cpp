#include "gpu_hw_opengl.h"
#include "psx_hw_depth_bridge.h"
#include "common/align.h"
#include "common/log.h"
#include "common/state_wrapper.h"
#include "common/timer.h"
#include "gpu_hw_shadergen.h"
#include "host_display.h"
#include "shader_cache_version.h"
#include "core/host_interface.h"
#include <cstring>
#include <libretro.h>
#include "system.h"
#include "texture_replacements.h"
Log_SetChannel(GPU_HW_OpenGL);

class LibretroOpenGLHostDisplayTexture : public HostDisplayTexture
{
public:
  LibretroOpenGLHostDisplayTexture(GL::Texture texture)
    : m_texture(std::move(texture))
  {
  }
  ~LibretroOpenGLHostDisplayTexture() override = default;

  void* GetHandle() const override { return reinterpret_cast<void*>(static_cast<uintptr_t>(m_texture.GetGLId())); }
  uint32_t GetWidth() const override { return m_texture.GetWidth(); }
  uint32_t GetHeight() const override { return m_texture.GetHeight(); }
  uint32_t GetSamples() const override { return m_texture.GetSamples(); }

  GLuint GetGLID() const { return m_texture.GetGLId(); }

private:
  GL::Texture m_texture;
};

LibretroOpenGLHostDisplay::LibretroOpenGLHostDisplay() = default;

LibretroOpenGLHostDisplay::~LibretroOpenGLHostDisplay() = default;

HostDisplay::RenderAPI LibretroOpenGLHostDisplay::GetRenderAPI() const
{
  return m_is_gles ? HostDisplay::RenderAPI::OpenGLES : HostDisplay::RenderAPI::OpenGL;
}

void* LibretroOpenGLHostDisplay::GetRenderDevice() const
{
  return nullptr;
}

void* LibretroOpenGLHostDisplay::GetRenderContext() const
{
  return nullptr;
}

static constexpr std::array<std::tuple<GLenum, GLenum, GLenum>, static_cast<uint32_t>(HostDisplayPixelFormat::Count)>
  s_display_pixel_format_mapping = {{
    {},                                                  // Unknown
    {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE},               // RGBA8
    {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE},               // BGRA8
    {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5},        // RGB565
    {GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV} // RGBA5551
  }};

std::unique_ptr<HostDisplayTexture> LibretroOpenGLHostDisplay::CreateTexture(uint32_t width, uint32_t height, uint32_t layers,
                                                                             uint32_t levels, uint32_t samples,
                                                                             HostDisplayPixelFormat format,
                                                                             const void* data, uint32_t data_stride,
                                                                             bool dynamic /* = false */)
{
  if (layers != 1 || levels != 1)
    return {};

  const auto [gl_internal_format, gl_format, gl_type] = s_display_pixel_format_mapping[static_cast<uint32_t>(format)];

  // TODO: Set pack width
  GL::Texture tex;
  if (!tex.Create(width, height, samples, gl_internal_format, gl_format, gl_type, data, data_stride))
    return {};

  return std::make_unique<LibretroOpenGLHostDisplayTexture>(std::move(tex));
}

bool LibretroOpenGLHostDisplay::SupportsDisplayPixelFormat(HostDisplayPixelFormat format) const
{
  return (std::get<0>(s_display_pixel_format_mapping[static_cast<uint32_t>(format)]) != static_cast<GLenum>(0));
}

bool LibretroOpenGLHostDisplay::BeginSetDisplayPixels(HostDisplayPixelFormat format, uint32_t width, uint32_t height,
                                                      void** out_buffer, uint32_t* out_pitch)
{
  const uint32_t pixel_size = GetDisplayPixelFormatSize(format);
  const uint32_t stride = Common::AlignUpPow2(width * pixel_size, 4);
  const uint32_t size_required = stride * height * pixel_size;

  const uint32_t buffer_size = Common::AlignUpPow2(size_required * 2, 4 * 1024 * 1024);
  if (!m_display_pixels_texture_pbo || m_display_pixels_texture_pbo->GetSize() < buffer_size)
  {
    m_display_pixels_texture_pbo.reset();
    m_display_pixels_texture_pbo = GL::StreamBuffer::Create(GL_PIXEL_UNPACK_BUFFER, buffer_size);
    if (!m_display_pixels_texture_pbo)
      return false;
  }

  const auto map = m_display_pixels_texture_pbo->Map(GetDisplayPixelFormatSize(format), size_required);
  m_display_texture_format = format;
  m_display_pixels_texture_pbo_map_offset = map.buffer_offset;
  m_display_pixels_texture_pbo_map_size = size_required;
  *out_buffer = map.pointer;
  *out_pitch = stride;

  glBindTexture(GL_TEXTURE_2D, m_display_pixels_texture_id);
  SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_display_pixels_texture_id)), format, width, height,
                    0, 0, width, height);
  return true;
}

void LibretroOpenGLHostDisplay::EndSetDisplayPixels()
{
  const uint32_t width = static_cast<uint32_t>(m_display_texture_view_width);
  const uint32_t height = static_cast<uint32_t>(m_display_texture_view_height);

  const auto [gl_internal_format, gl_format, gl_type] =
    s_display_pixel_format_mapping[static_cast<uint32_t>(m_display_texture_format)];

  glBindTexture(GL_TEXTURE_2D, m_display_pixels_texture_id);

  m_display_pixels_texture_pbo->Unmap(m_display_pixels_texture_pbo_map_size);
  m_display_pixels_texture_pbo->Bind();
  glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, width, height, 0, gl_format, gl_type,
               reinterpret_cast<void*>(static_cast<uintptr_t>(m_display_pixels_texture_pbo_map_offset)));
  m_display_pixels_texture_pbo->Unbind();

  m_display_pixels_texture_pbo_map_offset = 0;
  m_display_pixels_texture_pbo_map_size = 0;

  glBindTexture(GL_TEXTURE_2D, 0);
}

bool LibretroOpenGLHostDisplay::SetDisplayPixels(HostDisplayPixelFormat format, uint32_t width, uint32_t height,
                                                 const void* buffer, uint32_t pitch)
{
  glBindTexture(GL_TEXTURE_2D, m_display_pixels_texture_id);

  const auto [gl_internal_format, gl_format, gl_type] = s_display_pixel_format_mapping[static_cast<uint32_t>(format)];
  const uint32_t pixel_size = GetDisplayPixelFormatSize(format);
  const bool is_packed_tightly = (pitch == (pixel_size * width));

  // If we have GLES3, we can set row_length.
  if (!is_packed_tightly)
    glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / pixel_size);

  glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, width, height, 0, gl_format, gl_type, buffer);

  if (!is_packed_tightly)
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

  glBindTexture(GL_TEXTURE_2D, 0);

  SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_display_pixels_texture_id)), format, width, height,
                    0, 0, width, height);
  return true;
}

const char* LibretroOpenGLHostDisplay::GetGLSLVersionString() const
{
  if (GetRenderAPI() == RenderAPI::OpenGLES)
  {
    if (GLAD_GL_ES_VERSION_3_0)
      return "#version 300 es";
    else
      return "#version 100";
  }
  else
  {
    if (GLAD_GL_VERSION_3_3)
      return "#version 330";
    else
      return "#version 130";
  }
}

std::string LibretroOpenGLHostDisplay::GetGLSLVersionHeader() const
{
  std::string header = GetGLSLVersionString();
  header += "\n\n";
  if (GetRenderAPI() == RenderAPI::OpenGLES)
  {
    header += "precision highp float;\n";
    header += "precision highp int;\n\n";
  }

  return header;
}

static void APIENTRY GLDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                     const GLchar* message, const void* userParam)
{
  switch (severity)
  {
    case GL_DEBUG_SEVERITY_HIGH_KHR:
      Log_ErrorPrint(message);
      break;
    case GL_DEBUG_SEVERITY_MEDIUM_KHR:
      Log_WarningPrint(message);
      break;
    case GL_DEBUG_SEVERITY_LOW_KHR:
      Log_InfoPrint(message);
      break;
    case GL_DEBUG_SEVERITY_NOTIFICATION:
      break;
  }
}

static bool TryDesktopVersions(retro_hw_render_callback* cb)
{
  static constexpr std::array<std::tuple<uint32_t, uint32_t>, 11> desktop_versions_to_try = {
    {/*{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, */ {3, 3}, {3, 2}, {3, 1}, {3, 0}}};

  for (const auto& [major, minor] : desktop_versions_to_try)
  {
    if (major > 3 || (major == 3 && minor >= 2))
    {
      cb->context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
      cb->version_major = major;
      cb->version_minor = minor;
    }
    else
    {
      cb->context_type = RETRO_HW_CONTEXT_OPENGL;
      cb->version_major = 0;
      cb->version_minor = 0;
    }

    if (g_retro_environment_callback(RETRO_ENVIRONMENT_SET_HW_RENDER, cb))
      return true;
  }

  return false;
}

static bool TryESVersions(retro_hw_render_callback* cb)
{
  static constexpr std::array<std::tuple<uint32_t, uint32_t>, 4> es_versions_to_try = {{{3, 2}, {3, 1}, {3, 0}}};

  for (const auto& [major, minor] : es_versions_to_try)
  {
    if (major >= 3 && minor > 0)
    {
      cb->context_type = RETRO_HW_CONTEXT_OPENGLES_VERSION;
      cb->version_major = major;
      cb->version_minor = minor;
    }
    else
    {
      cb->context_type = RETRO_HW_CONTEXT_OPENGLES3;
      cb->version_major = 0;
      cb->version_minor = 0;
    }

    if (g_retro_environment_callback(RETRO_ENVIRONMENT_SET_HW_RENDER, cb))
      return true;
  }

  return false;
}

bool LibretroOpenGLHostDisplay::RequestHardwareRendererContext(retro_hw_render_callback* cb, bool prefer_gles)
{
  // Prefer a desktop OpenGL context where possible. If we can't get this, try OpenGL ES.
  cb->cache_context = false;
  cb->bottom_left_origin = true;

  if (!prefer_gles)
  {
    if (TryDesktopVersions(cb) || TryESVersions(cb))
      return true;
  }
  else
  {
    if (TryESVersions(cb) || TryDesktopVersions(cb))
      return true;
  }

  Log_ErrorPrint("Failed to set any GL HW renderer");
  return false;
}

bool LibretroOpenGLHostDisplay::CreateRenderDevice(const WindowInfo& wi, std::string_view adapter_name,
                                                   bool debug_device, bool threaded_presentation)
{
  // gross - but can't do much because of the GLADloadproc below.
  static retro_hw_render_callback* cb;
  cb = static_cast<retro_hw_render_callback*>(wi.display_connection);

  m_window_info = wi;
  m_is_gles = (cb->context_type == RETRO_HW_CONTEXT_OPENGLES3 || cb->context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION);

  const GLADloadproc get_proc_address = [](const char* sym) -> void* {
    return reinterpret_cast<void*>(cb->get_proc_address(sym));
  };

  // Load GLAD.
  const auto load_result = m_is_gles ? gladLoadGLES2Loader(get_proc_address) : gladLoadGLLoader(get_proc_address);
  if (!load_result)
  {
    Log_ErrorPrintf("Failed to load GL functions");
    return false;
  }

  return true;
}

bool LibretroOpenGLHostDisplay::InitializeRenderDevice(std::string_view shader_cache_directory, bool debug_device,
		bool threaded_presentation)
{
  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, reinterpret_cast<GLint*>(&m_uniform_buffer_alignment));

  if (debug_device && GLAD_GL_KHR_debug)
  {
    if (GetRenderAPI() == RenderAPI::OpenGLES)
      glDebugMessageCallbackKHR(GLDebugCallback, nullptr);
    else
      glDebugMessageCallback(GLDebugCallback, nullptr);

    glEnable(GL_DEBUG_OUTPUT);
  }

  return CreateResources();
}

void LibretroOpenGLHostDisplay::DestroyRenderDevice()
{
  ClearSoftwareCursor();
  DestroyResources();
}

void LibretroOpenGLHostDisplay::ResizeRenderWindow(int32_t new_window_width, int32_t new_window_height)
{
  m_window_info.surface_width = static_cast<uint32_t>(new_window_width);
  m_window_info.surface_height = static_cast<uint32_t>(new_window_height);
}

bool LibretroOpenGLHostDisplay::ChangeRenderWindow(const WindowInfo& new_wi)
{
  m_window_info = new_wi;
  return true;
}

bool LibretroOpenGLHostDisplay::CreateResources()
{
  static constexpr char fullscreen_quad_vertex_shader[] = R"(
uniform vec4 u_src_rect;
out vec2 v_tex0;

void main()
{
  vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  v_tex0 = u_src_rect.xy + pos * u_src_rect.zw;
  gl_Position = vec4(pos * vec2(2.0f, -2.0f) + vec2(-1.0f, 1.0f), 0.0f, 1.0f);
}
)";

  static constexpr char display_fragment_shader[] = R"(
uniform sampler2D samp0;

in vec2 v_tex0;
out vec4 o_col0;

void main()
{
  o_col0 = vec4(texture(samp0, v_tex0).rgb, 1.0);
}
)";

  static constexpr char cursor_fragment_shader[] = R"(
uniform sampler2D samp0;

in vec2 v_tex0;
out vec4 o_col0;

void main()
{
  o_col0 = texture(samp0, v_tex0);
}
)";

  if (!m_display_program.Compile(GetGLSLVersionHeader() + fullscreen_quad_vertex_shader, {},
                                 GetGLSLVersionHeader() + display_fragment_shader) ||
      !m_cursor_program.Compile(GetGLSLVersionHeader() + fullscreen_quad_vertex_shader, {},
                                GetGLSLVersionHeader() + cursor_fragment_shader))
  {
    Log_ErrorPrintf("Failed to compile display shaders");
    return false;
  }

  if (GetRenderAPI() != RenderAPI::OpenGLES)
  {
    m_display_program.BindFragData(0, "o_col0");
    m_cursor_program.BindFragData(0, "o_col0");
  }

  if (!m_display_program.Link() || !m_cursor_program.Link())
  {
    Log_ErrorPrintf("Failed to link display programs");
    return false;
  }

  m_display_program.Bind();
  m_display_program.RegisterUniform("u_src_rect");
  m_display_program.RegisterUniform("samp0");
  m_display_program.Uniform1i(1, 0);
  m_cursor_program.Bind();
  m_cursor_program.RegisterUniform("u_src_rect");
  m_cursor_program.RegisterUniform("samp0");
  m_cursor_program.Uniform1i(1, 0);

  glGenVertexArrays(1, &m_display_vao);

  // samplers
  glGenSamplers(1, &m_display_nearest_sampler);
  glSamplerParameteri(m_display_nearest_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glSamplerParameteri(m_display_nearest_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenSamplers(1, &m_display_linear_sampler);
  glSamplerParameteri(m_display_linear_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri(m_display_linear_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  return true;
}

void LibretroOpenGLHostDisplay::DestroyResources()
{
  if (m_display_pixels_texture_id != 0)
  {
    glDeleteTextures(1, &m_display_pixels_texture_id);
    m_display_pixels_texture_id = 0;
  }

  if (m_display_vao != 0)
  {
    glDeleteVertexArrays(1, &m_display_vao);
    m_display_vao = 0;
  }
  if (m_display_linear_sampler != 0)
  {
    glDeleteSamplers(1, &m_display_linear_sampler);
    m_display_linear_sampler = 0;
  }
  if (m_display_nearest_sampler != 0)
  {
    glDeleteSamplers(1, &m_display_nearest_sampler);
    m_display_nearest_sampler = 0;
  }

  m_cursor_program.Destroy();
  m_display_program.Destroy();
}

void LibretroOpenGLHostDisplay::RenderSoftwareCursor(int32_t left, int32_t bottom, int32_t width, int32_t height,
                                             HostDisplayTexture* texture_handle)
{
  glViewport(left, bottom, width, height);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
  glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  m_cursor_program.Bind();
  glBindTexture(GL_TEXTURE_2D, static_cast<LibretroOpenGLHostDisplayTexture*>(texture_handle)->GetGLID());

  m_cursor_program.Uniform4f(0, 0.0f, 0.0f, 1.0f, 1.0f);
  glBindSampler(0, m_display_linear_sampler);
  glBindVertexArray(m_display_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindSampler(0, 0);
}

bool LibretroOpenGLHostDisplay::Render()
{
  // No display texture this frame -> send the libretro frame-dupe
  // signal (NULL frame), matching the software-renderer code path in
  // LibretroHostDisplay::Render(). Previously the HW renderers all
  // pushed a black-cleared FBO instead, which disagreed with the SW
  // path on identical emulator state (display-disabled, mode-change
  // gap, etc.) and produced visible black flashes where the SW
  // backend would frame-dupe cleanly.
  if (!HasDisplayTexture())
  {
    g_retro_video_refresh_callback(nullptr, 0, 0, 0);
    return true;
  }

  const GLuint fbo = static_cast<GLuint>(
    static_cast<retro_hw_render_callback*>(m_window_info.display_connection)->get_current_framebuffer());
  const uint32_t resolution_scale = g_host_interface_storage.GetResolutionScale();
  const uint32_t display_width = static_cast<uint32_t>(m_display_width) * resolution_scale;
  const uint32_t display_height = static_cast<uint32_t>(m_display_height) * resolution_scale;
  // Lightgun state was cached at controller-update time; do NOT call
  // g_retro_input_state_callback() from the renderer - that would
  // sample input twice per frame across two callsites, which is
  // undefined behavior per the libretro spec (frontends are not
  // required to keep input_state values fresh outside of a poll).
  // One cursor position per gun port: with two guns connected each needs its
  // own crosshair, otherwise both players aim by the same marker.
  int32_t pos_x[MAX_LIGHTGUN_SLOTS] = {};
  int32_t pos_y[MAX_LIGHTGUN_SLOTS] = {};
  for (uint32_t slot = 0; slot < MAX_LIGHTGUN_SLOTS; slot++)
  {
    if (IsLightgunOffscreen(slot))
      continue;

    pos_x[slot] = ((static_cast<int32_t>(GetLightgunRawX(slot)) + 0x7FFF) * display_width)  / 0xFFFF;
    pos_y[slot] = ((static_cast<int32_t>(GetLightgunRawY(slot)) + 0x7FFF) * display_height) / 0xFFFF;
  }

  glEnable(GL_SCISSOR_TEST);
  glScissor(0, 0, display_width, display_height);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);

  {
    const auto [left, top, width, height] = CalculateDrawRect(display_width, display_height, 0, false);
    RenderDisplay(left, top, width, height, m_display_texture_handle, m_display_texture_width, m_display_texture_height,
                  m_display_texture_view_x, m_display_texture_view_y, m_display_texture_view_width,
                  m_display_texture_view_height);
  }

  if (g_settings.controller_show_crosshair && HasSoftwareCursor())
  {
    const float width_scale = (display_width / 2400.0f);
    const float height_scale = (display_height / 1920.0f);
    const uint32_t cursor_extents_x = static_cast<uint32_t>(static_cast<float>(m_cursor_texture->GetWidth()) * width_scale);
    const uint32_t cursor_extents_y = static_cast<uint32_t>(static_cast<float>(m_cursor_texture->GetHeight()) * height_scale);

    for (uint32_t slot = 0; slot < MAX_LIGHTGUN_SLOTS; slot++)
    {
      if (pos_x[slot] <= 0 && pos_y[slot] <= 0)
        continue;

      const int32_t out_left = pos_x[slot] - cursor_extents_x;
      const int32_t out_top = pos_y[slot] - cursor_extents_y;
      const int32_t out_width = cursor_extents_x * 2u;
      const int32_t out_height = cursor_extents_y * 2u;

      RenderSoftwareCursor(out_left, display_height - out_top - out_height, out_width, out_height, m_cursor_texture.get());
    }
  }

  g_retro_video_refresh_callback(RETRO_HW_FRAME_BUFFER_VALID, display_width, display_height, 0);

  GL::Program::ResetLastProgram();
  return true;
}

void LibretroOpenGLHostDisplay::RenderDisplay(int32_t left, int32_t bottom, int32_t width, int32_t height, void* texture_handle,
                                              uint32_t texture_width, int32_t texture_height, int32_t texture_view_x,
                                              int32_t texture_view_y, int32_t texture_view_width, int32_t texture_view_height)
{
  glViewport(left, bottom, width, height);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<uintptr_t>(texture_handle)));
  m_display_program.Bind();

  m_display_program.Uniform4f(
    0, static_cast<float>(texture_view_x) / static_cast<float>(texture_width),
    static_cast<float>(texture_view_y) / static_cast<float>(texture_height),
    static_cast<float>(texture_view_width) / static_cast<float>(texture_width),
    static_cast<float>(texture_view_height) / static_cast<float>(texture_height));
  glBindSampler(0, m_display_nearest_sampler);
  glBindVertexArray(m_display_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindSampler(0, 0);
}

GPU_HW_OpenGL::GPU_HW_OpenGL() : GPU_HW() {}

GPU_HW_OpenGL::~GPU_HW_OpenGL()
{
  // Destroy objects which don't have destructors to clean them up
  if (m_vram_fbo_id != 0)
    glDeleteFramebuffers(1, &m_vram_fbo_id);
  if (m_vao_id != 0)
    glDeleteVertexArrays(1, &m_vao_id);
  if (m_attributeless_vao_id != 0)
    glDeleteVertexArrays(1, &m_attributeless_vao_id);
  if (m_texture_buffer_r16ui_texture != 0)
    glDeleteTextures(1, &m_texture_buffer_r16ui_texture);

  if (m_host_display)
  {
    m_host_display->ClearDisplayTexture();
    ResetGraphicsAPIState();
  }

  // One of our programs might've been bound.
  GL::Program::ResetLastProgram();
  glUseProgram(0);
}

bool GPU_HW_OpenGL::Initialize(HostDisplay* host_display)
{
  if (host_display->GetRenderAPI() != HostDisplay::RenderAPI::OpenGL &&
      host_display->GetRenderAPI() != HostDisplay::RenderAPI::OpenGLES)
  {
    Log_ErrorPrintf("Host render API type is incompatible");
    return false;
  }

  const bool opengl_is_available =
    ((host_display->GetRenderAPI() == HostDisplay::RenderAPI::OpenGL &&
      (GLAD_GL_VERSION_3_0 || GLAD_GL_ARB_uniform_buffer_object)) ||
     (host_display->GetRenderAPI() == HostDisplay::RenderAPI::OpenGLES && GLAD_GL_ES_VERSION_3_0));
  if (!opengl_is_available)
  {
    Log_ErrorPrintf("OpenGL renderer unavailable, your driver or hardware is not "
                    "recent enough. OpenGL 3.1 or OpenGL ES 3.0 is required.");
    return false;
  }

  SetCapabilities();

  if (!GPU_HW::Initialize(host_display))
    return false;

  if (!CreateFramebuffer())
  {
    Log_ErrorPrintf("Failed to create framebuffer");
    return false;
  }

  if (!CreateVertexBuffer())
  {
    Log_ErrorPrintf("Failed to create vertex buffer");
    return false;
  }

  if (!CreateUniformBuffer())
  {
    Log_ErrorPrintf("Failed to create uniform buffer");
    return false;
  }

  if (!CreateTextureBuffer())
  {
    Log_ErrorPrintf("Failed to create texture buffer");
    return false;
  }

  if (!CompilePrograms())
  {
    Log_ErrorPrintf("Failed to compile programs");
    return false;
  }

  RestoreGraphicsAPIState();
  return true;
}

void GPU_HW_OpenGL::Reset(bool clear_vram)
{
  GPU_HW::Reset(clear_vram);

  if (clear_vram)
    ClearFramebuffer();
}

bool GPU_HW_OpenGL::DoState(StateWrapper& sw, HostDisplayTexture** host_texture, bool update_display)
{
  if (host_texture)
  {
    HostDisplayTexture* tex = *host_texture;
    if (sw.IsReading())
    {
      if (tex->GetWidth() != m_vram_texture.GetWidth() || tex->GetHeight() != m_vram_texture.GetHeight() ||
          tex->GetSamples() != m_vram_texture.GetSamples())
      {
        return false;
      }

      CopyFramebufferForState(
        m_vram_texture.GetGLTarget(), static_cast<GLuint>(reinterpret_cast<uintptr_t>(tex->GetHandle())), 0, 0, 0,
        m_vram_texture.GetGLId(), m_vram_fbo_id, 0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight());
    }
    else
    {
      if (!tex || tex->GetWidth() != m_vram_texture.GetWidth() || tex->GetHeight() != m_vram_texture.GetHeight() ||
          tex->GetSamples() != m_vram_texture.GetSamples())
      {
        delete tex;

        tex = m_host_display
                ->CreateTexture(m_vram_texture.GetWidth(), m_vram_texture.GetHeight(), 1, 1,
                                m_vram_texture.GetSamples(), HostDisplayPixelFormat::RGBA8, nullptr, 0, false)
                .release();
        *host_texture = tex;
        if (!tex)
          return false;
      }

      CopyFramebufferForState(m_vram_texture.GetGLTarget(), m_vram_texture.GetGLId(), m_vram_fbo_id, 0, 0,
                              static_cast<GLuint>(reinterpret_cast<uintptr_t>(tex->GetHandle())), 0, 0, 0,
                              m_vram_texture.GetWidth(), m_vram_texture.GetHeight());
    }
  }

  return GPU_HW::DoState(sw, host_texture, update_display);
}

void GPU_HW_OpenGL::CopyFramebufferForState(GLenum target, GLuint src_texture, uint32_t src_fbo, uint32_t src_x, uint32_t src_y,
                                            GLuint dst_texture, uint32_t dst_fbo, uint32_t dst_x, uint32_t dst_y, uint32_t width,
                                            uint32_t height)
{
  if (target != GL_TEXTURE_2D && GLAD_GL_VERSION_4_3)
  {
    glCopyImageSubData(src_texture, target, 0, src_x, src_y, 0, dst_texture, target, 0, dst_x, dst_y, 0, width, height,
                       1);
  }
  else if (target != GL_TEXTURE_2D && GLAD_GL_EXT_copy_image)
  {
    glCopyImageSubDataEXT(src_texture, target, 0, src_x, src_y, 0, dst_texture, target, 0, dst_x, dst_y, 0, width,
                          height, 1);
  }
  else if (target != GL_TEXTURE_2D && GLAD_GL_OES_copy_image)
  {
    glCopyImageSubDataOES(src_texture, target, 0, src_x, src_y, 0, dst_texture, target, 0, dst_x, dst_y, 0, width,
                          height, 1);
  }
  else
  {
    if (src_fbo == 0)
    {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, m_state_copy_fbo_id);
      glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, src_texture, 0);
    }
    else
    {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    }

    if (dst_fbo == 0)
    {
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_state_copy_fbo_id);
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, dst_texture, 0);
    }
    else
    {
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
    }

    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(src_x, src_y, src_x + width, src_y + height, dst_x, dst_y, dst_x + width, dst_y + height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_SCISSOR_TEST);

    if (src_fbo == 0)
    {
      glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    }
    else if (dst_fbo == 0)
    {
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_vram_fbo_id);
  }
}

void GPU_HW_OpenGL::ResetGraphicsAPIState()
{
  GPU_HW::ResetGraphicsAPIState();

  glEnable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glBindVertexArray(0);
  m_uniform_stream_buffer->Unbind();
}

void GPU_HW_OpenGL::RestoreGraphicsAPIState()
{
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_vram_fbo_id);
  glViewport(0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight());

  glDisable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_SCISSOR_TEST);
  glDepthMask(GL_TRUE);
  glBindVertexArray(m_vao_id);
  m_uniform_stream_buffer->Bind();
  m_vram_read_texture.Bind();
  SetBlendMode();
  m_current_depth_test = 0;
  SetDepthFunc();
  SetScissorFromDrawingArea();
  m_batch_ubo_dirty = true;
}

void GPU_HW_OpenGL::UpdateSettings()
{
  GPU_HW::UpdateSettings();

  // See GPU_HW_D3D12::UpdateSettings for the rationale on
  // shader_source_changed vs shaders_changed: post-7b575a3 the
  // batch GLSL is invariant under resolution_scale / true_color /
  // scaled_dithering, so toggling them no longer requires
  // CompilePrograms (which would otherwise relink every program in
  // the batch matrix). The new values ride the per-batch UBO
  // upload on the next FlushRender.
  //
  // only_dim_changed: dim-cube setting (filter / true_color /
  // scaled_dithering) changed and nothing in non_dim_diff. With
  // the dim cache (filter outermost) this means "filter sub-cube
  // can be lazy-populated, other filters' sub-cubes stay valid".
  // This path routes around CompilePrograms entirely and calls
  // PrecompileBatchPrograms directly, so the filter-independent
  // non-batch programs (display / vram_fill / vram_read /
  // vram_write / vram_copy / vram_update_depth / downsample)
  // aren't churned through the disk-backed program cache for
  // no functional benefit. Mirrors the D3D11 / D3D12 / Vulkan
  // dim-cache fast paths.
  //
  // display_only_source_changed: chroma_smoothing flipped and
  // nothing else affecting shader source changed. The batch program
  // matrix, VRAM ops programs, and downsample programs all stay
  // valid; only the 6-slot m_display_programs cache needs to go.
  // Mirrors the D3D12 / D3D11 partial-clear from 57ac62e / 93e5db5.
  // OpenGL has no worker thread so there's no relaunch step on this
  // path - precompile_mode contract is simply "RebuildDisplayPrograms
  // is synchronous" in any mode.
  bool framebuffer_changed, shaders_changed, only_dim_changed, downsample_changed, shader_source_changed,
    display_only_source_changed;
  UpdateHWSettings(&framebuffer_changed, &shaders_changed, &only_dim_changed, &downsample_changed,
                   &shader_source_changed, &display_only_source_changed);

  // A downsample-mode change that UpdateHWSettings did not fold into
  // framebuffer_changed (GL only does Box, so this is Disabled <-> Box -
  // it never sets framebuffer_changed, which keys on Adaptive) still
  // needs the box downsample texture created or freed. CreateFramebuffer
  // (re)builds it for the new mode, so route the change through the
  // normal ReadVRAM -> CreateFramebuffer -> UpdateVRAM round-trip rather
  // than recreating it in isolation. Downsample toggling is a rare user
  // action, so the extra round-trip is not a concern.
  if (downsample_changed && !framebuffer_changed)
    framebuffer_changed = true;

  if (framebuffer_changed)
  {
    RestoreGraphicsAPIState();
    ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    ResetGraphicsAPIState();
    m_host_display->ClearDisplayTexture();
    CreateFramebuffer();
  }
  if (shader_source_changed)
  {
    if (display_only_source_changed)
    {
      // chroma_smoothing flipped and nothing else - rebuild the six
      // display programs against the new m_chroma_smoothing value
      // (which UpdateHWSettings has already written into the member).
      // The 144-cell batch program matrix, the VRAM ops programs, and
      // the downsample programs all stay valid. Cost is 6 link calls
      // (mostly cache hits from m_shader_cache for previously-seen
      // GLSL hashes) instead of the full CompilePrograms pass walking
      // the entire batch matrix.
      (void)RebuildDisplayPrograms();
    }
    else if (only_dim_changed)
    {
      // Filter (and/or cbuffer-only members) changed but nothing in
      // non_dim_diff. m_render_programs is filter-dimensioned so the
      // previous filter's sub-cube remains valid; cycling back to a
      // previously-visited filter is a slot-validity check inside
      // GetBatchProgram with no glCompileShader / glLinkProgram.
      //
      // The non-batch programs (display / vram_fill / vram_read /
      // vram_write / vram_copy / vram_update_depth / downsample)
      // are all filter-independent - none of them read
      // m_texture_filter inside shadergen (see gpu_hw_shadergen.cpp
      // where m_texture_filter is only ever referenced from
      // GenerateBatchFragmentShader and the WriteBatchTextureFilter
      // helper it calls). They keep working with the GL::Programs
      // they already have, so calling CompilePrograms here would
      // rebuild them via the disk-backed program cache (which hits
      // as glProgramBinary reloads on the same GLSL hashes) for
      // ~10-100ms of pure GL::Program churn per filter toggle with
      // no functional benefit.
      //
      // Instead, reconstruct m_shadergen so future calls into it
      // see current settings, build a progress tracker sized for
      // the batch matrix only, and call PrecompileBatchPrograms
      // directly. PrecompileBatchPrograms walks the new filter sub-
      // cube via GetBatchProgram (which is dim-cache aware -
      // already-populated cells from a previous visit short-circuit
      // on the IsValid() check).
      m_shadergen = std::make_unique<GPU_HW_ShaderGen>(
        m_host_display->GetRenderAPI(), m_resolution_scale, m_multisamples, m_per_sample_shading, m_true_color,
        m_scaled_dithering, m_texture_filtering, m_using_uv_limits, m_pgxp_depth_buffer, m_disable_color_perspective,
        m_supports_dual_source_blend);

      const uint32_t batch_progress_units =
        (g_settings.gpu_shader_precompile_mode == GPUShaderPrecompileMode::Enabled)
          ? CountReachableBatchShaders(m_supports_dual_source_blend)
          : 0u;
      ShaderCompileProgressTracker progress("Compiling Programs", batch_progress_units);
      (void)PrecompileBatchPrograms(progress);
    }
    else
    {
      // Non-dim, non-display-only source change: full rebuild walks
      // the current filter's sub-cube via CompilePrograms's clear +
      // build pass.
      CompilePrograms();
    }
  }

  // Rebuild the downsample program for the new mode. shader_source_changed
  // was false for a downsample-only change, so CompilePrograms above did
  // not run; without this the box program stays null after a Disabled <->
  // Box toggle and the downsample pass draws with no program. The texture
  // side was handled by the downsample_changed -> framebuffer_changed
  // promotion above.
  if (downsample_changed)
    CompileDownsampleProgram();

  if (framebuffer_changed)
  {
    RestoreGraphicsAPIState();
    UpdateVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT, m_vram_ptr, false, false);
    UpdateDepthBufferFromMaskBit();
    UpdateDisplay();
    ResetGraphicsAPIState();
  }
}

void GPU_HW_OpenGL::MapBatchVertexPointer(uint32_t required_vertices)
{
  const GL::StreamBuffer::MappingResult res =
    m_vertex_stream_buffer->Map(sizeof(BatchVertex), required_vertices * sizeof(BatchVertex));

  m_batch_start_vertex_ptr = static_cast<BatchVertex*>(res.pointer);
  m_batch_current_vertex_ptr = m_batch_start_vertex_ptr;
  m_batch_end_vertex_ptr = m_batch_start_vertex_ptr + res.space_aligned;
  m_batch_base_vertex = res.index_aligned;
}

void GPU_HW_OpenGL::UnmapBatchVertexPointer(uint32_t used_vertices)
{
  m_vertex_stream_buffer->Unmap(used_vertices * sizeof(BatchVertex));
  m_vertex_stream_buffer->Bind();
  m_batch_start_vertex_ptr = nullptr;
  m_batch_end_vertex_ptr = nullptr;
  m_batch_current_vertex_ptr = nullptr;
}

void GPU_HW_OpenGL::SetCapabilities()
{
  GLint max_texture_size = VRAM_WIDTH;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
  Log_InfoPrintf("Max texture size: %dx%d", max_texture_size, max_texture_size);
  m_max_resolution_scale = static_cast<uint32_t>(max_texture_size / VRAM_WIDTH);

  m_max_multisamples = 1;
  if (GLAD_GL_ARB_texture_storage || GLAD_GL_ES_VERSION_3_2)
  {
    glGetIntegerv(GL_MAX_SAMPLES, reinterpret_cast<GLint*>(&m_max_multisamples));
    if (m_max_multisamples == 0)
      m_max_multisamples = 1;
  }

  m_supports_per_sample_shading = GLAD_GL_VERSION_4_0 || GLAD_GL_ES_VERSION_3_2 || GLAD_GL_ARB_sample_shading;
  Log_InfoPrintf("Per-sample shading: %s", m_supports_per_sample_shading ? "supported" : "not supported");
  Log_InfoPrintf("Max multisamples: %u", m_max_multisamples);

  glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, reinterpret_cast<GLint*>(&m_uniform_buffer_alignment));
  Log_InfoPrintf("Uniform buffer offset alignment: %u", m_uniform_buffer_alignment);

  if (!GLAD_GL_VERSION_4_3 && !GLAD_GL_EXT_copy_image && !GLAD_GL_ES_VERSION_3_2 && !GLAD_GL_OES_copy_image)
    Log_WarningPrintf("GL_EXT/OES_copy_image missing, this may affect performance.");

#ifdef __APPLE__
  // Partial texture buffer uploads appear to be broken in macOS's OpenGL driver.
  m_use_texture_buffer_for_vram_writes = false;
#else
  m_use_texture_buffer_for_vram_writes = (GLAD_GL_VERSION_3_1 || GLAD_GL_ES_VERSION_3_2);

  // And Samsung's ANGLE/GLES driver?
  if (std::strstr(reinterpret_cast<const char*>(glGetString(GL_RENDERER)), "ANGLE"))
    m_use_texture_buffer_for_vram_writes = false;
#endif
  m_texture_stream_buffer_size = VRAM_UPDATE_TEXTURE_BUFFER_SIZE;
  if (m_use_texture_buffer_for_vram_writes)
  {
    GLint max_texel_buffer_size;
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, reinterpret_cast<GLint*>(&max_texel_buffer_size));
    Log_InfoPrintf("Max texel buffer size: %u", max_texel_buffer_size);
    if (max_texel_buffer_size < static_cast<int>(VRAM_WIDTH * VRAM_HEIGHT))
    {
      Log_WarningPrintf("Maximum texture buffer size is less than VRAM size, not using texel buffers.");
      m_use_texture_buffer_for_vram_writes = false;
    }
    else
    {
      m_texture_stream_buffer_size =
        std::min<uint32_t>(VRAM_UPDATE_TEXTURE_BUFFER_SIZE, static_cast<uint32_t>(max_texel_buffer_size) * sizeof(uint16_t));
    }
  }

  if (!m_use_texture_buffer_for_vram_writes)
  {
    // Try SSBOs.
    GLint max_fragment_storage_blocks = 0;
    GLint64 max_ssbo_size = 0;
    if (GLAD_GL_VERSION_4_3 || GLAD_GL_ES_VERSION_3_1 || GLAD_GL_ARB_shader_storage_buffer_object)
    {
      glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &max_fragment_storage_blocks);
      glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &max_ssbo_size);
    }

    Log_InfoPrintf("Max fragment shader storage blocks: %d", max_fragment_storage_blocks);
    Log_InfoPrintf("Max shader storage buffer size: %" PRId64, max_ssbo_size);
    m_use_ssbo_for_vram_writes = (max_fragment_storage_blocks > 0 &&
                                  max_ssbo_size >= static_cast<GLint64>(VRAM_WIDTH * VRAM_HEIGHT * sizeof(uint16_t)));
    if (m_use_ssbo_for_vram_writes)
    {
      Log_InfoPrintf("Using shader storage buffers for VRAM writes.");
      m_texture_stream_buffer_size =
        static_cast<uint32_t>(std::min<uint64_t>(VRAM_UPDATE_TEXTURE_BUFFER_SIZE, static_cast<uint64_t>(max_ssbo_size)));
    }
    else
    {
      Log_WarningPrintf("Texture buffers and SSBOs are not supported, VRAM writes will be slower and multisampling "
                        "will be unavailable.");
      m_max_multisamples = 1;
      m_supports_per_sample_shading = false;
    }
  }

  int max_dual_source_draw_buffers = 0;
  glGetIntegerv(GL_MAX_DUAL_SOURCE_DRAW_BUFFERS, &max_dual_source_draw_buffers);
  m_supports_dual_source_blend =
    (max_dual_source_draw_buffers > 0) &&
    (GLAD_GL_VERSION_3_3 || GLAD_GL_ARB_blend_func_extended || GLAD_GL_EXT_blend_func_extended);

  // adaptive smoothing would require texture views, which aren't in GLES.
  m_supports_adaptive_downsampling = false;

  // noperspective is not supported in GLSL ES.
  m_supports_disable_color_perspective = false;
}

bool GPU_HW_OpenGL::CreateFramebuffer()
{
  // scale vram size to internal resolution
  const uint32_t texture_width = VRAM_WIDTH * m_resolution_scale;
  const uint32_t texture_height = VRAM_HEIGHT * m_resolution_scale;
  const uint32_t multisamples = m_multisamples;

  if (!m_vram_texture.Create(texture_width, texture_height, multisamples, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, nullptr,
                             false, true) ||
      /* DEPTH_COMPONENT32F rather than DEPTH_COMPONENT16: in PGXP depth-buffer
       * mode the value written here is the reconstructed perspective W, carried
       * at full float32 through the vertex path - a 16-bit depth quantizes
       * exactly the precision PGXP exists to recover. Lossless for the legacy
       * ordered-depth path too, and unconditional because a mid-session PGXP-
       * depth toggle recompiles shaders without recreating this texture.
       * DEPTH_COMPONENT32F is a core sized format on GL 3.0+ / GLES 3.0+. */
      !m_vram_depth_texture.Create(texture_width, texture_height, multisamples, GL_DEPTH_COMPONENT32F,
                                   GL_DEPTH_COMPONENT, GL_FLOAT, nullptr, false) ||
      !m_vram_read_texture.Create(texture_width, texture_height, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false,
                                  true) ||
      !m_vram_read_texture.CreateFramebuffer() ||
      !m_vram_encoding_texture.Create(VRAM_WIDTH, VRAM_HEIGHT, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, nullptr,
                                      false) ||
      !m_vram_encoding_texture.CreateFramebuffer() ||
      !m_display_texture.Create(GPU_MAX_DISPLAY_WIDTH * m_resolution_scale, GPU_MAX_DISPLAY_HEIGHT * m_resolution_scale,
                                1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, nullptr, false) ||
      !m_display_texture.CreateFramebuffer())
  {
    return false;
  }

  if (m_vram_fbo_id == 0)
    glGenFramebuffers(1, &m_vram_fbo_id);

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_vram_fbo_id);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_vram_texture.GetGLTarget(),
                         m_vram_texture.GetGLId(), 0);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_vram_depth_texture.GetGLTarget(),
                         m_vram_depth_texture.GetGLId(), 0);

  if (m_downsample_mode == GPUDownsampleMode::Box)
  {
    if (!m_downsample_texture.Create(VRAM_WIDTH, VRAM_HEIGHT, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE) ||
        !m_downsample_texture.CreateFramebuffer())
    {
      return false;
    }
  }

  if (m_state_copy_fbo_id == 0)
    glGenFramebuffers(1, &m_state_copy_fbo_id);

  SetFullVRAMDirtyRectangle();
  return true;
}

void GPU_HW_OpenGL::ClearFramebuffer()
{
  const float depth_clear_value = m_pgxp_depth_buffer ? 1.0f : 0.0f;

  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  IsGLES() ? glClearDepthf(depth_clear_value) : glClearDepth(depth_clear_value);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  m_last_depth_z = 1.0f;
  SetFullVRAMDirtyRectangle();
}

bool GPU_HW_OpenGL::CreateVertexBuffer()
{
  m_vertex_stream_buffer = GL::StreamBuffer::Create(GL_ARRAY_BUFFER, VERTEX_BUFFER_SIZE);
  if (!m_vertex_stream_buffer)
    return false;

  m_vertex_stream_buffer->Bind();

  glGenVertexArrays(1, &m_vao_id);
  glBindVertexArray(m_vao_id);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glEnableVertexAttribArray(3);
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(0, 4, GL_FLOAT, false, sizeof(BatchVertex), reinterpret_cast<void*>(offsetof(BatchVertex, x)));
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, true, sizeof(BatchVertex),
                        reinterpret_cast<void*>(offsetof(BatchVertex, color)));
  glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(BatchVertex), reinterpret_cast<void*>(offsetof(BatchVertex, u)));
  glVertexAttribIPointer(3, 1, GL_UNSIGNED_INT, sizeof(BatchVertex),
                         reinterpret_cast<void*>(offsetof(BatchVertex, texpage)));
  glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, true, sizeof(BatchVertex),
                        reinterpret_cast<void*>(offsetof(BatchVertex, uv_limits)));
  glBindVertexArray(0);

  glGenVertexArrays(1, &m_attributeless_vao_id);
  return true;
}

bool GPU_HW_OpenGL::CreateUniformBuffer()
{
  m_uniform_stream_buffer = GL::StreamBuffer::Create(GL_UNIFORM_BUFFER, UNIFORM_BUFFER_SIZE);
  if (!m_uniform_stream_buffer)
    return false;

  return true;
}

bool GPU_HW_OpenGL::CreateTextureBuffer()
{
  const GLenum target =
    (m_use_ssbo_for_vram_writes ? GL_SHADER_STORAGE_BUFFER :
                                  (m_use_texture_buffer_for_vram_writes ? GL_TEXTURE_BUFFER : GL_PIXEL_UNPACK_BUFFER));
  m_texture_stream_buffer = GL::StreamBuffer::Create(target, m_texture_stream_buffer_size);
  if (!m_texture_stream_buffer)
    return false;

  if (m_use_texture_buffer_for_vram_writes)
  {
    glGenTextures(1, &m_texture_buffer_r16ui_texture);
    glBindTexture(GL_TEXTURE_BUFFER, m_texture_buffer_r16ui_texture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R16UI, m_texture_stream_buffer->GetGLBufferId());
  }

  m_texture_stream_buffer->Unbind();
  return true;
}

bool GPU_HW_OpenGL::CompilePrograms()
{
  // Reset every program object so the new compile starts from a
  // clean slate. Without this, on an UpdateSettings round-trip
  // through CompilePrograms (in Lazy / Disabled mode where the
  // matrix isn't refilled here) the lazy fault-in path in
  // GetBatchProgram would observe programs left over from the
  // previous settings - built with the old texture filter / scale
  // / etc. - and use them for the new draws. The synchronous
  // 'Enabled' loop further down also relies on this: it calls
  // GetBatchProgram which short-circuits on IsValid(), so a
  // pre-populated slot would silently keep its old program.
  //
  // GL::Program's move-assign operator calls Destroy() on the
  // destination before taking the source's state, so default-
  // assigning each slot to a temporary {} both glDeleteProgram's
  // the old handle and rewinds the slot to id = 0. 5-level
  // nesting matches the
  // [filter][render_mode][texture_mode][dithering][interlacing]
  // shape of m_render_programs added in the dim cache port.
  //
  // The dim-cache filter-only fast path doesn't go through here -
  // see UpdateSettings's only_dim_changed branch which calls
  // PrecompileBatchPrograms directly, preserving every existing
  // filter sub-cube. CompilePrograms is now only called from
  // Initialize and from UpdateSettings' full-rebuild branch (any
  // non-dim, non-display-only shader source change), both of
  // which want a clean-slate rebuild.
  for (auto& a : m_render_programs)
    for (auto& b : a)
      for (auto& c : b)
        for (auto& d : c)
          for (auto& slot : d)
            slot = GL::Program{};

  // Open the disk-backed program cache once. On subsequent calls
  // (UpdateSettings -> CompilePrograms) the instance still holds
  // the index from last time and the disk file hasn't moved, so
  // re-opening would double-count m_index and leak file handles.
  if (!m_shader_cache.IsOpen())
  {
    m_shader_cache.Open(IsGLES(), g_host_interface->GetShaderCacheBasePath(), SHADER_CACHE_VERSION);
  }
  GL::ShaderCache& shader_cache = m_shader_cache;

  m_use_binding_layout = GPU_HW_ShaderGen::UseGLSLBindingLayout();
  const bool use_binding_layout = m_use_binding_layout;

  m_shadergen = std::make_unique<GPU_HW_ShaderGen>(
    m_host_display->GetRenderAPI(), m_resolution_scale, m_multisamples, m_per_sample_shading, m_true_color,
    m_scaled_dithering, m_texture_filtering, m_using_uv_limits, m_pgxp_depth_buffer, m_disable_color_perspective,
    m_supports_dual_source_blend);
  GPU_HW_ShaderGen& shadergen = *m_shadergen;

  // OpenGL is the odd one out: the libretro hardware-renderer
  // protocol exposes only a single GL context bound to the
  // runloop, so there's no way to spin up a worker that compiles
  // in the background like D3D11/D3D12/Vulkan do. 'Lazy' therefore
  // degrades to the same shape as 'Disabled': skip the matrix at
  // CompilePrograms time, fault each combo in on the runloop the
  // first time the game dispatches a draw using it.
  //
  // 'Enabled' still does the full synchronous walk via
  // PrecompileBatchPrograms below, mirroring the D3D11/D3D12/Vulkan
  // commits.
  //
  // Structurally unreachable cells (reserved texture modes, two-pass
  // fallback modes for untextured polys, single-pass dual-source on
  // hardware that lacks it) are skipped via IsBatchShaderReachable.
  // batch_progress_units is sized to the same reachable count so the
  // progress bar lands at 100%.
  const uint32_t batch_progress_units =
    (g_settings.gpu_shader_precompile_mode == GPUShaderPrecompileMode::Enabled)
      ? CountReachableBatchShaders(m_supports_dual_source_blend)
      : 0u;

  ShaderCompileProgressTracker progress("Compiling Programs",
                                        batch_progress_units + (2 * 3) + (2 * 2) + 1 + 1 + 1 + 1 + 1);

  if (!PrecompileBatchPrograms(progress))
    return false;
  // For Lazy and Disabled: m_render_programs stays default-
  // constructed (program id 0); each cell is filled on first draw
  // by GetBatchProgram on the runloop thread.

  if (!RebuildDisplayPrograms())
    return false;
  for (uint8_t i = 0; i < 6; i++)
    progress.Increment();

  for (uint8_t wrapped = 0; wrapped < 2; wrapped++)
  {
    for (uint8_t interlaced = 0; interlaced < 2; interlaced++)
    {
      std::optional<GL::Program> prog = shader_cache.GetProgram(
        shadergen.GenerateScreenQuadVertexShader(), {},
        shadergen.GenerateVRAMFillFragmentShader(static_cast<bool>(wrapped), static_cast<bool>(interlaced)),
        [this, use_binding_layout](GL::Program& p) {
          if (!IsGLES() && !use_binding_layout)
            p.BindFragData(0, "o_col0");
        });
      if (!prog)
        return false;

      if (!use_binding_layout)
        prog->BindUniformBlock("UBOBlock", 1);

      m_vram_fill_programs[wrapped][interlaced] = std::move(*prog);
      progress.Increment();
    }
  }

  std::optional<GL::Program> prog =
    shader_cache.GetProgram(shadergen.GenerateScreenQuadVertexShader(), {}, shadergen.GenerateVRAMReadFragmentShader(),
                            [this, use_binding_layout](GL::Program& p) {
                              if (!IsGLES() && !use_binding_layout)
                                p.BindFragData(0, "o_col0");
                            });
  if (!prog)
    return false;

  if (!use_binding_layout)
  {
    prog->BindUniformBlock("UBOBlock", 1);
    prog->Bind();
    prog->Uniform1i("samp0", 0);
  }
  m_vram_read_program = std::move(*prog);
  progress.Increment();

  prog =
    shader_cache.GetProgram(shadergen.GenerateScreenQuadVertexShader(), {}, shadergen.GenerateVRAMCopyFragmentShader(),
                            [this, use_binding_layout](GL::Program& p) {
                              if (!IsGLES() && !use_binding_layout)
                                p.BindFragData(0, "o_col0");
                            });
  if (!prog)
    return false;

  if (!use_binding_layout)
  {
    prog->BindUniformBlock("UBOBlock", 1);
    prog->Bind();
    prog->Uniform1i("samp0", 0);
  }
  m_vram_copy_program = std::move(*prog);
  progress.Increment();

  prog = shader_cache.GetProgram(shadergen.GenerateScreenQuadVertexShader(), {},
                                 shadergen.GenerateVRAMUpdateDepthFragmentShader());
  if (!prog)
    return false;

  prog->Bind();
  prog->Uniform1i("samp0", 0);
  m_vram_update_depth_program = std::move(*prog);
  progress.Increment();

  if (m_use_texture_buffer_for_vram_writes || m_use_ssbo_for_vram_writes)
  {
    prog = shader_cache.GetProgram(shadergen.GenerateScreenQuadVertexShader(), {},
                                   shadergen.GenerateVRAMWriteFragmentShader(m_use_ssbo_for_vram_writes),
                                   [this, use_binding_layout](GL::Program& p) {
                                     if (!IsGLES() && !use_binding_layout)
                                       p.BindFragData(0, "o_col0");
                                   });
    if (!prog)
      return false;

    if (!use_binding_layout)
    {
      prog->BindUniformBlock("UBOBlock", 1);
      prog->Bind();
      prog->Uniform1i("samp0", 0);
    }
    m_vram_write_program = std::move(*prog);
  }

  progress.Increment();

  if (!CompileDownsampleProgram())
    return false;

  progress.Increment();
#undef UPDATE_PROGRESS

  return true;
}

bool GPU_HW_OpenGL::CompileDownsampleProgram()
{
  // (Re)build the Box downsample program for the current
  // m_downsample_mode. Split out of CompilePrograms so UpdateSettings
  // can rebuild it on a downsample-mode change: GPU_HW::UpdateHWSettings
  // keeps downsample mode out of shaders_changed (it does not touch the
  // batch matrix) and surfaces it via downsample_changed, so without
  // this the program stays null after a runtime Disabled <-> Box switch
  // and the downsample pass draws with no program. GL only supports Box
  // downsampling (m_supports_adaptive_downsampling = false), so Disabled
  // is a no-op. m_shader_cache / m_shadergen / m_use_binding_layout were
  // set up by the initial CompilePrograms and stay valid here (a
  // downsample-only change does not move any shadergen input).
  if (m_downsample_mode != GPUDownsampleMode::Box)
    return true;

  GL::ShaderCache& shader_cache = m_shader_cache;
  GPU_HW_ShaderGen& shadergen = *m_shadergen;
  const bool use_binding_layout = m_use_binding_layout;

  auto prog = shader_cache.GetProgram(shadergen.GenerateScreenQuadVertexShader(), {},
                                      shadergen.GenerateBoxSampleDownsampleFragmentShader(),
                                      [this, use_binding_layout](GL::Program& p) {
                                        if (!IsGLES() && !use_binding_layout)
                                          p.BindFragData(0, "o_col0");
                                      });
  if (!prog)
    return false;

  if (!use_binding_layout)
  {
    prog->Bind();
    prog->Uniform1i("samp0", 0);
  }

  m_downsample_program = std::move(*prog);
  return true;
}

bool GPU_HW_OpenGL::RebuildDisplayPrograms()
{
  // (Re)compile the 2x3 m_display_programs matrix against the
  // current m_chroma_smoothing. Called from CompilePrograms during
  // the initial / full-rebuild pass and from UpdateSettings on a
  // chroma_smoothing-only flip - chroma_smoothing is a DefineMacro
  // inside GenerateDisplayFragmentShader only (see
  // gpu_hw_shadergen.cpp:1056), so the batch program matrix, the
  // VRAM ops programs, the VRAM read/write/copy/update-depth
  // programs, and the downsample programs all stay valid through
  // a chroma toggle and don't need rebuilding. Costs 6 link calls
  // (effectively cache hits via m_shader_cache when an HLSL/GLSL
  // hash has been seen before; cold builds otherwise) instead of
  // the full CompilePrograms pass.
  //
  // chroma_smoothing only takes effect on the depth_24bit paths
  // (see the SMOOTH_CHROMA DefineMacro in
  // GenerateDisplayFragmentShader), so technically only three of
  // the six display programs depend on it. Rebuilding all six
  // anyway keeps this path simple - on a chroma toggle the three
  // depth_24bit=false programs re-resolve to cache hits on their
  // existing GLSL hashes (instant), and the three depth_24bit=true
  // programs pick up the new SMOOTH_CHROMA value.
  if (!m_shadergen)
    return false;
  const bool use_binding_layout = m_use_binding_layout;
  for (uint8_t depth_24bit = 0; depth_24bit < 2; depth_24bit++)
  {
    for (uint8_t interlaced = 0; interlaced < 3; interlaced++)
    {
      const std::string vs = m_shadergen->GenerateScreenQuadVertexShader();
      const std::string fs = m_shadergen->GenerateDisplayFragmentShader(
        static_cast<bool>(depth_24bit), static_cast<InterlacedRenderMode>(interlaced), m_chroma_smoothing);

      std::optional<GL::Program> prog =
        m_shader_cache.GetProgram(vs, {}, fs, [this, use_binding_layout](GL::Program& p) {
          if (!IsGLES() && !use_binding_layout)
            p.BindFragData(0, "o_col0");
        });
      if (!prog)
        return false;

      if (!use_binding_layout)
      {
        prog->BindUniformBlock("UBOBlock", 1);
        prog->Bind();
        prog->Uniform1i("samp0", 0);
      }
      m_display_programs[depth_24bit][interlaced] = std::move(*prog);
    }
  }
  return true;
}

bool GPU_HW_OpenGL::PrecompileBatchPrograms(ShaderCompileProgressTracker& progress)
{
  // Walk the current m_texture_filtering sub-cube of m_render_programs
  // synchronously in Enabled mode; do nothing in Lazy / Disabled
  // (OpenGL has no background-compile worker - the libretro
  // hardware-renderer protocol gives one GL context bound to the
  // runloop, so 'Lazy' degrades to the same shape as 'Disabled':
  // fault each combo in on the runloop the first time the game
  // dispatches a draw using it).
  //
  // Extracted from CompilePrograms so the only_dim_changed fast
  // path in UpdateSettings can call just this helper without
  // paying the ~10-100ms of cache-hit-but-still-wasted GL::Program
  // churn that CompilePrograms' non-batch program rebuild block
  // incurs on every filter flip. None of the non-batch programs
  // (display / vram_fill / vram_read / vram_write / vram_copy /
  // vram_update_depth / downsample) depend on filter, so the
  // existing linked program objects in those slots stay valid
  // across a filter toggle - replacing them via the disk-backed
  // program cache (which hits as glProgramBinary reloads on the
  // same GLSL hashes) is pure overhead. See gpu_hw_shadergen.cpp -
  // GenerateBatchFragmentShader and WriteBatchTextureFilter
  // (lines ~706, ~728, ~839) are the only callers that read
  // m_texture_filter from shadergen state.
  //
  // Structurally unreachable cells (reserved texture modes, two-
  // pass fallback modes for untextured polys, single-pass dual-
  // source on hardware that lacks it) are skipped via
  // IsBatchShaderReachable. Progress is ticked once per reachable
  // cell so the bar lands at batch_progress_units =
  // CountReachableBatchShaders(dual_source).
  const GPUShaderPrecompileMode precompile_mode = g_settings.gpu_shader_precompile_mode;
  if (precompile_mode != GPUShaderPrecompileMode::Enabled)
    return true;

  const bool dual_source = m_supports_dual_source_blend;
  const GPUTextureFilter cur_filter = m_texture_filtering;
  for (uint8_t render_mode = 0; render_mode < 4; render_mode++)
  {
    for (uint8_t texture_mode = 0; texture_mode < 9; texture_mode++)
    {
      if (!IsBatchShaderReachable(static_cast<BatchRenderMode>(render_mode), texture_mode, dual_source))
        continue;

      for (uint8_t dithering = 0; dithering < 2; dithering++)
      {
        for (uint8_t interlacing = 0; interlacing < 2; interlacing++)
        {
          const GL::Program* prog = GetBatchProgram(cur_filter, render_mode, texture_mode, static_cast<bool>(dithering),
                                                    static_cast<bool>(interlacing));
          if (!prog)
            return false;
          progress.Increment();
        }
      }
    }
  }
  return true;
}

const GL::Program* GPU_HW_OpenGL::GetBatchProgram(GPUTextureFilter filter, uint8_t render_mode, uint8_t texture_mode, bool dithering, bool interlacing)
{
  // Reserved_*Direct16Bit dedup. The fragment shader source for
  // texture_mode 3 / 7 is byte-for-byte identical to 2 / 6 after
  // macro expansion. Unlike the other backends we can't share a
  // refcounted handle between slots (GL::Program is move-only and
  // owns its GLuint via glDeleteProgram in its destructor); the
  // best we can do is re-link the canonical source string into a
  // separate program object. The disk-backed program cache hits on
  // the second GetProgram call and reloads the linked binary
  // instead of recompiling, so the duplicate is cheap.
  const uint8_t lookup_mode = (texture_mode == static_cast<uint8_t>(GPUTextureMode::Reserved_Direct16Bit))    ? 2u :
                         (texture_mode == static_cast<uint8_t>(GPUTextureMode::Reserved_RawDirect16Bit)) ? 6u :
                                                                                                      texture_mode;
  const uint8_t filter_idx = static_cast<uint8_t>(filter);

  GL::Program& slot = m_render_programs[filter_idx][render_mode][texture_mode][static_cast<uint8_t>(dithering)][static_cast<uint8_t>(interlacing)];
  if (slot.IsValid())
    return &slot;

  // Construct a per-call shadergen bound to the requested filter.
  // m_shadergen exists for the non-batch programs below
  // CompilePrograms and is bound to the runtime-current
  // m_texture_filtering. With the dim cache the batch slot we're
  // building may be for a filter different from m_texture_filtering
  // (when CompilePrograms walks a freshly-toggled filter's sub-cube
  // before any draw has run, the runtime is still on the new
  // filter, but the contract is to use the explicit filter
  // parameter so the cache stays consistent with what would happen
  // if a non-current sub-cube were faulted in some other way -
  // and to mirror what D3D11 / D3D12 do).
  GPU_HW_ShaderGen tmp_shadergen(
    m_host_display->GetRenderAPI(), m_resolution_scale, m_multisamples, m_per_sample_shading, m_true_color,
    m_scaled_dithering, filter, m_using_uv_limits, m_pgxp_depth_buffer, m_disable_color_perspective,
    m_supports_dual_source_blend);

  const bool textured = (static_cast<GPUTextureMode>(lookup_mode) != GPUTextureMode::Disabled);
  const std::string batch_vs = tmp_shadergen.GenerateBatchVertexShader(textured);
  const std::string fs = tmp_shadergen.GenerateBatchFragmentShader(
    static_cast<BatchRenderMode>(render_mode), static_cast<GPUTextureMode>(lookup_mode), dithering, interlacing);

  const bool use_binding_layout = m_use_binding_layout;
  const auto link_callback = [this, textured, use_binding_layout](GL::Program& prog) {
    if (!use_binding_layout)
    {
      prog.BindAttribute(0, "a_pos");
      prog.BindAttribute(1, "a_col0");
      if (textured)
      {
        prog.BindAttribute(2, "a_texcoord");
        prog.BindAttribute(3, "a_texpage");
        prog.BindAttribute(4, "a_uv_limits");
      }

      if (!IsGLES() || m_supports_dual_source_blend)
      {
        if (m_supports_dual_source_blend)
        {
          prog.BindFragDataIndexed(0, "o_col0");
          prog.BindFragDataIndexed(1, "o_col1");
        }
        else
        {
          prog.BindFragData(0, "o_col0");
        }
      }
    }
  };

  std::optional<GL::Program> prog = m_shader_cache.GetProgram(batch_vs, {}, fs, link_callback);
  if (!prog)
  {
    Log_ErrorPrintf("Lazy batch program compile failed for (f=%u, rm=%u, tm=%u, d=%u, i=%u)",
                    static_cast<uint8_t>(filter), render_mode, texture_mode,
                    static_cast<uint8_t>(dithering), static_cast<uint8_t>(interlacing));
    return nullptr;
  }

  if (!use_binding_layout)
  {
    prog->BindUniformBlock("UBOBlock", 1);
    if (textured)
    {
      prog->Bind();
      prog->Uniform1i("samp0", 0);
    }
  }

  slot = std::move(*prog);
  return &slot;
}

void GPU_HW_OpenGL::DrawBatchVertices(BatchRenderMode render_mode, uint32_t base_vertex, uint32_t num_vertices)
{
  // Fetch the batch program via the lazy helper. In 'Enabled'
  // precompile mode every slot was filled at CompilePrograms time
  // so this is a fast IsValid() check + array index. In 'Lazy' /
  // 'Disabled' mode the slot is compiled on the first use of that
  // combination, then cached. Single-threaded - no mutex / no
  // atomic - because the libretro hardware-renderer protocol only
  // gives us one GL context bound to this thread.
  //
  // m_texture_filtering selects the active filter's sub-cube. Filter
  // is the outermost dim so a filter toggle in UpdateSettings can
  // skip the CompilePrograms round trip - the other filters' sub-
  // cubes remain valid and reachable, switching back to a previously-
  // visited filter is a slot validity check.
  const GL::Program* prog = GetBatchProgram(m_texture_filtering, static_cast<uint8_t>(render_mode), static_cast<uint8_t>(m_batch.texture_mode),
                                            m_batch.dithering, m_batch.interlacing);
  if (!prog)
    return;
  prog->Bind();

  if (m_current_transparency_mode != m_batch.transparency_mode || m_current_render_mode != render_mode)
  {
    m_current_transparency_mode = m_batch.transparency_mode;
    m_current_render_mode = render_mode;
    SetBlendMode();
  }

  SetDepthFunc();

  glDrawArrays(GL_TRIANGLES, m_batch_base_vertex, num_vertices);
}

void GPU_HW_OpenGL::SetBlendMode()
{
  if (UseAlphaBlending(m_current_transparency_mode, m_current_render_mode))
  {
    glEnable(GL_BLEND);
    glBlendEquationSeparate(m_current_transparency_mode == GPUTransparencyMode::BackgroundMinusForeground ?
                              GL_FUNC_REVERSE_SUBTRACT :
                              GL_FUNC_ADD,
                            GL_FUNC_ADD);
    if (m_supports_dual_source_blend)
    {
      glBlendFuncSeparate(GL_ONE, m_supports_dual_source_blend ? GL_SRC1_ALPHA : GL_SRC_ALPHA, GL_ONE, GL_ZERO);
    }
    else
    {
      const float factor =
        (m_current_transparency_mode == GPUTransparencyMode::HalfBackgroundPlusHalfForeground) ? 0.5f : 1.0f;
      glBlendFuncSeparate(GL_ONE, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
      glBlendColor(0.0f, 0.0f, 0.0f, factor);
    }
  }
  else
  {
    glDisable(GL_BLEND);
  }
}

bool GPU_HW_OpenGL::BlitVRAMReplacementTexture(const TextureReplacementTexture* tex, uint32_t dst_x, uint32_t dst_y, uint32_t width,
                                               uint32_t height)
{
  if (!m_vram_write_replacement_texture.IsValid())
  {
    if (!m_vram_write_replacement_texture.Create(tex->GetWidth(), tex->GetHeight(), 1, GL_RGBA, GL_RGBA,
                                                 GL_UNSIGNED_BYTE, tex->GetPixels(), true) ||
        !m_vram_write_replacement_texture.CreateFramebuffer())
    {
      m_vram_write_replacement_texture.Destroy();
      return false;
    }
  }
  else
  {
    m_vram_write_replacement_texture.Replace(tex->GetWidth(), tex->GetHeight(), GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE,
                                             tex->GetPixels());
  }

  glDisable(GL_SCISSOR_TEST);
  m_vram_write_replacement_texture.BindFramebuffer(GL_READ_FRAMEBUFFER);

  dst_y = m_vram_texture.GetHeight() - dst_y - height;
  glBlitFramebuffer(0, tex->GetHeight(), tex->GetWidth(), 0, dst_x, dst_y, dst_x + width, dst_y + height,
                    GL_COLOR_BUFFER_BIT, GL_LINEAR);

  m_vram_read_texture.Bind();
  glEnable(GL_SCISSOR_TEST);
  return true;
}

void GPU_HW_OpenGL::SetDepthFunc()
{
  SetDepthFunc(m_batch.use_depth_buffer ? GL_LEQUAL : (m_batch.check_mask_before_draw ? GL_GEQUAL : GL_ALWAYS));
}

void GPU_HW_OpenGL::SetDepthFunc(GLenum func)
{
  if (m_current_depth_test == func)
    return;

  glDepthFunc(func);
  m_current_depth_test = func;
}

void GPU_HW_OpenGL::SetScissorFromDrawingArea()
{
  int left, top, right, bottom;
  CalcScissorRect(&left, &top, &right, &bottom);

  const int width = right - left;
  const int height = bottom - top;
  const int x = left;
  const int y = m_vram_texture.GetHeight() - bottom;
  glScissor(x, y, width, height);
}

void GPU_HW_OpenGL::UploadUniformBuffer(const void* data, uint32_t data_size)
{
  const GL::StreamBuffer::MappingResult res = m_uniform_stream_buffer->Map(m_uniform_buffer_alignment, data_size);
  std::memcpy(res.pointer, data, data_size);
  m_uniform_stream_buffer->Unmap(data_size);

  glBindBufferRange(GL_UNIFORM_BUFFER, 1, m_uniform_stream_buffer->GetGLBufferId(), res.buffer_offset, data_size);
}

void GPU_HW_OpenGL::ClearDisplay()
{
  GPU_HW::ClearDisplay();

  m_host_display->ClearDisplayTexture();

  m_display_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_vram_fbo_id);
}

void GPU_HW_OpenGL::UpdateDisplay()
{
  GPU_HW::UpdateDisplay();

    m_host_display->SetDisplayParameters(m_crtc_state.display_width, m_crtc_state.display_height,
                                         m_crtc_state.display_origin_left, m_crtc_state.display_origin_top,
                                         m_crtc_state.display_vram_width, m_crtc_state.display_vram_height,
                                         GetDisplayAspectRatio());

    const uint32_t resolution_scale = m_GPUSTAT.display_area_color_depth_24 ? 1 : m_resolution_scale;
    const uint32_t vram_offset_x = m_crtc_state.display_vram_left;
    const uint32_t vram_offset_y = m_crtc_state.display_vram_top;
    const uint32_t scaled_vram_offset_x = vram_offset_x * resolution_scale;
    const uint32_t scaled_vram_offset_y = vram_offset_y * resolution_scale;
    const uint32_t display_width = m_crtc_state.display_vram_width;
    const uint32_t display_height = m_crtc_state.display_vram_height;
    const uint32_t scaled_display_width = display_width * resolution_scale;
    const uint32_t scaled_display_height = display_height * resolution_scale;
    const InterlacedRenderMode interlaced = GetInterlacedRenderMode();

    // Hand QRD the PGXP depth target plus the sub-rectangle actually scanned
    // out, so it can resolve display-space depth without re-deriving any of it.
    {
      qrd::PsxHwDepthInfo depth_info{};
      depth_info.depth_texture = m_vram_depth_texture.GetGLId();
      depth_info.texture_width = static_cast<int>(m_vram_depth_texture.GetWidth());
      depth_info.texture_height = static_cast<int>(m_vram_depth_texture.GetHeight());
      depth_info.display_x = static_cast<int>(scaled_vram_offset_x);
      depth_info.display_y = static_cast<int>(scaled_vram_offset_y);
      depth_info.display_width = static_cast<int>(scaled_display_width);
      depth_info.display_height = static_cast<int>(scaled_display_height);
      if (m_drawing_area.Valid())
      {
        // CalcScissorRect is what actually bounded the depth writes, including
        // the inclusive-right +1 and the resolution scale.
        int da_left, da_top, da_right, da_bottom;
        CalcScissorRect(&da_left, &da_top, &da_right, &da_bottom);
        depth_info.draw_x = da_left;
        depth_info.draw_y = da_top;
        depth_info.draw_width = da_right - da_left;
        depth_info.draw_height = da_bottom - da_top;
      }
      depth_info.pgxp_depth = m_pgxp_depth_buffer;
      depth_info.valid = !IsDisplayDisabled() && depth_info.depth_texture != 0 &&
                         scaled_display_width > 0 && scaled_display_height > 0;
      qrd::psx_hw_depth_publish(depth_info);
    }

    if (IsDisplayDisabled())
    {
      m_host_display->ClearDisplayTexture();
    }
    else if (!m_GPUSTAT.display_area_color_depth_24 && interlaced == GPU_HW::InterlacedRenderMode::None &&
             !IsUsingMultisampling() && (scaled_vram_offset_x + scaled_display_width) <= m_vram_texture.GetWidth() &&
             (scaled_vram_offset_y + scaled_display_height) <= m_vram_texture.GetHeight())
    {
      if (IsUsingDownsampling())
      {
        DownsampleFramebuffer(m_vram_texture, scaled_vram_offset_x, scaled_vram_offset_y, scaled_display_width,
                              scaled_display_height);
      }
      else
      {
        m_host_display->SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_vram_texture.GetGLId())),
                                          HostDisplayPixelFormat::RGBA8, m_vram_texture.GetWidth(),
                                          m_vram_texture.GetHeight(), scaled_vram_offset_x,
                                          m_vram_texture.GetHeight() - scaled_vram_offset_y, scaled_display_width,
                                          -static_cast<int32_t>(scaled_display_height));
      }
    }
    else
    {
      glDisable(GL_BLEND);
      glDisable(GL_SCISSOR_TEST);
      glDisable(GL_DEPTH_TEST);

      m_display_programs[static_cast<uint8_t>(m_GPUSTAT.display_area_color_depth_24)][static_cast<uint8_t>(interlaced)].Bind();
      m_display_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
      m_vram_texture.Bind();

      if (interlaced == InterlacedRenderMode::None && (GLAD_GL_VERSION_4_3 || GLAD_GL_ES_VERSION_3_0))
      {
        static constexpr std::array<GLenum, 1> attachments = {GL_COLOR_ATTACHMENT0};
        glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLsizei>(attachments.size()), attachments.data());
      }

      const uint8_t height_div2 = static_cast<uint8_t>(interlaced == GPU_HW::InterlacedRenderMode::SeparateFields);
      const uint32_t reinterpret_field_offset = (interlaced != InterlacedRenderMode::None) ? GetInterlacedDisplayField() : 0;
      const uint32_t scaled_flipped_vram_offset_y = m_vram_texture.GetHeight() - scaled_vram_offset_y -
                                               reinterpret_field_offset - (scaled_display_height >> height_div2);
      const uint32_t reinterpret_start_x = m_crtc_state.regs.X * resolution_scale;
      const uint32_t reinterpret_crop_left = (m_crtc_state.display_vram_left - m_crtc_state.regs.X) * resolution_scale;
      // 6 DWORDs to match the post-RESOLUTION_SCALE-refactor display_ps
      // cbuffer (u_vram_offset.xy, u_crop_left, u_field_offset,
      // u_resolution_scale, u_pad0). m_resolution_scale is pushed,
      // NOT the local resolution_scale (forced to 1 in 24-bit mode)
      // since the shader's RESOLUTION_SCALE has always been the
      // session m_resolution_scale. scaled_flipped_vram_offset_y
      // preserves OpenGL's lower-left origin y-flip.
      const uint32_t uniforms[6] = {reinterpret_start_x, scaled_flipped_vram_offset_y, reinterpret_crop_left,
                               reinterpret_field_offset, m_resolution_scale, 0u /* u_pad0 */};
      UploadUniformBuffer(uniforms, sizeof(uniforms));
      m_batch_ubo_dirty = true;

      glViewport(0, 0, scaled_display_width, scaled_display_height);
      glBindVertexArray(m_attributeless_vao_id);
      glDrawArrays(GL_TRIANGLES, 0, 3);

      if (IsUsingDownsampling())
      {
        DownsampleFramebuffer(m_display_texture, 0, 0, scaled_display_width, scaled_display_height);
      }
      else
      {
        m_host_display->SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_display_texture.GetGLId())),
                                          HostDisplayPixelFormat::RGBA8, m_display_texture.GetWidth(),
                                          m_display_texture.GetHeight(), 0, scaled_display_height, scaled_display_width,
                                          -static_cast<int32_t>(scaled_display_height));
      }

      // restore state
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_vram_fbo_id);
      glBindVertexArray(m_vao_id);
      glViewport(0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight());
      glEnable(GL_DEPTH_TEST);
      glEnable(GL_SCISSOR_TEST);
      m_vram_read_texture.Bind();
      SetBlendMode();
      SetDepthFunc();
    }
}

void GPU_HW_OpenGL::ReadVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
  if (IsUsingSoftwareRendererForReadbacks())
  {
    ReadSoftwareRendererVRAM(x, y, width, height);
    return;
  }

  // Get bounds with wrap-around handled.
  const Common::Rectangle<uint32_t> copy_rect = GetVRAMTransferBounds(x, y, width, height);
  const uint32_t encoded_width = (copy_rect.GetWidth() + 1) / 2;
  const uint32_t encoded_height = copy_rect.GetHeight();

  // Encode the 24-bit texture as 16-bit.
  // 6 DWORDs to match the post-RESOLUTION_SCALE-refactor vram_read_ps
  // cbuffer (u_base_coords.xy, u_size.xy, u_resolution_scale, u_pad0).
  const uint32_t uniforms[6] = {copy_rect.left, VRAM_HEIGHT - copy_rect.top - copy_rect.GetHeight(), copy_rect.GetWidth(),
                           copy_rect.GetHeight(), m_resolution_scale, 0u /* u_pad0 */};
  m_vram_encoding_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
  m_vram_texture.Bind();
  m_vram_read_program.Bind();
  UploadUniformBuffer(uniforms, sizeof(uniforms));
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glViewport(0, 0, encoded_width, encoded_height);
  glBindVertexArray(m_attributeless_vao_id);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  // Readback encoded texture.
  m_vram_encoding_texture.BindFramebuffer(GL_READ_FRAMEBUFFER);
  glPixelStorei(GL_PACK_ALIGNMENT, 2);
  glPixelStorei(GL_PACK_ROW_LENGTH, VRAM_WIDTH / 2);
  glReadPixels(0, 0, encoded_width, encoded_height, GL_RGBA, GL_UNSIGNED_BYTE,
               &m_vram_shadow[copy_rect.top * VRAM_WIDTH + copy_rect.left]);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  RestoreGraphicsAPIState();
}

void GPU_HW_OpenGL::FillVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color)
{
  if (IsUsingSoftwareRendererForReadbacks())
    FillSoftwareRendererVRAM(x, y, width, height, color);

  GPU_HW::FillVRAM(x, y, width, height, color);

  const Common::Rectangle<uint32_t> bounds(GetVRAMTransferBounds(x, y, width, height));
  glScissor(bounds.left * m_resolution_scale,
            m_vram_texture.GetHeight() - (bounds.top * m_resolution_scale) - (height * m_resolution_scale),
            width * m_resolution_scale, height * m_resolution_scale);

  // fast path when not using interlaced rendering
  const bool wrapped = IsVRAMFillOversized(x, y, width, height);
  const bool interlaced = IsInterlacedRenderingEnabled();
  if (!wrapped && !interlaced)
  {
    const auto [r, g, b, a] =
      RGBA8ToFloat(m_true_color ? color : VRAMRGBA5551ToRGBA8888(VRAMRGBA8888ToRGBA5551(color)));
    glClearColor(r, g, b, a);
    IsGLES() ? glClearDepthf(a) : glClearDepth(a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    SetScissorFromDrawingArea();
  }
  else
  {
    const VRAMFillUBOData uniforms = GetVRAMFillUBOData(x, y, width, height, color);

    m_vram_fill_programs[static_cast<uint8_t>(wrapped)][static_cast<uint8_t>(interlaced)].Bind();
    UploadUniformBuffer(&uniforms, sizeof(uniforms));
    glDisable(GL_BLEND);
    SetDepthFunc(GL_ALWAYS);
    glBindVertexArray(m_attributeless_vao_id);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    RestoreGraphicsAPIState();
  }
}

void GPU_HW_OpenGL::UpdateVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, bool set_mask, bool check_mask)
{
  if (IsUsingSoftwareRendererForReadbacks())
    UpdateSoftwareRendererVRAM(x, y, width, height, data, set_mask, check_mask);

  const Common::Rectangle<uint32_t> bounds = GetVRAMTransferBounds(x, y, width, height);
  GPU_HW::UpdateVRAM(bounds.left, bounds.top, bounds.GetWidth(), bounds.GetHeight(), data, set_mask, check_mask);

  if (!check_mask)
  {
    const TextureReplacementTexture* rtex = g_texture_replacements.GetVRAMWriteReplacement(width, height, data);
    if (rtex && BlitVRAMReplacementTexture(rtex, x * m_resolution_scale, y * m_resolution_scale,
                                           width * m_resolution_scale, height * m_resolution_scale))
    {
      return;
    }
  }

  const uint32_t num_pixels = width * height;
  if (m_use_texture_buffer_for_vram_writes || m_use_ssbo_for_vram_writes)
  {
    const auto map_result = m_texture_stream_buffer->Map(sizeof(uint16_t), num_pixels * sizeof(uint16_t));
    std::memcpy(map_result.pointer, data, num_pixels * sizeof(uint16_t));
    m_texture_stream_buffer->Unmap(num_pixels * sizeof(uint16_t));
    m_texture_stream_buffer->Unbind();

    glDisable(GL_BLEND);
    SetDepthFunc((check_mask && !m_pgxp_depth_buffer) ? GL_GEQUAL : GL_ALWAYS);

    m_vram_write_program.Bind();
    if (m_use_ssbo_for_vram_writes)
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_texture_stream_buffer->GetGLBufferId());
    else
      glBindTexture(GL_TEXTURE_BUFFER, m_texture_buffer_r16ui_texture);

    const VRAMWriteUBOData uniforms =
      GetVRAMWriteUBOData(x, y, width, height, map_result.index_aligned, set_mask, check_mask);
    UploadUniformBuffer(&uniforms, sizeof(uniforms));

    // the viewport should already be set to the full vram, so just adjust the scissor
    const Common::Rectangle<uint32_t> scaled_bounds = bounds * m_resolution_scale;
    glScissor(scaled_bounds.left, m_vram_texture.GetHeight() - scaled_bounds.top - scaled_bounds.GetHeight(),
              scaled_bounds.GetWidth(), scaled_bounds.GetHeight());

    glBindVertexArray(m_attributeless_vao_id);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    RestoreGraphicsAPIState();
  }
  else
  {
    if ((x + width) > VRAM_WIDTH || (y + height) > VRAM_HEIGHT || check_mask)
    {
      // CPU round trip if oversized for now.
      Log_WarningPrintf("Oversized/masked VRAM update (%u-%u, %u-%u), CPU round trip", x, x + width, y, y + height);
      ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
      GPU::UpdateVRAM(x, y, width, height, data, set_mask, check_mask);
      UpdateVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT, m_vram_shadow.data(), false, false);
      return;
    }

    GPU_HW::UpdateVRAM(x, y, width, height, data, set_mask, check_mask);

    const auto map_result = m_texture_stream_buffer->Map(sizeof(uint32_t), num_pixels * sizeof(uint32_t));

    // reverse copy the rows so it matches opengl's lower-left origin
    const uint32_t source_stride = width * sizeof(uint16_t);
    const uint8_t* source_ptr = static_cast<const uint8_t*>(data) + (source_stride * (height - 1));
    const uint16_t mask_or = set_mask ? 0x8000 : 0x0000;
    uint32_t* dest_ptr = static_cast<uint32_t*>(map_result.pointer);
    for (uint32_t row = 0; row < height; row++)
    {
      const uint8_t* source_row_ptr = source_ptr;

      for (uint32_t col = 0; col < width; col++)
      {
        uint16_t src_col;
        std::memcpy(&src_col, source_row_ptr, sizeof(src_col));
        source_row_ptr += sizeof(src_col);
        *(dest_ptr++) = VRAMRGBA5551ToRGBA8888(src_col | mask_or);
      }

      source_ptr -= source_stride;
    }

    m_texture_stream_buffer->Unmap(num_pixels * sizeof(uint32_t));
    m_texture_stream_buffer->Bind();

    // have to write to the 1x texture first
    if (m_resolution_scale > 1)
      m_vram_encoding_texture.Bind();
    else
      m_vram_texture.Bind();

    // lower-left origin flip happens here
    const uint32_t flipped_y = VRAM_HEIGHT - y - height;

    // update texture data
    glTexSubImage2D(m_vram_texture.GetGLTarget(), 0, x, flipped_y, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                    reinterpret_cast<void*>(static_cast<uintptr_t>(map_result.buffer_offset)));
    m_texture_stream_buffer->Unbind();

    if (m_resolution_scale > 1)
    {
      // scale to internal resolution
      const uint32_t scaled_width = width * m_resolution_scale;
      const uint32_t scaled_height = height * m_resolution_scale;
      const uint32_t scaled_x = x * m_resolution_scale;
      const uint32_t scaled_y = y * m_resolution_scale;
      const uint32_t scaled_flipped_y = m_vram_texture.GetHeight() - scaled_y - scaled_height;
      glDisable(GL_SCISSOR_TEST);
      m_vram_encoding_texture.BindFramebuffer(GL_READ_FRAMEBUFFER);
      glBlitFramebuffer(x, flipped_y, x + width, flipped_y + height, scaled_x, scaled_flipped_y,
                        scaled_x + scaled_width, scaled_flipped_y + scaled_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
      glEnable(GL_SCISSOR_TEST);
    }
  }
}

void GPU_HW_OpenGL::CopyVRAM(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height)
{
  if (IsUsingSoftwareRendererForReadbacks())
    CopySoftwareRendererVRAM(src_x, src_y, dst_x, dst_y, width, height);

  const Common::Rectangle<uint32_t> dst_bounds = GetVRAMTransferBounds(dst_x, dst_y, width, height);
  const Common::Rectangle<uint32_t> src_bounds = GetVRAMTransferBounds(src_x, src_y, width, height);
  const bool src_dirty = m_vram_dirty_rect.Intersects(src_bounds);

  if (UseVRAMCopyShader(src_x, src_y, dst_x, dst_y, width, height))
  {
    if (src_dirty)
      UpdateVRAMReadTexture();
    IncludeVRAMDirtyRectangle(dst_bounds);

    VRAMCopyUBOData uniforms = GetVRAMCopyUBOData(src_x, src_y, dst_x, dst_y, width, height);
    uniforms.u_src_y = m_vram_texture.GetHeight() - uniforms.u_src_y - uniforms.u_height;
    uniforms.u_dst_y = m_vram_texture.GetHeight() - uniforms.u_dst_y - uniforms.u_height;
    UploadUniformBuffer(&uniforms, sizeof(uniforms));

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    SetDepthFunc((m_GPUSTAT.check_mask_before_draw && !m_pgxp_depth_buffer) ? GL_GEQUAL : GL_ALWAYS);

    const Common::Rectangle<uint32_t> dst_bounds_scaled(dst_bounds * m_resolution_scale);
    glViewport(dst_bounds_scaled.left,
               m_vram_texture.GetHeight() - dst_bounds_scaled.top - dst_bounds_scaled.GetHeight(),
               dst_bounds_scaled.GetWidth(), dst_bounds_scaled.GetHeight());
    m_vram_read_texture.Bind();
    m_vram_copy_program.Bind();
    glBindVertexArray(m_attributeless_vao_id);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    RestoreGraphicsAPIState();

    if (m_GPUSTAT.check_mask_before_draw)
      m_current_depth++;

    return;
  }

  GPU_HW::CopyVRAM(src_x, src_y, dst_x, dst_y, width, height);

  src_x *= m_resolution_scale;
  src_y *= m_resolution_scale;
  dst_x *= m_resolution_scale;
  dst_y *= m_resolution_scale;
  width *= m_resolution_scale;
  height *= m_resolution_scale;

  // lower-left origin flip
  src_y = m_vram_texture.GetHeight() - src_y - height;
  dst_y = m_vram_texture.GetHeight() - dst_y - height;

  if (GLAD_GL_VERSION_4_3)
  {
    glCopyImageSubData(m_vram_texture.GetGLId(), m_vram_texture.GetGLTarget(), 0, src_x, src_y, 0,
                       m_vram_texture.GetGLId(), m_vram_texture.GetGLTarget(), 0, dst_x, dst_y, 0, width, height, 1);
  }
  else if (GLAD_GL_EXT_copy_image)
  {
    glCopyImageSubDataEXT(m_vram_texture.GetGLId(), m_vram_texture.GetGLTarget(), 0, src_x, src_y, 0,
                          m_vram_texture.GetGLId(), m_vram_texture.GetGLTarget(), 0, dst_x, dst_y, 0, width, height, 1);
  }
  else if (GLAD_GL_OES_copy_image)
  {
    glCopyImageSubDataOES(m_vram_texture.GetGLId(), m_vram_texture.GetGLTarget(), 0, src_x, src_y, 0,
                          m_vram_texture.GetGLId(), m_vram_texture.GetGLTarget(), 0, dst_x, dst_y, 0, width, height, 1);
  }
  else
  {
    // glBlitFramebufer with same source/destination should be legal, but on Mali (at least Bifrost) it breaks.
    // So, blit from the shadow texture, like in the other renderers.
    if (src_dirty)
      UpdateVRAMReadTexture();

    glDisable(GL_SCISSOR_TEST);
    m_vram_read_texture.BindFramebuffer(GL_READ_FRAMEBUFFER);
    glBlitFramebuffer(src_x, src_y, src_x + width, src_y + height, dst_x, dst_y, dst_x + width, dst_y + height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_SCISSOR_TEST);
  }

  IncludeVRAMDirtyRectangle(dst_bounds);
}

void GPU_HW_OpenGL::UpdateVRAMReadTexture()
{
  const auto scaled_rect = m_vram_dirty_rect * m_resolution_scale;
  const uint32_t width = scaled_rect.GetWidth();
  const uint32_t height = scaled_rect.GetHeight();
  const uint32_t x = scaled_rect.left;
  const uint32_t y = m_vram_texture.GetHeight() - scaled_rect.top - height;
  const bool multisampled = m_vram_texture.IsMultisampled();

  if (!multisampled && GLAD_GL_VERSION_4_3)
  {
    glCopyImageSubData(m_vram_texture.GetGLId(), m_vram_texture.GetGLTarget(), 0, x, y, 0,
                       m_vram_read_texture.GetGLId(), m_vram_texture.GetGLTarget(), 0, x, y, 0, width, height, 1);
  }
  else if (!multisampled && GLAD_GL_EXT_copy_image)
  {
    glCopyImageSubDataEXT(m_vram_texture.GetGLId(), m_vram_texture.GetGLTarget(), 0, x, y, 0,
                          m_vram_read_texture.GetGLId(), m_vram_texture.GetGLTarget(), 0, x, y, 0, width, height, 1);
  }
  else if (!multisampled && GLAD_GL_OES_copy_image)
  {
    glCopyImageSubDataOES(m_vram_texture.GetGLId(), m_vram_texture.GetGLTarget(), 0, x, y, 0,
                          m_vram_read_texture.GetGLId(), m_vram_texture.GetGLTarget(), 0, x, y, 0, width, height, 1);
  }
  else
  {
    m_vram_read_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_vram_fbo_id);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(x, y, x + width, y + height, x, y, x + width, y + height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_vram_fbo_id);
  }

  GPU_HW::UpdateVRAMReadTexture();
}

void GPU_HW_OpenGL::UpdateDepthBufferFromMaskBit()
{
  if (m_pgxp_depth_buffer)
    return;

  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glDepthFunc(GL_ALWAYS);

  m_vram_texture.Bind();
  m_vram_update_depth_program.Bind();
  glBindVertexArray(m_attributeless_vao_id);
  glDrawArrays(GL_TRIANGLES, 0, 3);

  glBindVertexArray(m_vao_id);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glEnable(GL_SCISSOR_TEST);

  m_vram_read_texture.Bind();
}

void GPU_HW_OpenGL::ClearDepthBuffer()
{
  glDisable(GL_SCISSOR_TEST);
  IsGLES() ? glClearDepthf(1.0f) : glClearDepth(1.0f);
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST);
  m_last_depth_z = 1.0f;
}

void GPU_HW_OpenGL::DownsampleFramebuffer(GL::Texture& source, uint32_t left, uint32_t top, uint32_t width, uint32_t height)
{
  DownsampleFramebufferBoxFilter(source, left, top, width, height);
}

void GPU_HW_OpenGL::DownsampleFramebufferBoxFilter(GL::Texture& source, uint32_t left, uint32_t top, uint32_t width, uint32_t height)
{
  const uint32_t ds_left = left / m_resolution_scale;
  const uint32_t ds_top = top / m_resolution_scale;
  const uint32_t ds_width = width / m_resolution_scale;
  const uint32_t ds_height = height / m_resolution_scale;

  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glViewport(ds_left, m_downsample_texture.GetHeight() - ds_top - ds_height, ds_width, ds_height);
  glBindVertexArray(m_attributeless_vao_id);
  source.Bind();
  m_downsample_texture.BindFramebuffer(GL_DRAW_FRAMEBUFFER);
  m_downsample_program.Bind();
  glDrawArrays(GL_TRIANGLES, 0, 3);

  RestoreGraphicsAPIState();

  m_host_display->SetDisplayTexture(reinterpret_cast<void*>(static_cast<uintptr_t>(m_downsample_texture.GetGLId())),
                                    HostDisplayPixelFormat::RGBA8, m_downsample_texture.GetWidth(),
                                    m_downsample_texture.GetHeight(), ds_left,
                                    m_downsample_texture.GetHeight() - ds_top, ds_width, -static_cast<int32_t>(ds_height));
}

std::unique_ptr<GPU> GPU::CreateHardwareOpenGLRenderer()
{
  return std::make_unique<GPU_HW_OpenGL>();
}
