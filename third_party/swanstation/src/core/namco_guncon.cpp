#include "namco_guncon.h"
#include "common/state_wrapper.h"
#include "gpu.h"
#include "host_display.h"
#include "host_interface.h"
#include "resources.h"
#include "system.h"
#include <array>

NamcoGunCon::NamcoGunCon() = default;

NamcoGunCon::~NamcoGunCon() = default;

ControllerType NamcoGunCon::GetType() const
{
  return ControllerType::NamcoGunCon;
}

void NamcoGunCon::Reset()
{
  m_transfer_state = TransferState::Idle;
}

bool NamcoGunCon::DoState(StateWrapper& sw, bool apply_input_state)
{
  if (!Controller::DoState(sw, apply_input_state))
    return false;

  uint16_t button_state = m_button_state;
  uint16_t position_x = m_position_x;
  uint16_t position_y = m_position_y;
  sw.Do(&button_state);
  sw.Do(&position_x);
  sw.Do(&position_y);
  if (apply_input_state)
  {
    m_button_state = button_state;
    m_position_x = position_x;
    m_position_y = position_y;
  }

  sw.Do(&m_transfer_state);
  return true;
}

void NamcoGunCon::SetButtonState(Button button, bool pressed)
{
  if (button == Button::ShootOffscreen)
  {
    if (m_shoot_offscreen != pressed)
    {
      m_shoot_offscreen = pressed;
      SetButtonState(Button::Trigger, pressed);
    }

    return;
  }

  static constexpr std::array<uint8_t, static_cast<size_t>(Button::Count)> indices = {{13, 3, 14}};
  const uint16_t bit = uint16_t(1) << indices[static_cast<uint8_t>(button)];
  const uint16_t new_state = pressed ? (m_button_state & ~bit) : (m_button_state | bit);
  if (new_state != m_button_state)
  {
    // The runahead simulation needs to re-run any frame where input changed
    // between the original poll and now; signalling here lets it detect that.
    System::SetRunaheadReplayFlag();
    m_button_state = new_state;
  }
}

void NamcoGunCon::SetButtonState(int32_t button_code, bool pressed)
{
  if (button_code < 0 || button_code >= static_cast<int32_t>(Button::Count))
    return;

  SetButtonState(static_cast<Button>(button_code), pressed);
}

void NamcoGunCon::ResetTransferState()
{
  m_transfer_state = TransferState::Idle;
}

bool NamcoGunCon::Transfer(const uint8_t data_in, uint8_t* data_out)
{
  static constexpr uint16_t ID = 0x5A63;

  switch (m_transfer_state)
  {
    case TransferState::Idle:
    {
      *data_out = 0xFF;

      if (data_in == 0x01)
      {
        m_transfer_state = TransferState::Ready;
        return true;
      }
      break;
    }

    case TransferState::Ready:
    {
      if (data_in == 0x42)
      {
        *data_out = static_cast<uint8_t>(ID);
        m_transfer_state = TransferState::IDMSB;
        return true;
      }

      *data_out = 0xFF;
      break;
    }

    case TransferState::IDMSB:
    {
      *data_out = static_cast<uint8_t>(ID >> 8);
      m_transfer_state = TransferState::ButtonsLSB;
      return true;
    }

    case TransferState::ButtonsLSB:
    {
      *data_out = static_cast<uint8_t>(m_button_state);
      m_transfer_state = TransferState::ButtonsMSB;
      return true;
    }

    case TransferState::ButtonsMSB:
    {
      *data_out = static_cast<uint8_t>(m_button_state >> 8);
      m_transfer_state = TransferState::XLSB;
      return true;
    }

    case TransferState::XLSB:
    {
      UpdatePosition();
      *data_out = static_cast<uint8_t>(m_position_x);
      m_transfer_state = TransferState::XMSB;
      return true;
    }

    case TransferState::XMSB:
    {
      *data_out = static_cast<uint8_t>(m_position_x >> 8);
      m_transfer_state = TransferState::YLSB;
      return true;
    }

    case TransferState::YLSB:
    {
      *data_out = static_cast<uint8_t>(m_position_y);
      m_transfer_state = TransferState::YMSB;
      return true;
    }

    case TransferState::YMSB:
    {
      *data_out = static_cast<uint8_t>(m_position_y >> 8);
      m_transfer_state = TransferState::Idle;
      break;
    }

    default:
      break;
  }

  return false;
}

void NamcoGunCon::SetHostPosition(int32_t x, int32_t y)
{
  if (m_host_position_x == x && m_host_position_y == y)
    return;

  // Same reasoning as the button path: the position is part of the
  // PSX-visible input fingerprint that runahead has to replay.
  System::SetRunaheadReplayFlag();
  m_host_position_x = x;
  m_host_position_y = y;
}

void NamcoGunCon::UpdatePosition()
{
  // Screen coordinates for THIS gun, set per-port by the frontend. Reading
  // HostDisplay's single global mouse position here made every connected
  // GunCon aim wherever the last-updated port pointed.
  const int32_t mouse_x = m_host_position_x;
  const int32_t mouse_y = m_host_position_y;

  // are we within the active display area?
  uint32_t tick, line;
  if (mouse_x < 0 || mouse_y < 0 ||
      !g_gpu->ConvertScreenCoordinatesToBeamTicksAndLines(mouse_x, mouse_y, m_x_scale, m_y_scale, &tick, &line) ||
      m_shoot_offscreen)
  {
    m_position_x = 0x01;
    m_position_y = 0x0A;
    return;
  }

  // 8MHz units for X = 44100*768*11/7 = 53222400 / 8000000 = 6.6528
  const double divider = static_cast<double>(g_gpu->GetCRTCFrequency()) / 8000000.0;
  // Keep the divide in double (the divider is computed in double; narrowing
  // both operands back to float threw that away) and round to nearest rather
  // than truncating. This coordinate is the emulator's own host-mouse ->
  // beam-position mapping, not a hardware register value being reproduced, so
  // rounding is strictly closer to where the player is aiming. tick is
  // non-negative and divider > 0, so + 0.5 is a correct round-half-up.
  m_position_x = static_cast<uint16_t>((static_cast<double>(tick) / divider) + 0.5);
  m_position_y = static_cast<uint16_t>(line);
}

std::unique_ptr<NamcoGunCon> NamcoGunCon::Create()
{
  return std::make_unique<NamcoGunCon>();
}

void NamcoGunCon::LoadSettings(const char* section)
{
  Controller::LoadSettings(section);

  m_crosshair_image.SetPixels(Resources::CROSSHAIR_IMAGE_WIDTH, Resources::CROSSHAIR_IMAGE_HEIGHT,
                              Resources::CROSSHAIR_IMAGE_DATA.data());

  m_crosshair_image_scale = 1.0f;

  m_x_scale = g_host_interface->GetFloatSettingValue(section, "XScale", 1.0f);

  m_y_scale = g_host_interface->GetFloatSettingValue(section, "YScale", 1.0f);
}

bool NamcoGunCon::GetSoftwareCursor(const Common::RGBA8Image** image, float* image_scale)
{
  *image = &m_crosshair_image;
  *image_scale = m_crosshair_image_scale;
  return true;
}
