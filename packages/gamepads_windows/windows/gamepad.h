
#include <wtypes.h>

#include <windows.h>
#include <atomic>
#include <functional>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <GameInput.h>

struct GamepadData {
  std::string id;
  std::string name;
  int num_buttons;
  int vendor_id;
  int product_id;
};

struct Event {
  int time;
  std::string type;
  std::string key;
  double value;
};

class Gamepads {
 private:
  // One per connected device. Owns the polling thread and holds a reference on
  // the IGameInputDevice for as long as that thread can still touch it.
  struct GamepadEntry {
    GamepadData data;
    IGameInputDevice* device = nullptr;  // AddRef'd; released after the join.
    std::atomic<bool> stop_thread{false};
    std::thread thread;
  };

  // Device callbacks arrive on a GameInput worker thread while get_gamepads()
  // and stop() run on the Flutter platform thread, so all access to `gamepads`
  // and `stopping` is guarded.
  std::mutex gamepads_mutex;
  std::list<std::unique_ptr<GamepadEntry>> gamepads;
  bool stopping = false;

  GameInputCallbackToken
      deviceCallbackToken{};  // zero == "no callback registered"
  void read_gamepad(GamepadEntry* entry);
  // Signals the entry's thread, joins it, then releases the device. Must be
  // called with gamepads_mutex unlocked.
  void shutdown_entry(std::unique_ptr<GamepadEntry> entry);

  void on_gamepad_connected(IGameInputDevice* device);
  void on_gamepad_disconnected(IGameInputDevice* device);

 public:
  std::optional<std::function<void(GamepadData* gamepad, const Event& event)>>
      event_emitter;
  void init();
  void stop();
  // Returns copies: a device can disconnect (and its entry be destroyed) at any
  // point after this returns, so handing out pointers would invite a dangling
  // read on the caller's side.
  std::list<GamepadData> get_gamepads();
};

extern Gamepads gamepads;
