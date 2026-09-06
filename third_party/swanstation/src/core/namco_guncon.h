#pragma once
#include "controller.h"
#include <memory>
#include <optional>
#include <string_view>

class NamcoGunCon final : public Controller
{
public:
  enum class Button : uint8_t
  {
    Trigger = 0,
    A = 1,
    B = 2,
    ShootOffscreen = 3,
    Count
  };

  NamcoGunCon();
  ~NamcoGunCon() override;

  static std::unique_ptr<NamcoGunCon> Create();

  ControllerType GetType() const override;

  void Reset() override;
  bool DoState(StateWrapper& sw, bool apply_input_state) override;
  void LoadSettings(const char* section) override;
  bool GetSoftwareCursor(const Common::RGBA8Image** image, float* image_scale) override;

  void SetButtonState(int32_t button_code, bool pressed) override;

  void ResetTransferState() override;
  bool Transfer(const uint8_t data_in, uint8_t* data_out) override;

  void SetButtonState(Button button, bool pressed);

  // Per-controller aim, in host display pixels. Two GunCons can be plugged in
  // at once (QRD's dual-wield mode), and HostDisplay's mouse position is a
  // single global: whichever port the frontend updated last would win, so both
  // guns aimed at the same spot. Each gun now carries its own position.
  // Negative means "no aim" (offscreen), matching the old mouse convention.
  void SetHostPosition(int32_t x, int32_t y);
  ALWAYS_INLINE int32_t GetHostPositionX() const { return m_host_position_x; }
  ALWAYS_INLINE int32_t GetHostPositionY() const { return m_host_position_y; }

private:
  void UpdatePosition();

  enum class TransferState : uint8_t
  {
    Idle,
    Ready,
    IDMSB,
    ButtonsLSB,
    ButtonsMSB,
    XLSB,
    XMSB,
    YLSB,
    YMSB
  };

  Common::RGBA8Image m_crosshair_image;
  std::string m_crosshair_image_path;
  float m_crosshair_image_scale = 1.0f;
  float m_x_scale = 1.0f;
  float m_y_scale = 1.0f;

  // buttons are active low
  uint16_t m_button_state = UINT16_C(0xFFFF);
  uint16_t m_position_x = 0;
  uint16_t m_position_y = 0;
  int32_t m_host_position_x = -1;
  int32_t m_host_position_y = -1;
  bool m_shoot_offscreen = false;

  TransferState m_transfer_state = TransferState::Idle;
};
