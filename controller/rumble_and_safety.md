# Rumble And Safety Notes

## Safety Rules

The gamepad path must be inert unless a valid controller is present.

Required rules:
- If no eligible controller is detected, do nothing.
- If an eligible controller exists but no active controller is latched yet, do nothing.
- After latching, accept gameplay input only from that controller.
- On disconnect, clear all gamepad state immediately.
- On `onPause()` and `onDestroy()`, clear all gamepad state immediately.
- If the controller is not vibration-capable, skip rumble silently.

Eligible controller test:
- `SOURCE_GAMEPAD`
- `SOURCE_DPAD`
- `SOURCE_JOYSTICK`

Latching rule:
- First eligible device that sends valid gameplay input becomes player 1.
- Ignore other eligible devices until the active one disconnects or the app clears state.

## Rumble Behavior

Existing gameplay rumble already produces:
- `amplitude` normalized to `0..1`
- `duration_ms`

That signal should be reused for Bluetooth gamepads.

Important interpretation:
- `1.0` means maximum requested strength for that controller
- it does not mean identical physical strength across different hardware

V1 rumble policy:
- forward queued gameplay rumble only
- keep Quest Touch rumble unchanged
- do not mirror UI-click haptics to the Bluetooth controller
- clamp amplitude into `0..1`
- map to Android one-shot vibration with amplitude `1..255`

If Android exposes only a simple vibrator:
- use one combined vibration effect

If the controller exposes richer vibration internally:
- do not special-case it in v1
- keep one normalized path first

## Validation Checklist

### No Controller
- Launch app with no paired controller.
- Confirm gameplay and VR UI behave exactly as before.
- Confirm no spurious native gamepad state is set.

### Detection
- Pair a controller after launch.
- Confirm the app sees it as eligible.
- Confirm it does not affect gameplay until real input is received and the controller latches.

### Input
- Verify only the active latched controller drives gameplay.
- Verify D-pad works.
- Verify left stick works.
- Verify face buttons and shoulders match the fixed mappings.
- Verify Quest Touch still controls panels and menus.

### Disconnect
- Disconnect the active controller during gameplay.
- Confirm directions and buttons immediately clear.
- Confirm no stuck movement remains.
- Confirm rumble calls stop reaching the disconnected device.

### Rumble
- Use a title/profile that triggers gameplay rumble.
- Confirm the Bluetooth controller vibrates if it supports vibration.
- Confirm nothing breaks if the controller lacks vibration support.
- Confirm high-amplitude rumble feels stronger than low-amplitude rumble on the same controller.

## Future Upgrade Path

If later needed, the next upgrade should be one of:
- add Paddleboat for broader controller normalization and feature detection
- add per-device tuning curves for rumble strength
- add left/right motor separation when a tested API path is stable

Those are explicitly out of scope for the first implementation.
