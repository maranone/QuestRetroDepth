# Standard Bluetooth Gamepad Support Plan

## Goal

Add gameplay-only support for paired Bluetooth gamepads on Quest, while keeping Quest Touch as the only input for VR menus, laser UI, and setup.

This work should:
- support standard Android gamepads with an Xbox-style position-first layout
- leave the existing Touch controller mapping UI unchanged
- avoid changes to saved controller-map settings
- add a hard safety gate so nothing happens unless a valid gamepad is detected
- reuse the current gameplay rumble pipeline for supported controllers

This work should not:
- add Bluetooth controllers to the mapping panel
- make the gamepad drive VR menus or laser UI
- add multiplayer controller assignment
- add per-device remap data

## Existing Code Seams

### Android activity
- VR activity: `app/src/main/java/com/retrodepth/questretrodepth/QuestVrActivity.kt`
- Current lifecycle points already available:
  - `onResume()` at line ~115
  - `onPause()` at line ~349
  - `onDestroy()` at line ~354
- Native bridge already exists in this activity for VR lifecycle and ROM loading.

### Native shell
- VR shell class: `app/src/main/cpp/openxr_shell.h`
- VR shell implementation: `app/src/main/cpp/openxr_shell.cpp`
- Existing gameplay input is assembled in `OpenXrShell::poll_actions()`
- Existing Quest Touch gameplay input is written into `m_input_state`

### JNI bridge
- Native JNI entry points: `app/src/main/cpp/questretrodepth_main.cpp`
- `nativeStartVr()` already captures the `Activity` global ref, which can be reused for gamepad rumble callbacks.

### Existing rumble path
- Experimental gameplay rumble is produced in the emu thread in `questretrodepth_main.cpp`
- Rumble events are forwarded with `g_openxr_shell.enqueue_haptic(event)`
- Rumble events already use normalized amplitude `0..1` and `duration_ms`

### Manifest
- `android.permission.VIBRATE` already exists in `app/src/main/AndroidManifest.xml`

## Files To Change

### 1. `app/src/main/java/com/retrodepth/questretrodepth/QuestVrActivity.kt`

Add Android gamepad detection, event handling, native state forwarding, and native-invoked rumble playback.

#### Add imports
- `android.hardware.input.InputManager`
- `android.os.VibrationEffect`
- `android.view.InputDevice`
- `android.view.KeyEvent`
- `android.view.MotionEvent`

#### Add Kotlin state holder
Create a private data holder for digital gameplay state only:

```kotlin
private data class StandardGamepadSnapshot(
    val south: Boolean = false,
    val east: Boolean = false,
    val west: Boolean = false,
    val north: Boolean = false,
    val l1: Boolean = false,
    val r1: Boolean = false,
    val start: Boolean = false,
    val select: Boolean = false,
    val up: Boolean = false,
    val down: Boolean = false,
    val left: Boolean = false,
    val right: Boolean = false,
)
```

#### Add fields
- `private lateinit var inputManager: InputManager`
- `private val eligibleGamepadDeviceIds = mutableSetOf<Int>()`
- `private var activeGamepadDeviceId: Int? = null`
- `private var gamepadSnapshot = StandardGamepadSnapshot()`

#### Add helpers
- `private fun isEligibleGamepadDevice(device: InputDevice?): Boolean`
  - return `false` for null
  - require at least one of:
    - `SOURCE_GAMEPAD`
    - `SOURCE_DPAD`
    - `SOURCE_JOYSTICK`
- `private fun refreshEligibleGamepads()`
  - rebuild the set from `inputManager.inputDeviceIds`
  - if the active device is no longer present, call `clearActiveGamepad()`
- `private fun clearActiveGamepad()`
  - set `activeGamepadDeviceId = null`
  - reset `gamepadSnapshot`
  - call `nativeClearStandardGamepadState()`
- `private fun latchActiveGamepadIfNeeded(deviceId: Int): Boolean`
  - if device is not eligible, return `false`
  - if `activeGamepadDeviceId == null`, set it
  - return whether the provided device is the active one
- `private fun pushGamepadStateToNative()`
  - call `nativeSetStandardGamepadState(...)` with `connected = activeGamepadDeviceId != null`
- `private fun readAxisWithDeadzone(event: MotionEvent, axis: Int): Float`
  - read axis value
  - if `abs(value) < 0.5f`, return `0f`
  - otherwise return the raw axis value

#### Register device listener
In `onResume()`:
- initialize `inputManager = getSystemService(InputManager::class.java)`
- register an `InputManager.InputDeviceListener`
- call `refreshEligibleGamepads()`
- call `nativeClearStandardGamepadState()` before VR continues

In `onPause()`:
- unregister listener
- call `clearActiveGamepad()`

In `onDestroy()`:
- call `clearActiveGamepad()` before `nativeStopVr()`

#### Override `dispatchKeyEvent`
Handle only standard gameplay keys from eligible active devices.

Accepted keys:
- `KEYCODE_BUTTON_A`
- `KEYCODE_BUTTON_B`
- `KEYCODE_BUTTON_X`
- `KEYCODE_BUTTON_Y`
- `KEYCODE_BUTTON_L1`
- `KEYCODE_BUTTON_R1`
- `KEYCODE_BUTTON_START`
- `KEYCODE_BUTTON_SELECT`
- `KEYCODE_BACK`
- `KEYCODE_DPAD_UP`
- `KEYCODE_DPAD_DOWN`
- `KEYCODE_DPAD_LEFT`
- `KEYCODE_DPAD_RIGHT`

Behavior:
- ignore events whose source/device are not eligible
- allow first valid event to latch the active controller
- ignore events from non-active controllers once a controller is latched
- for down/up, update the relevant field in `gamepadSnapshot`
- map `KEYCODE_BACK` to `select = true/false`
- after any handled change, call `pushGamepadStateToNative()`
- return `true` only when handling gamepad events
- otherwise call `super.dispatchKeyEvent(event)`

#### Override `dispatchGenericMotionEvent`
Use this only for active eligible devices.

Read:
- `AXIS_X`
- `AXIS_Y`
- `AXIS_HAT_X`
- `AXIS_HAT_Y`

Convert to digital:
- left stick:
  - `x <= -0.5f` -> left
  - `x >= 0.5f` -> right
  - `y <= -0.5f` -> up
  - `y >= 0.5f` -> down
- hat:
  - same directional conversion

Direction merge:
- `up/down/left/right` should be the OR of hat and left-stick directions

After update:
- call `pushGamepadStateToNative()`
- return `true` only when consuming an eligible active-controller motion event

#### Add native methods
Add these externals:

```kotlin
private external fun nativeSetStandardGamepadState(
    connected: Boolean,
    south: Boolean,
    east: Boolean,
    west: Boolean,
    north: Boolean,
    l1: Boolean,
    r1: Boolean,
    start: Boolean,
    select: Boolean,
    up: Boolean,
    down: Boolean,
    left: Boolean,
    right: Boolean
)

private external fun nativeClearStandardGamepadState()
```

#### Add native-callable rumble method
Add:

```kotlin
fun applyActiveGamepadRumble(amplitude01: Float, durationMs: Int)
```

Behavior:
- if no active controller is latched, return
- resolve the active `InputDevice`
- if no longer eligible, call `clearActiveGamepad()` and return
- clamp amplitude into `0f..1f`
- if amplitude is `<= 0f` or duration is `<= 0`, return
- on API 31+, use `device.vibratorManager.defaultVibrator`
- on lower API levels, use `device.vibrator`
- if no vibrator or `hasVibrator() == false`, return
- convert `0..1` amplitude into `1..255`
- call `VibrationEffect.createOneShot(durationMs.toLong(), amplitude255)`

### 2. `app/src/main/cpp/openxr_shell.h`

Add an internal standard gamepad state separate from the Touch mapping system.

#### Add type
Inside `OpenXrShell`, add:

```cpp
struct StandardGamepadState {
    bool south = false;
    bool east = false;
    bool west = false;
    bool north = false;
    bool l1 = false;
    bool r1 = false;
    bool start = false;
    bool select = false;
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
};
```

#### Add methods
- `void set_standard_gamepad_state(const StandardGamepadState& state, bool connected);`
- `void clear_standard_gamepad_state();`
- `void set_gamepad_rumble_sink(std::function<void(float, int)> sink);`

#### Add members
- `bool m_has_standard_gamepad = false;`
- `StandardGamepadState m_standard_gamepad_state{};`
- `std::function<void(float, int)> m_gamepad_rumble_sink;`

Keep these under the same mutex/ownership model as other cross-thread state.

### 3. `app/src/main/cpp/openxr_shell.cpp`

#### Implement setters
- `set_standard_gamepad_state(...)`
  - lock `m_input_mutex`
  - store `m_has_standard_gamepad`
  - store snapshot
- `clear_standard_gamepad_state()`
  - lock `m_input_mutex`
  - zero snapshot and mark disconnected
- `set_gamepad_rumble_sink(...)`
  - lock `m_mutex`
  - store callback

#### Merge gameplay input
In `OpenXrShell::poll_actions()`:
- keep the current Touch input path unchanged
- after Touch values are computed, derive a second set of gameplay booleans from `m_standard_gamepad_state` only if `m_has_standard_gamepad`
- merge by OR into the final `m_input_state`

Do not touch panel/menu logic:
- if panels are open, keep zeroing gameplay input as today
- do not let the gamepad control the VR UI

#### Backend-specific mapping
Map from `StandardGamepadState` to `EmulatorInputState` by `m_current_backend_kind`:
- SNES mapping: use the fixed layout from `controller_mappings.md`
- Genesis mapping: use the fixed layout from `controller_mappings.md`

Do not use `m_button_map` for Bluetooth gamepads.

#### Forward rumble
In `flush_pending_haptics()`:
- keep the existing call to `fire_haptic(event.right, event.amplitude, event.duration_ms)` for Quest Touch
- also call the optional gamepad sink with:
  - `event.amplitude`
  - `event.duration_ms`

Important:
- forward only queued gameplay rumble here
- do not forward UI-click `fire_haptic(...)` calls to the Bluetooth gamepad in v1

### 4. `app/src/main/cpp/questretrodepth_main.cpp`

#### Add JNI exports
Add:
- `Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeSetStandardGamepadState`
- `Java_com_retrodepth_questretrodepth_QuestVrActivity_nativeClearStandardGamepadState`

Behavior:
- build `qrd::OpenXrShell::StandardGamepadState`
- call `g_openxr_shell.set_standard_gamepad_state(...)`
- clear via `g_openxr_shell.clear_standard_gamepad_state()`

#### Install rumble sink
Inside `nativeStartVr()` after `g_activity_global` is valid:
- call `g_openxr_shell.set_gamepad_rumble_sink(...)`
- the sink should:
  - attach to the JVM if needed
  - get the activity class from `g_activity_global`
  - find `applyActiveGamepadRumble(float, int)`
  - call it with amplitude and duration
  - clear Java exceptions if needed
  - detach the thread if it attached

#### Clear sink on stop
Inside `nativeStopVr()`:
- call `g_openxr_shell.clear_standard_gamepad_state()`
- call `g_openxr_shell.set_gamepad_rumble_sink(nullptr)`

## Important Constraints

- Leave `app/src/main/cpp/button_map.h` unchanged.
- Leave the controller mapping panel unchanged.
- Leave settings save/load unchanged.
- Leave `app/build.gradle` unchanged for v1.
- Do not add Paddleboat in this pass.
- Do not expose the Bluetooth controller to the existing remap UI.

## Safety Gate

The safety behavior is mandatory:
- if there is no eligible controller, do nothing
- if there is no latched active controller, do nothing
- ignore events from non-active controllers after latching
- on disconnect, clear all digital state immediately
- on pause/destroy, clear all digital state immediately
- if a controller has no vibration support, skip rumble silently

## Recommended Edit Order

1. Add Kotlin controller detection and event plumbing in `QuestVrActivity.kt`
2. Add native JNI setters in `questretrodepth_main.cpp`
3. Add `StandardGamepadState` to `OpenXrShell`
4. Merge gameplay input in `poll_actions()`
5. Add gamepad rumble callback path
6. Test on Quest with and without a controller

## Acceptance Criteria

- No controller paired: app behaves exactly as before
- Xbox controller paired: gameplay works without using the mapping menu
- SNES controls feel position-correct
- Genesis controls feel position-correct
- Quest Touch still owns VR UI
- Disconnecting the controller clears inputs
- Gameplay rumble reaches the controller if supported

