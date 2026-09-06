#include "gpu_sw.h"
#include "common/align.h"
#include "common/make_array.h"
#include "common/platform.h"
#include "host_display.h"
#include "pgxp.h"
#include "psx_pgxp_capture.h"
#include "settings.h"
#include "system.h"
#include <algorithm>
#include <cstring>
#if defined(CPU_X64)
#include <emmintrin.h>
#elif defined(CPU_AARCH64)
#ifdef _MSC_VER
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#endif

template<typename T>
ALWAYS_INLINE static constexpr std::tuple<T, T> MinMax(T v1, T v2)
{
  if (v1 > v2)
    return std::tie(v2, v1);
  return std::tie(v1, v2);
}


GPU_SW::GPU_SW()
{
  m_vram_ptr = m_backend.GetVRAM();
}

GPU_SW::~GPU_SW()
{
  m_backend.Shutdown();
  if (m_host_display)
    m_host_display->ClearDisplayTexture();
}

bool GPU_SW::Initialize(HostDisplay* host_display)
{
  if (!GPU::Initialize(host_display) || !m_backend.Initialize(false))
    return false;

  static constexpr auto formats_for_16bit = make_array(HostDisplayPixelFormat::RGB565, HostDisplayPixelFormat::RGBA5551,
                                                       HostDisplayPixelFormat::RGBA8, HostDisplayPixelFormat::BGRA8);
  static constexpr auto formats_for_24bit =
    make_array(HostDisplayPixelFormat::RGBA8, HostDisplayPixelFormat::BGRA8, HostDisplayPixelFormat::RGB565,
               HostDisplayPixelFormat::RGBA5551);
  for (const HostDisplayPixelFormat format : formats_for_16bit)
  {
    if (m_host_display->SupportsDisplayPixelFormat(format))
    {
      m_16bit_display_format = format;
      break;
    }
  }
  for (const HostDisplayPixelFormat format : formats_for_24bit)
  {
    if (m_host_display->SupportsDisplayPixelFormat(format))
    {
      m_24bit_display_format = format;
      break;
    }
  }

  return true;
}

bool GPU_SW::DoState(StateWrapper& sw, HostDisplayTexture** host_texture, bool update_display)
{
  // ignore the host texture for software mode, since we want to save vram here
  return GPU::DoState(sw, nullptr, update_display);
}

void GPU_SW::Reset(bool clear_vram)
{
  GPU::Reset(clear_vram);

  m_backend.Reset(clear_vram);
}

void GPU_SW::UpdateSettings()
{
  GPU::UpdateSettings();
  m_backend.UpdateSettings();
}

template<HostDisplayPixelFormat out_format, typename out_type>
static void CopyOutRow16(const uint16_t* src_ptr, out_type* dst_ptr, uint32_t width);

template<HostDisplayPixelFormat out_format, typename out_type>
static out_type VRAM16ToOutput(uint16_t value);

template<>
ALWAYS_INLINE uint16_t VRAM16ToOutput<HostDisplayPixelFormat::RGBA5551, uint16_t>(uint16_t value)
{
  return (value & 0x3E0) | ((value >> 10) & 0x1F) | ((value & 0x1F) << 10);
}

template<>
ALWAYS_INLINE uint16_t VRAM16ToOutput<HostDisplayPixelFormat::RGB565, uint16_t>(uint16_t value)
{
  return ((value & 0x3E0) << 1) | ((value & 0x20) << 1) | ((value >> 10) & 0x1F) | ((value & 0x1F) << 11);
}

template<>
ALWAYS_INLINE uint32_t VRAM16ToOutput<HostDisplayPixelFormat::RGBA8, uint32_t>(uint16_t value)
{
  const uint32_t value32 = static_cast<uint32_t>(value);
  const uint32_t r = (value32 & 31u) << 3;
  const uint32_t g = ((value32 >> 5) & 31u) << 3;
  const uint32_t b = ((value32 >> 10) & 31u) << 3;
  const uint32_t a = ((value >> 15) != 0) ? 255 : 0;
  return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
}

template<>
ALWAYS_INLINE uint32_t VRAM16ToOutput<HostDisplayPixelFormat::BGRA8, uint32_t>(uint16_t value)
{
  const uint32_t value32 = static_cast<uint32_t>(value);
  const uint32_t r = (value32 & 31u) << 3;
  const uint32_t g = ((value32 >> 5) & 31u) << 3;
  const uint32_t b = ((value32 >> 10) & 31u) << 3;
  return static_cast<uint32_t>(b) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(r) << 16) | (0xFF000000u);
}

#if defined(CPU_X64) || defined(CPU_AARCH64)
static uint32_t AlignDownPow2(uint32_t value, unsigned int alignment)
{
  return value & (~(alignment - 1));
}
#endif

template<>
ALWAYS_INLINE void CopyOutRow16<HostDisplayPixelFormat::RGBA5551, uint16_t>(const uint16_t* src_ptr, uint16_t* dst_ptr, uint32_t width)
{
  uint32_t col = 0;

#if defined(CPU_X64)
  const uint32_t aligned_width = AlignDownPow2(width, 8);
  for (; col < aligned_width; col += 8)
  {
    const __m128i single_mask = _mm_set1_epi16(0x1F);
    __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_ptr));
    src_ptr += 8;
    __m128i a = _mm_and_si128(value, _mm_set1_epi16(static_cast<int16_t>(0x3E0)));
    __m128i b = _mm_and_si128(_mm_srli_epi16(value, 10), single_mask);
    __m128i c = _mm_slli_epi16(_mm_and_si128(value, single_mask), 10);
    value = _mm_or_si128(_mm_or_si128(a, b), c);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_ptr), value);
    dst_ptr += 8;
  }
#elif defined(CPU_AARCH64)
  const uint32_t aligned_width = AlignDownPow2(width, 8);
  for (; col < aligned_width; col += 8)
  {
    const uint16x8_t single_mask = vdupq_n_u16(0x1F);
    uint16x8_t value = vld1q_u16(src_ptr);
    src_ptr += 8;
    uint16x8_t a = vandq_u16(value, vdupq_n_u16(0x3E0));
    uint16x8_t b = vandq_u16(vshrq_n_u16(value, 10), single_mask);
    uint16x8_t c = vshlq_n_u16(vandq_u16(value, single_mask), 10);
    value = vorrq_u16(vorrq_u16(a, b), c);
    vst1q_u16(dst_ptr, value);
    dst_ptr += 8;
  }
#endif

  for (; col < width; col++)
    *(dst_ptr++) = VRAM16ToOutput<HostDisplayPixelFormat::RGBA5551, uint16_t>(*(src_ptr++));
}

template<>
ALWAYS_INLINE void CopyOutRow16<HostDisplayPixelFormat::RGB565, uint16_t>(const uint16_t* src_ptr, uint16_t* dst_ptr, uint32_t width)
{
  uint32_t col = 0;

#if defined(CPU_X64)
  const uint32_t aligned_width = AlignDownPow2(width, 8);
  for (; col < aligned_width; col += 8)
  {
    const __m128i single_mask = _mm_set1_epi16(0x1F);
    __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_ptr));
    src_ptr += 8;
    __m128i a = _mm_slli_epi16(_mm_and_si128(value, _mm_set1_epi16(static_cast<int16_t>(0x3E0))), 1);
    __m128i b = _mm_slli_epi16(_mm_and_si128(value, _mm_set1_epi16(static_cast<int16_t>(0x20))), 1);
    __m128i c = _mm_and_si128(_mm_srli_epi16(value, 10), single_mask);
    __m128i d = _mm_slli_epi16(_mm_and_si128(value, single_mask), 11);
    value = _mm_or_si128(_mm_or_si128(_mm_or_si128(a, b), c), d);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_ptr), value);
    dst_ptr += 8;
  }
#elif defined(CPU_AARCH64)
  const uint32_t aligned_width = AlignDownPow2(width, 8);
  const uint16x8_t single_mask = vdupq_n_u16(0x1F);
  for (; col < aligned_width; col += 8)
  {
    uint16x8_t value = vld1q_u16(src_ptr);
    src_ptr += 8;
    uint16x8_t a = vshlq_n_u16(vandq_u16(value, vdupq_n_u16(0x3E0)), 1); // (value & 0x3E0) << 1
    uint16x8_t b = vshlq_n_u16(vandq_u16(value, vdupq_n_u16(0x20)), 1);  // (value & 0x20) << 1
    uint16x8_t c = vandq_u16(vshrq_n_u16(value, 10), single_mask);       // ((value >> 10) & 0x1F)
    uint16x8_t d = vshlq_n_u16(vandq_u16(value, single_mask), 11);       // ((value & 0x1F) << 11)
    value = vorrq_u16(vorrq_u16(vorrq_u16(a, b), c), d);
    vst1q_u16(dst_ptr, value);
    dst_ptr += 8;
  }
#endif

  for (; col < width; col++)
    *(dst_ptr++) = VRAM16ToOutput<HostDisplayPixelFormat::RGB565, uint16_t>(*(src_ptr++));
}

template<>
ALWAYS_INLINE void CopyOutRow16<HostDisplayPixelFormat::RGBA8, uint32_t>(const uint16_t* src_ptr, uint32_t* dst_ptr, uint32_t width)
{
  uint32_t col = 0;

#if defined(CPU_X64)
  const uint32_t aligned_width = AlignDownPow2(width, 8);
  const __m128i single_mask = _mm_set1_epi16(0x1F);
  for (; col < aligned_width; col += 8)
  {
    __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_ptr));
    src_ptr += 8;
    /* Extract 5-bit channels, then scale to 8 bits. */
    __m128i r8 = _mm_slli_epi16(_mm_and_si128(value, single_mask), 3);
    __m128i g8 = _mm_slli_epi16(_mm_and_si128(_mm_srli_epi16(value,  5), single_mask), 3);
    __m128i b8 = _mm_slli_epi16(_mm_and_si128(_mm_srli_epi16(value, 10), single_mask), 3);
    /* Alpha: bit 15 expanded.  Arithmetic right shift by 15 fills the
     * lane with 0xFFFF when the STP bit was set and 0x0000 otherwise;
     * left-shifting by 8 leaves 0xFF (or 0x00) in the high byte where
     * the alpha channel needs to land after the unpack. */
    __m128i a_hi = _mm_slli_epi16(_mm_srai_epi16(value, 15), 8);
    /* Pack into pairs of u16 lanes that, after interleaving, form the
     * 32-bit RGBA pixel in little-endian byte order R|G|B|A. */
    __m128i rg = _mm_or_si128(r8, _mm_slli_epi16(g8, 8));
    __m128i ba = _mm_or_si128(b8, a_hi);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_ptr),     _mm_unpacklo_epi16(rg, ba));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_ptr + 4), _mm_unpackhi_epi16(rg, ba));
    dst_ptr += 8;
  }
#elif defined(CPU_AARCH64)
  const uint32_t aligned_width = AlignDownPow2(width, 8);
  const uint16x8_t single_mask = vdupq_n_u16(0x1F);
  for (; col < aligned_width; col += 8)
  {
    uint16x8_t value = vld1q_u16(src_ptr);
    src_ptr += 8;
    uint16x8_t r8 = vshlq_n_u16(vandq_u16(value, single_mask), 3);
    uint16x8_t g8 = vshlq_n_u16(vandq_u16(vshrq_n_u16(value,  5), single_mask), 3);
    uint16x8_t b8 = vshlq_n_u16(vandq_u16(vshrq_n_u16(value, 10), single_mask), 3);
    /* Arithmetic >> 15 on the signed reinterpretation gives -1 (0xFFFF)
     * or 0 per lane based on the STP bit; << 8 leaves 0xFF or 0 in the
     * high byte. */
    uint16x8_t a_hi = vshlq_n_u16(vreinterpretq_u16_s16(vshrq_n_s16(vreinterpretq_s16_u16(value), 15)), 8);
    uint16x8x2_t pair;
    pair.val[0] = vorrq_u16(r8, vshlq_n_u16(g8, 8));   // R|G in u16 lanes
    pair.val[1] = vorrq_u16(b8, a_hi);                  // B|A in u16 lanes
    /* vst2q stores .val[0][i] then .val[1][i] for each lane,
     * producing the little-endian R|G|B|A layout when read as u32. */
    vst2q_u16(reinterpret_cast<uint16_t*>(dst_ptr), pair);
    dst_ptr += 8;
  }
#endif

  for (; col < width; col++)
    *(dst_ptr++) = VRAM16ToOutput<HostDisplayPixelFormat::RGBA8, uint32_t>(*(src_ptr++));
}

template<>
ALWAYS_INLINE void CopyOutRow16<HostDisplayPixelFormat::BGRA8, uint32_t>(const uint16_t* src_ptr, uint32_t* dst_ptr, uint32_t width)
{
  uint32_t col = 0;

#if defined(CPU_X64)
  const uint32_t aligned_width = AlignDownPow2(width, 8);
  const __m128i single_mask = _mm_set1_epi16(0x1F);
  /* BGRA8 always emits 0xFF in the alpha channel regardless of the
   * source STP bit; keep that as a constant high byte. */
  const __m128i alpha_hi = _mm_set1_epi16(static_cast<int16_t>(0xFF00));
  for (; col < aligned_width; col += 8)
  {
    __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_ptr));
    src_ptr += 8;
    __m128i r8 = _mm_slli_epi16(_mm_and_si128(value, single_mask), 3);
    __m128i g8 = _mm_slli_epi16(_mm_and_si128(_mm_srli_epi16(value,  5), single_mask), 3);
    __m128i b8 = _mm_slli_epi16(_mm_and_si128(_mm_srli_epi16(value, 10), single_mask), 3);
    /* Pack pairs so the unpack interleaves them into B|G|R|A bytes. */
    __m128i bg = _mm_or_si128(b8, _mm_slli_epi16(g8, 8));
    __m128i ra = _mm_or_si128(r8, alpha_hi);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_ptr),     _mm_unpacklo_epi16(bg, ra));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_ptr + 4), _mm_unpackhi_epi16(bg, ra));
    dst_ptr += 8;
  }
#elif defined(CPU_AARCH64)
  const uint32_t aligned_width = AlignDownPow2(width, 8);
  const uint16x8_t single_mask = vdupq_n_u16(0x1F);
  const uint16x8_t alpha_hi    = vdupq_n_u16(0xFF00);
  for (; col < aligned_width; col += 8)
  {
    uint16x8_t value = vld1q_u16(src_ptr);
    src_ptr += 8;
    uint16x8_t r8 = vshlq_n_u16(vandq_u16(value, single_mask), 3);
    uint16x8_t g8 = vshlq_n_u16(vandq_u16(vshrq_n_u16(value,  5), single_mask), 3);
    uint16x8_t b8 = vshlq_n_u16(vandq_u16(vshrq_n_u16(value, 10), single_mask), 3);
    uint16x8x2_t pair;
    pair.val[0] = vorrq_u16(b8, vshlq_n_u16(g8, 8));   // B|G in u16 lanes
    pair.val[1] = vorrq_u16(r8, alpha_hi);              // R|A (A=0xFF) in u16 lanes
    vst2q_u16(reinterpret_cast<uint16_t*>(dst_ptr), pair);
    dst_ptr += 8;
  }
#endif

  for (; col < width; col++)
    *(dst_ptr++) = VRAM16ToOutput<HostDisplayPixelFormat::BGRA8, uint32_t>(*(src_ptr++));
}

template<HostDisplayPixelFormat display_format>
void GPU_SW::CopyOut15Bit(uint32_t src_x, uint32_t src_y, uint32_t width, uint32_t height, uint32_t field, bool interlaced, bool interleaved)
{
  uint8_t* dst_ptr;
  uint32_t dst_stride;

  using OutputPixelType = std::conditional_t<
    display_format == HostDisplayPixelFormat::RGBA8 || display_format == HostDisplayPixelFormat::BGRA8, uint32_t, uint16_t>;

  if (!interlaced)
  {
    if (!m_host_display->BeginSetDisplayPixels(display_format, width, height, reinterpret_cast<void**>(&dst_ptr),
                                               &dst_stride))
    {
      return;
    }
  }
  else
  {
    dst_stride = GPU_MAX_DISPLAY_WIDTH * sizeof(OutputPixelType);
    dst_ptr = m_display_texture_buffer.data() + (field != 0 ? dst_stride : 0);
  }

  const uint32_t output_stride = dst_stride;
  const uint8_t interlaced_shift = static_cast<uint8_t>(interlaced);
  const uint8_t interleaved_shift = static_cast<uint8_t>(interleaved);

  // Fast path when not wrapping around.
  if ((src_x + width) <= VRAM_WIDTH && (src_y + height) <= VRAM_HEIGHT)
  {
    const uint32_t rows = height >> interlaced_shift;
    dst_stride <<= interlaced_shift;

    const uint16_t* src_ptr = &m_vram_ptr[src_y * VRAM_WIDTH + src_x];
    const uint32_t src_step = VRAM_WIDTH << interleaved_shift;
    for (uint32_t row = 0; row < rows; row++)
    {
      CopyOutRow16<display_format>(src_ptr, reinterpret_cast<OutputPixelType*>(dst_ptr), width);
      src_ptr += src_step;
      dst_ptr += dst_stride;
    }
  }
  else
  {
    const uint32_t rows = height >> interlaced_shift;
    dst_stride <<= interlaced_shift;

    const uint32_t end_x = src_x + width;
    for (uint32_t row = 0; row < rows; row++)
    {
      const uint16_t* src_row_ptr = &m_vram_ptr[(src_y % VRAM_HEIGHT) * VRAM_WIDTH];
      OutputPixelType* dst_row_ptr = reinterpret_cast<OutputPixelType*>(dst_ptr);

      for (uint32_t col = src_x; col < end_x; col++)
        *(dst_row_ptr++) = VRAM16ToOutput<display_format, OutputPixelType>(src_row_ptr[col % VRAM_WIDTH]);

      src_y += (1 << interleaved_shift);
      dst_ptr += dst_stride;
    }
  }

  if (!interlaced)
  {
    m_host_display->EndSetDisplayPixels();
  }
  else
  {
    m_host_display->SetDisplayPixels(display_format, width, height, m_display_texture_buffer.data(), output_stride);
  }
}

void GPU_SW::CopyOut15Bit(HostDisplayPixelFormat display_format, uint32_t src_x, uint32_t src_y, uint32_t width, uint32_t height, uint32_t field,
                          bool interlaced, bool interleaved)
{
  switch (display_format)
  {
    case HostDisplayPixelFormat::RGBA5551:
      CopyOut15Bit<HostDisplayPixelFormat::RGBA5551>(src_x, src_y, width, height, field, interlaced, interleaved);
      break;
    case HostDisplayPixelFormat::RGB565:
      CopyOut15Bit<HostDisplayPixelFormat::RGB565>(src_x, src_y, width, height, field, interlaced, interleaved);
      break;
    case HostDisplayPixelFormat::RGBA8:
      CopyOut15Bit<HostDisplayPixelFormat::RGBA8>(src_x, src_y, width, height, field, interlaced, interleaved);
      break;
    case HostDisplayPixelFormat::BGRA8:
      CopyOut15Bit<HostDisplayPixelFormat::BGRA8>(src_x, src_y, width, height, field, interlaced, interleaved);
      break;
    default:
      break;
  }
}

template<HostDisplayPixelFormat display_format>
void GPU_SW::CopyOut24Bit(uint32_t src_x, uint32_t src_y, uint32_t skip_x, uint32_t width, uint32_t height, uint32_t field, bool interlaced,
                          bool interleaved)
{
  uint8_t* dst_ptr;
  uint32_t dst_stride;

  using OutputPixelType = std::conditional_t<
    display_format == HostDisplayPixelFormat::RGBA8 || display_format == HostDisplayPixelFormat::BGRA8, uint32_t, uint16_t>;

  if (!interlaced)
  {
    if (!m_host_display->BeginSetDisplayPixels(display_format, width, height, reinterpret_cast<void**>(&dst_ptr),
                                               &dst_stride))
      return;
  }
  else
  {
    dst_stride = Common::AlignUpPow2<uint32_t>(width * sizeof(OutputPixelType), 4);
    dst_ptr = m_display_texture_buffer.data() + (field != 0 ? dst_stride : 0);
  }

  const uint32_t output_stride = dst_stride;
  const uint8_t interlaced_shift = static_cast<uint8_t>(interlaced);
  const uint8_t interleaved_shift = static_cast<uint8_t>(interleaved);
  const uint32_t rows = height >> interlaced_shift;
  dst_stride <<= interlaced_shift;

  if ((src_x + width) <= VRAM_WIDTH && (src_y + (rows << interleaved_shift)) <= VRAM_HEIGHT)
  {
    const uint8_t* src_ptr = reinterpret_cast<const uint8_t*>(&m_vram_ptr[src_y * VRAM_WIDTH + src_x]) + (skip_x * 3);
    const uint32_t src_stride = (VRAM_WIDTH << interleaved_shift) * sizeof(uint16_t);
    for (uint32_t row = 0; row < rows; row++)
    {
      if constexpr (display_format == HostDisplayPixelFormat::RGBA8)
      {
        const uint8_t* src_row_ptr = src_ptr;
        uint8_t* dst_row_ptr = reinterpret_cast<uint8_t*>(dst_ptr);
        for (uint32_t col = 0; col < width; col++)
        {
          *(dst_row_ptr++) = *(src_row_ptr++);
          *(dst_row_ptr++) = *(src_row_ptr++);
          *(dst_row_ptr++) = *(src_row_ptr++);
          *(dst_row_ptr++) = 0xFF;
        }
      }
      else if constexpr (display_format == HostDisplayPixelFormat::BGRA8)
      {
        const uint8_t* src_row_ptr = src_ptr;
        uint8_t* dst_row_ptr = reinterpret_cast<uint8_t*>(dst_ptr);
        for (uint32_t col = 0; col < width; col++)
        {
          *(dst_row_ptr++) = src_row_ptr[2];
          *(dst_row_ptr++) = src_row_ptr[1];
          *(dst_row_ptr++) = src_row_ptr[0];
          *(dst_row_ptr++) = 0xFF;
          src_row_ptr += 3;
        }
      }
      else if constexpr (display_format == HostDisplayPixelFormat::RGB565)
      {
        const uint8_t* src_row_ptr = src_ptr;
        uint16_t* dst_row_ptr = reinterpret_cast<uint16_t*>(dst_ptr);
        for (uint32_t col = 0; col < width; col++)
        {
          *(dst_row_ptr++) = ((static_cast<uint16_t>(src_row_ptr[0]) >> 3) << 11) |
                             ((static_cast<uint16_t>(src_row_ptr[1]) >> 2) << 5) | (static_cast<uint16_t>(src_row_ptr[2]) >> 3);
          src_row_ptr += 3;
        }
      }
      else if constexpr (display_format == HostDisplayPixelFormat::RGBA5551)
      {
        const uint8_t* src_row_ptr = src_ptr;
        uint16_t* dst_row_ptr = reinterpret_cast<uint16_t*>(dst_ptr);
        for (uint32_t col = 0; col < width; col++)
        {
          *(dst_row_ptr++) = ((static_cast<uint16_t>(src_row_ptr[0]) >> 3) << 10) |
                             ((static_cast<uint16_t>(src_row_ptr[1]) >> 3) << 5) | (static_cast<uint16_t>(src_row_ptr[2]) >> 3);
          src_row_ptr += 3;
        }
      }

      src_ptr += src_stride;
      dst_ptr += dst_stride;
    }
  }
  else
  {
    for (uint32_t row = 0; row < rows; row++)
    {
      const uint16_t* src_row_ptr = &m_vram_ptr[(src_y % VRAM_HEIGHT) * VRAM_WIDTH];
      OutputPixelType* dst_row_ptr = reinterpret_cast<OutputPixelType*>(dst_ptr);

      for (uint32_t col = 0; col < width; col++)
      {
        const uint32_t offset = (src_x + (((skip_x + col) * 3) / 2));
        const uint16_t s0 = src_row_ptr[offset % VRAM_WIDTH];
        const uint16_t s1 = src_row_ptr[(offset + 1) % VRAM_WIDTH];
        const uint8_t shift = static_cast<uint8_t>(col & 1u) * 8;
        const uint32_t rgb = (((static_cast<uint32_t>(s1) << 16) | static_cast<uint32_t>(s0)) >> shift);

        if constexpr (display_format == HostDisplayPixelFormat::RGBA8)
        {
          *(dst_row_ptr++) = rgb | 0xFF000000u;
        }
        else if constexpr (display_format == HostDisplayPixelFormat::BGRA8)
        {
          *(dst_row_ptr++) = (rgb & 0x00FF00) | ((rgb & 0xFF) << 16) | ((rgb >> 16) & 0xFF) | 0xFF000000u;
        }
        else if constexpr (display_format == HostDisplayPixelFormat::RGB565)
        {
          *(dst_row_ptr++) = ((rgb >> 3) & 0x1F) | (((rgb >> 10) << 5) & 0x7E0) | (((rgb >> 19) << 11) & 0x3E0000);
        }
        else if constexpr (display_format == HostDisplayPixelFormat::RGBA5551)
        {
          *(dst_row_ptr++) = ((rgb >> 3) & 0x1F) | (((rgb >> 11) << 5) & 0x3E0) | (((rgb >> 19) << 10) & 0x1F0000);
        }
      }

      src_y += (1 << interleaved_shift);
      dst_ptr += dst_stride;
    }
  }

  if (!interlaced)
  {
    m_host_display->EndSetDisplayPixels();
  }
  else
  {
    m_host_display->SetDisplayPixels(display_format, width, height, m_display_texture_buffer.data(), output_stride);
  }
}

void GPU_SW::CopyOut24Bit(HostDisplayPixelFormat display_format, uint32_t src_x, uint32_t src_y, uint32_t skip_x, uint32_t width,
                          uint32_t height, uint32_t field, bool interlaced, bool interleaved)
{
  switch (display_format)
  {
    case HostDisplayPixelFormat::RGBA5551:
      CopyOut24Bit<HostDisplayPixelFormat::RGBA5551>(src_x, src_y, skip_x, width, height, field, interlaced,
                                                     interleaved);
      break;
    case HostDisplayPixelFormat::RGB565:
      CopyOut24Bit<HostDisplayPixelFormat::RGB565>(src_x, src_y, skip_x, width, height, field, interlaced, interleaved);
      break;
    case HostDisplayPixelFormat::RGBA8:
      CopyOut24Bit<HostDisplayPixelFormat::RGBA8>(src_x, src_y, skip_x, width, height, field, interlaced, interleaved);
      break;
    case HostDisplayPixelFormat::BGRA8:
      CopyOut24Bit<HostDisplayPixelFormat::BGRA8>(src_x, src_y, skip_x, width, height, field, interlaced, interleaved);
      break;
    default:
      break;
  }
}

void GPU_SW::ClearDisplay()
{
  std::memset(m_display_texture_buffer.data(), 0, m_display_texture_buffer.size());
}

void GPU_SW::UpdateDisplay()
{
  // fill display texture
  m_backend.Sync(true);

  {
    m_host_display->SetDisplayParameters(m_crtc_state.display_width, m_crtc_state.display_height,
                                         m_crtc_state.display_origin_left, m_crtc_state.display_origin_top,
                                         m_crtc_state.display_vram_width, m_crtc_state.display_vram_height,
                                         GetDisplayAspectRatio());

    if (IsDisplayDisabled())
    {
      m_host_display->ClearDisplayTexture();
      return;
    }

    const uint32_t vram_offset_y = m_crtc_state.display_vram_top;
    const uint32_t display_width = m_crtc_state.display_vram_width;
    const uint32_t display_height = m_crtc_state.display_vram_height;

    // Draw coordinates are VRAM coordinates. Without the scanout origin the
    // depth map cannot be aligned to the video frame: double-buffered games
    // alternate between VRAM y=0 and y=256 every frame.
    qrd::psx_pgxp_capture_set_display(m_crtc_state.display_vram_left, m_crtc_state.display_vram_top,
                                      static_cast<uint16_t>(display_width),
                                      static_cast<uint16_t>(display_height));

    if (IsInterlacedDisplayEnabled())
    {
      const uint32_t field = GetInterlacedDisplayField();
      if (m_GPUSTAT.display_area_color_depth_24)
      {
        CopyOut24Bit(m_24bit_display_format, m_crtc_state.regs.X, vram_offset_y + field,
                     m_crtc_state.display_vram_left - m_crtc_state.regs.X, display_width, display_height, field, true,
                     m_GPUSTAT.vertical_resolution);
      }
      else
      {
        CopyOut15Bit(m_16bit_display_format, m_crtc_state.display_vram_left, vram_offset_y + field, display_width,
                     display_height, field, true, m_GPUSTAT.vertical_resolution);
      }
    }
    else
    {
      if (m_GPUSTAT.display_area_color_depth_24)
      {
        CopyOut24Bit(m_24bit_display_format, m_crtc_state.regs.X, vram_offset_y,
                     m_crtc_state.display_vram_left - m_crtc_state.regs.X, display_width, display_height, 0, false,
                     false);
      }
      else
      {
        CopyOut15Bit(m_16bit_display_format, m_crtc_state.display_vram_left, vram_offset_y, display_width,
                     display_height, 0, false, false);
      }
    }
  }
}

void GPU_SW::FillBackendCommandParameters(GPUBackendCommand* cmd) const
{
  cmd->params.bits = 0;
  cmd->params.check_mask_before_draw = m_GPUSTAT.check_mask_before_draw;
  cmd->params.set_mask_while_drawing = m_GPUSTAT.set_mask_while_drawing;
  cmd->params.active_line_lsb = m_crtc_state.active_line_lsb;
  cmd->params.interlaced_rendering = IsInterlacedRenderingEnabled();
}

void GPU_SW::FillDrawCommand(GPUBackendDrawCommand* cmd, GPURenderCommand rc) const
{
  FillBackendCommandParameters(cmd);
  cmd->rc.bits = rc.bits;
  cmd->draw_mode.bits = m_draw_mode.mode_reg.bits;
  cmd->palette.bits = m_draw_mode.palette_reg;
  cmd->window = m_draw_mode.texture_window;
}

void GPU_SW::DispatchRenderCommand()
{
  if (m_drawing_area_changed)
  {
    GPUBackendSetDrawingAreaCommand* cmd = m_backend.NewSetDrawingAreaCommand();
    cmd->new_area = m_drawing_area;
    m_backend.PushCommand(cmd);
    m_drawing_area_changed = false;
  }

  const GPURenderCommand rc{m_render_command.bits};

  switch (rc.primitive)
  {
    case GPUPrimitive::Polygon:
    {
      const uint32_t num_vertices = rc.quad_polygon ? 4 : 3;
      GPUBackendDrawPolygonCommand* cmd = m_backend.NewDrawPolygonCommand(num_vertices);
      FillDrawCommand(cmd, rc);

      const uint32_t first_color = rc.color_for_first_vertex;
      const bool shaded = rc.shading_enable;
      const bool textured = rc.texture_enable;
      qrd::PsxPgxpCapturePrimitive capture{};
      capture.kind = rc.quad_polygon ? qrd::PsxPgxpCaptureKind::Quad : qrd::PsxPgxpCaptureKind::Triangle;
      capture.vertex_count = static_cast<uint8_t>(num_vertices);
      capture.semi_transparent = rc.transparency_enable;
      for (uint32_t i = 0; i < num_vertices; i++)
      {
        GPUBackendDrawPolygonCommand::Vertex* vert = &cmd->vertices[i];
        vert->color = (shaded && i > 0) ? (FifoPop() & UINT32_C(0x00FFFFFF)) : first_color;
        const uint64_t maddr_and_pos = m_fifo.Pop();
        const GPUVertexPosition vp{static_cast<uint32_t>(maddr_and_pos)};
        vert->x = m_drawing_offset.x + vp.x;
        vert->y = m_drawing_offset.y + vp.y;
        vert->texcoord = textured ? static_cast<uint16_t>(FifoPop()) : 0;

        auto& cv = capture.vertices[i];
        cv.x = static_cast<float>(vert->x);
        cv.y = static_cast<float>(vert->y);
        cv.w = 0.0f;
        if (qrd::psx_pgxp_capture_enabled() && g_settings.gpu_pgxp_enable)
        {
          cv.valid_w = PGXP::GetPreciseVertex(static_cast<uint32_t>(maddr_and_pos >> 32), vp.bits,
                                               vert->x, vert->y, m_drawing_offset.x, m_drawing_offset.y,
                                               &cv.x, &cv.y, &cv.w);
        }
      }

      if (!IsDrawingAreaIsValid())
        return;

      // Cull polygons which are too large.
      const auto [min_x_12, max_x_12] = MinMax(cmd->vertices[1].x, cmd->vertices[2].x);
      const auto [min_y_12, max_y_12] = MinMax(cmd->vertices[1].y, cmd->vertices[2].y);
      const int32_t min_x = std::min(min_x_12, cmd->vertices[0].x);
      const int32_t max_x = std::max(max_x_12, cmd->vertices[0].x);
      const int32_t min_y = std::min(min_y_12, cmd->vertices[0].y);
      const int32_t max_y = std::max(max_y_12, cmd->vertices[0].y);

      if ((max_x - min_x) >= MAX_PRIMITIVE_WIDTH || (max_y - min_y) >= MAX_PRIMITIVE_HEIGHT)
      {
      }
      else
      {
        AddDrawTriangleTicks(cmd->vertices[0].x, cmd->vertices[0].y, cmd->vertices[1].x, cmd->vertices[1].y,
                             cmd->vertices[2].x, cmd->vertices[2].y, rc.shading_enable, rc.texture_enable,
                             rc.transparency_enable);
      }

      // quads
      if (rc.quad_polygon)
      {
        const int32_t min_x_123 = std::min(min_x_12, cmd->vertices[3].x);
        const int32_t max_x_123 = std::max(max_x_12, cmd->vertices[3].x);
        const int32_t min_y_123 = std::min(min_y_12, cmd->vertices[3].y);
        const int32_t max_y_123 = std::max(max_y_12, cmd->vertices[3].y);

        // Cull polygons which are too large.
        if ((max_x_123 - min_x_123) >= MAX_PRIMITIVE_WIDTH || (max_y_123 - min_y_123) >= MAX_PRIMITIVE_HEIGHT)
        {
        }
        else
        {
          AddDrawTriangleTicks(cmd->vertices[2].x, cmd->vertices[2].y, cmd->vertices[1].x, cmd->vertices[1].y,
                               cmd->vertices[3].x, cmd->vertices[3].y, rc.shading_enable, rc.texture_enable,
                               rc.transparency_enable);
        }
      }

      // Emitted here, not before the validity check above: the capture must
      // describe exactly what the software renderer actually draws, otherwise
      // the depth map disagrees with the colour image it displaces.
      qrd::psx_pgxp_capture_emit(capture);

      m_backend.PushCommand(cmd);
    }
    break;

    case GPUPrimitive::Rectangle:
    {
      GPUBackendDrawRectangleCommand* cmd = m_backend.NewDrawRectangleCommand();
      FillDrawCommand(cmd, rc);
      cmd->color = rc.color_for_first_vertex;

      const GPUVertexPosition vp{FifoPop()};
      cmd->x = TruncateGPUVertexPosition(m_drawing_offset.x + vp.x);
      cmd->y = TruncateGPUVertexPosition(m_drawing_offset.y + vp.y);

      if (rc.texture_enable)
      {
        const uint32_t texcoord_and_palette = FifoPop();
        cmd->palette.bits = static_cast<uint16_t>(texcoord_and_palette >> 16);
        cmd->texcoord = static_cast<uint16_t>(texcoord_and_palette);
      }
      else
      {
        cmd->palette.bits = 0;
        cmd->texcoord = 0;
      }

      switch (rc.rectangle_size)
      {
        case GPUDrawRectangleSize::R1x1:
          cmd->width = 1;
          cmd->height = 1;
          break;
        case GPUDrawRectangleSize::R8x8:
          cmd->width = 8;
          cmd->height = 8;
          break;
        case GPUDrawRectangleSize::R16x16:
          cmd->width = 16;
          cmd->height = 16;
          break;
        default:
        {
          const uint32_t width_and_height = FifoPop();
          cmd->width = static_cast<uint16_t>(width_and_height & VRAM_WIDTH_MASK);
          cmd->height = static_cast<uint16_t>((width_and_height >> 16) & VRAM_HEIGHT_MASK);

          if (cmd->width >= MAX_PRIMITIVE_WIDTH || cmd->height >= MAX_PRIMITIVE_HEIGHT)
            return;
        }
        break;
      }

      if (!IsDrawingAreaIsValid())
        return;

      // Rectangles are 2D screen elements (HUD, text, sprites). They carry no
      // PGXP W, so they are captured with valid_w = false and the depth
      // rasteriser pins them to the screen plane instead of displacing them.
      if (qrd::psx_pgxp_capture_enabled())
      {
        qrd::PsxPgxpCapturePrimitive capture{};
        capture.kind = qrd::PsxPgxpCaptureKind::Sprite;
        capture.vertex_count = 4;
        capture.semi_transparent = rc.transparency_enable;
        const int32_t x0 = cmd->x;
        const int32_t y0 = cmd->y;
        const int32_t x1 = x0 + cmd->width;
        const int32_t y1 = y0 + cmd->height;
        capture.vertices[0] = {static_cast<float>(x0), static_cast<float>(y0), 0.0f, false};
        capture.vertices[1] = {static_cast<float>(x1), static_cast<float>(y0), 0.0f, false};
        capture.vertices[2] = {static_cast<float>(x0), static_cast<float>(y1), 0.0f, false};
        capture.vertices[3] = {static_cast<float>(x1), static_cast<float>(y1), 0.0f, false};
        qrd::psx_pgxp_capture_emit(capture);
      }

      const uint32_t clip_left = static_cast<uint32_t>(std::clamp<int32_t>(cmd->x, m_drawing_area.left, m_drawing_area.right));
      const uint32_t clip_right =
        static_cast<uint32_t>(std::clamp<int32_t>(cmd->x + cmd->width, m_drawing_area.left, m_drawing_area.right)) + 1u;
      const uint32_t clip_top = static_cast<uint32_t>(std::clamp<int32_t>(cmd->y, m_drawing_area.top, m_drawing_area.bottom));
      const uint32_t clip_bottom =
        static_cast<uint32_t>(std::clamp<int32_t>(cmd->y + cmd->height, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

      AddDrawRectangleTicks(clip_right - clip_left, clip_bottom - clip_top, rc.texture_enable, rc.transparency_enable);

      m_backend.PushCommand(cmd);
    }
    break;

    case GPUPrimitive::Line:
    {
      if (!rc.polyline)
      {
        GPUBackendDrawLineCommand* cmd = m_backend.NewDrawLineCommand(2);
        FillDrawCommand(cmd, rc);
        cmd->palette.bits = 0;

        if (rc.shading_enable)
        {
          cmd->vertices[0].color = rc.color_for_first_vertex;
          const GPUVertexPosition start_pos{FifoPop()};
          cmd->vertices[0].x = m_drawing_offset.x + start_pos.x;
          cmd->vertices[0].y = m_drawing_offset.y + start_pos.y;

          cmd->vertices[1].color = FifoPop() & UINT32_C(0x00FFFFFF);
          const GPUVertexPosition end_pos{FifoPop()};
          cmd->vertices[1].x = m_drawing_offset.x + end_pos.x;
          cmd->vertices[1].y = m_drawing_offset.y + end_pos.y;
        }
        else
        {
          cmd->vertices[0].color = rc.color_for_first_vertex;
          cmd->vertices[1].color = rc.color_for_first_vertex;

          const GPUVertexPosition start_pos{FifoPop()};
          cmd->vertices[0].x = m_drawing_offset.x + start_pos.x;
          cmd->vertices[0].y = m_drawing_offset.y + start_pos.y;

          const GPUVertexPosition end_pos{FifoPop()};
          cmd->vertices[1].x = m_drawing_offset.x + end_pos.x;
          cmd->vertices[1].y = m_drawing_offset.y + end_pos.y;
        }

        if (!IsDrawingAreaIsValid())
          return;

        const auto [min_x, max_x] = MinMax(cmd->vertices[0].x, cmd->vertices[1].x);
        const auto [min_y, max_y] = MinMax(cmd->vertices[0].y, cmd->vertices[1].y);
        if ((max_x - min_x) >= MAX_PRIMITIVE_WIDTH || (max_y - min_y) >= MAX_PRIMITIVE_HEIGHT)
          return;

        const uint32_t clip_left = static_cast<uint32_t>(std::clamp<int32_t>(min_x, m_drawing_area.left, m_drawing_area.right));
        const uint32_t clip_right = static_cast<uint32_t>(std::clamp<int32_t>(max_x, m_drawing_area.left, m_drawing_area.right)) + 1u;
        const uint32_t clip_top = static_cast<uint32_t>(std::clamp<int32_t>(min_y, m_drawing_area.top, m_drawing_area.bottom));
        const uint32_t clip_bottom =
          static_cast<uint32_t>(std::clamp<int32_t>(max_y, m_drawing_area.top, m_drawing_area.bottom)) + 1u;
        AddDrawLineTicks(clip_right - clip_left, clip_bottom - clip_top, rc.shading_enable);

        if (qrd::psx_pgxp_capture_enabled())
        {
          qrd::PsxPgxpCapturePrimitive capture{};
          capture.kind = qrd::PsxPgxpCaptureKind::Line;
          capture.vertex_count = 2;
          capture.semi_transparent = rc.transparency_enable;
          for (uint32_t i = 0; i < 2; ++i)
          {
            capture.vertices[i].x = static_cast<float>(cmd->vertices[i].x);
            capture.vertices[i].y = static_cast<float>(cmd->vertices[i].y);
          }
          qrd::psx_pgxp_capture_emit(capture);
        }

        m_backend.PushCommand(cmd);
      }
      else
      {
        const uint32_t num_vertices = GetPolyLineVertexCount();

        GPUBackendDrawLineCommand* cmd = m_backend.NewDrawLineCommand(num_vertices);
        FillDrawCommand(cmd, m_render_command);

        uint32_t buffer_pos = 0;
        const GPUVertexPosition start_vp{m_blit_buffer[buffer_pos++]};
        cmd->vertices[0].x = start_vp.x + m_drawing_offset.x;
        cmd->vertices[0].y = start_vp.y + m_drawing_offset.y;
        cmd->vertices[0].color = m_render_command.color_for_first_vertex;

        const bool shaded = m_render_command.shading_enable;
        for (uint32_t i = 1; i < num_vertices; i++)
        {
          cmd->vertices[i].color =
            shaded ? (m_blit_buffer[buffer_pos++] & UINT32_C(0x00FFFFFF)) : m_render_command.color_for_first_vertex;
          const GPUVertexPosition vp{m_blit_buffer[buffer_pos++]};
          cmd->vertices[i].x = m_drawing_offset.x + vp.x;
          cmd->vertices[i].y = m_drawing_offset.y + vp.y;

          const auto [min_x, max_x] = MinMax(cmd->vertices[i - 1].x, cmd->vertices[i].x);
          const auto [min_y, max_y] = MinMax(cmd->vertices[i - 1].y, cmd->vertices[i].y);
          if ((max_x - min_x) >= MAX_PRIMITIVE_WIDTH || (max_y - min_y) >= MAX_PRIMITIVE_HEIGHT)
          {
          }
          else
          {
            const uint32_t clip_left = static_cast<uint32_t>(std::clamp<int32_t>(min_x, m_drawing_area.left, m_drawing_area.right));
            const uint32_t clip_right =
              static_cast<uint32_t>(std::clamp<int32_t>(max_x, m_drawing_area.left, m_drawing_area.right)) + 1u;
            const uint32_t clip_top = static_cast<uint32_t>(std::clamp<int32_t>(min_y, m_drawing_area.top, m_drawing_area.bottom));
            const uint32_t clip_bottom =
              static_cast<uint32_t>(std::clamp<int32_t>(max_y, m_drawing_area.top, m_drawing_area.bottom)) + 1u;

            AddDrawLineTicks(clip_right - clip_left, clip_bottom - clip_top, m_render_command.shading_enable);
          }
        }

        m_backend.PushCommand(cmd);
      }
    }
    break;

    default:
      break;
  }
}

void GPU_SW::ReadVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
  m_backend.Sync(false);
}

void GPU_SW::FillVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color)
{
  GPUBackendFillVRAMCommand* cmd = m_backend.NewFillVRAMCommand();
  FillBackendCommandParameters(cmd);
  cmd->x = static_cast<uint16_t>(x);
  cmd->y = static_cast<uint16_t>(y);
  cmd->width = static_cast<uint16_t>(width);
  cmd->height = static_cast<uint16_t>(height);
  cmd->color = color;
  m_backend.PushCommand(cmd);
}

void GPU_SW::UpdateVRAM(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data, bool set_mask, bool check_mask)
{
  const uint32_t num_words = width * height;
  GPUBackendUpdateVRAMCommand* cmd = m_backend.NewUpdateVRAMCommand(num_words);
  FillBackendCommandParameters(cmd);
  cmd->params.set_mask_while_drawing = set_mask;
  cmd->params.check_mask_before_draw = check_mask;
  cmd->x = static_cast<uint16_t>(x);
  cmd->y = static_cast<uint16_t>(y);
  cmd->width = static_cast<uint16_t>(width);
  cmd->height = static_cast<uint16_t>(height);
  std::memcpy(cmd->data, data, sizeof(uint16_t) * num_words);
  m_backend.PushCommand(cmd);
}

void GPU_SW::CopyVRAM(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width, uint32_t height)
{
  GPUBackendCopyVRAMCommand* cmd = m_backend.NewCopyVRAMCommand();
  FillBackendCommandParameters(cmd);
  cmd->src_x = static_cast<uint16_t>(src_x);
  cmd->src_y = static_cast<uint16_t>(src_y);
  cmd->dst_x = static_cast<uint16_t>(dst_x);
  cmd->dst_y = static_cast<uint16_t>(dst_y);
  cmd->width = static_cast<uint16_t>(width);
  cmd->height = static_cast<uint16_t>(height);
  m_backend.PushCommand(cmd);
}

std::unique_ptr<GPU> GPU::CreateSoftwareRenderer()
{
  return std::make_unique<GPU_SW>();
}
