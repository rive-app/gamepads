#ifndef FLUTTER_PLUGIN_GAMEPADS_WINDOWS_RAW_GAMEPADS_H_
#define FLUTTER_PLUGIN_GAMEPADS_WINDOWS_RAW_GAMEPADS_H_

#include <atomic>
#include <functional>
#include <list>
#include <mutex>
#include <thread>

#include "gamepad.h"

// Fallback backend for controllers GameInput does not classify as a gamepad
// (PlayStation and Switch pads, generic HID pads). Reads them through
// Windows.Gaming.Input.RawGameController, which needs nothing installed, and
// emits indexed events in the SDL joystick conventions so the Dart side can
// map them with the SDL controller database:
//   "raw:b<N>"  button N, 1.0 pressed / 0.0 released
//   "raw:a<N>"  axis N, -32768..32767
//   "raw:h<N>"  hat N, bitmask of 1 = up, 2 = right, 4 = down, 8 = left
class RawGamepads {
 public:
  using Emitter = std::function<void(GamepadData* gamepad, const Event& event)>;
  // Returns true when another backend already reports the device with this
  // vendor/product id, so it must not be reported a second time from here.
  using IsClaimed = std::function<bool(int vendor_id, int product_id)>;

  void start(Emitter emitter, IsClaimed is_claimed);
  // Joins the polling thread; nothing is emitted once this returns.
  void stop();
  // Returns copies, for the same reason as Gamepads::get_gamepads().
  std::list<GamepadData> get_gamepads();

 private:
  void run();

  Emitter emitter;
  IsClaimed is_claimed;
  std::atomic<bool> stop_thread{false};
  std::thread thread;

  // Written by the polling thread, read from the Flutter platform thread.
  std::mutex gamepads_mutex;
  std::list<GamepadData> gamepads;
};

#endif  // FLUTTER_PLUGIN_GAMEPADS_WINDOWS_RAW_GAMEPADS_H_
