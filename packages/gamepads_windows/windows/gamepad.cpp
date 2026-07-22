#include <algorithm>
#include <ppl.h>
#include <vector>
#include <concrt.h>
#include <winerror.h>

#include "gamepad.h"
#include "utils.h"
#include <GameInput.h>
#include <iomanip>
#include <sstream>
#pragma comment(lib, "GameInput.lib")

Gamepads gamepads;

// Created in init() and released in stop(), after every polling thread has been
// joined — the threads call into it without any further null check.
static IGameInput* g_gameInput = nullptr;

std::string get_button_name(uint32_t button) {
  switch (button) {
    case GameInputGamepadMenu:
      return "menu";
    case GameInputGamepadView:
      return "view";
    case GameInputGamepadA:
      return "a";
    case GameInputGamepadB:
      return "b";
    case GameInputGamepadX:
      return "x";
    case GameInputGamepadY:
      return "y";
    case GameInputGamepadDPadUp:
      return "dpadUp";
    case GameInputGamepadDPadDown:
      return "dpadDown";
    case GameInputGamepadDPadLeft:
      return "dpadLeft";
    case GameInputGamepadDPadRight:
      return "dpadRight";
    case GameInputGamepadLeftShoulder:
      return "leftShoulder";
    case GameInputGamepadRightShoulder:
      return "rightShoulder";
    case GameInputGamepadLeftThumbstick:
      return "leftThumbstick";
    case GameInputGamepadRightThumbstick:
      return "rightThumbstick";
  }
  return "button-" + std::to_string(button);
}

std::string AppLocalDeviceIdToString(const APP_LOCAL_DEVICE_ID& id) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (size_t i = 0; i < APP_LOCAL_DEVICE_ID_SIZE; ++i) {
    oss << std::setw(2) << static_cast<int>(id.value[i]);
  }
  return oss.str();
}

std::list<Event> diff_states(const GameInputGamepadState& old,
                             const GameInputGamepadState& current) {
  std::time_t now = std::time(nullptr);
  int time = static_cast<int>(now);

  std::list<Event> events;
  if (old.leftThumbstickX != current.leftThumbstickX) {
    events.push_back(
        {time, "analog", "leftThumbstickX", current.leftThumbstickX});
  }
  if (old.leftThumbstickY != current.leftThumbstickY) {
    events.push_back(
        {time, "analog", "leftThumbstickY", current.leftThumbstickY});
  }
  if (old.rightThumbstickX != current.rightThumbstickX) {
    events.push_back(
        {time, "analog", "rightThumbstickX", current.rightThumbstickX});
  }
  if (old.rightThumbstickY != current.rightThumbstickY) {
    events.push_back(
        {time, "analog", "rightThumbstickY", current.rightThumbstickY});
  }
  if (old.leftTrigger != current.leftTrigger) {
    events.push_back({time, "analog", "leftTrigger", current.leftTrigger});
  }
  if (old.rightTrigger != current.rightTrigger) {
    events.push_back({time, "analog", "rightTrigger", current.rightTrigger});
  }
  if (old.buttons != current.buttons) {
    // While GameInputDeviceInfo.controllerButtonCount often gives 14,
    // if you install GameInput v3 redistributable, the reported
    // button count drops to zero. Button input is still reported.
    for (uint32_t i = 0; i < 14; ++i) {
      bool was_pressed = old.buttons & (1 << i);
      bool is_pressed = current.buttons & (1 << i);
      if (was_pressed != is_pressed) {
        double value = is_pressed ? 1.0 : 0.0;
        auto key = get_button_name(1 << i);
        events.push_back({time, "button", key, value});
      }
    }
  }
  return events;
}

bool are_states_different(const GameInputGamepadState& a,
                          const GameInputGamepadState& b) {
  return a.leftThumbstickX != b.leftThumbstickX ||
         a.leftThumbstickY != b.leftThumbstickY ||
         a.leftTrigger != b.leftTrigger ||
         a.rightThumbstickX != b.rightThumbstickX ||
         a.rightThumbstickY != b.rightThumbstickY ||
         a.rightTrigger != b.rightTrigger || a.buttons != b.buttons;
}

void Gamepads::init() {
  {
    std::lock_guard<std::mutex> lock(this->gamepads_mutex);
    this->stopping = false;
  }

  if (FAILED(GameInputCreate(&g_gameInput)) || g_gameInput == nullptr) {
    std::cerr << "Failed to initialize GameInput" << std::endl;
    g_gameInput = nullptr;
    return;
  }

  // Register listener for gamepad events. The callback runs on a GameInput
  // worker thread, so everything it touches has to be thread safe.
  g_gameInput->RegisterDeviceCallback(
      nullptr,  // All devices
      GameInputKindGamepad, GameInputDeviceConnected, GameInputAsyncEnumeration,
      static_cast<void*>(this),
      [](_In_ GameInputCallbackToken callbackToken, _In_ void* context,
         _In_ IGameInputDevice* device, _In_ uint64_t timestamp,
         _In_ GameInputDeviceStatus currentStatus,
         _In_ GameInputDeviceStatus previousStatus) {
        auto* self = static_cast<Gamepads*>(context);
        if (currentStatus & GameInputDeviceConnected) {
          self->on_gamepad_connected(device);
        } else {
          self->on_gamepad_disconnected(device);
        }
      },
      &this->deviceCallbackToken);
}

void Gamepads::stop() {
  // Unregister first, so no new polling thread can be spawned against a device
  // we are about to release. The timeout makes this wait for any callback that
  // is already in flight.
  if (g_gameInput != nullptr && this->deviceCallbackToken != 0) {
    g_gameInput->UnregisterCallback(this->deviceCallbackToken, 5000);
    this->deviceCallbackToken = 0;
  }

  std::list<std::unique_ptr<GamepadEntry>> entries;
  {
    std::lock_guard<std::mutex> lock(this->gamepads_mutex);
    // Closes the race against a connect callback that already passed the
    // unregister but has not taken the lock yet.
    this->stopping = true;
    entries.swap(this->gamepads);
  }

  // Every polling thread has to be joined before g_gameInput is released: each
  // one calls GetCurrentReading on it, and on its device, every 8ms.
  for (auto& entry : entries) {
    shutdown_entry(std::move(entry));
  }
  entries.clear();

  if (g_gameInput != nullptr) {
    g_gameInput->Release();
    g_gameInput = nullptr;
  }

  // The emitter closes over the plugin instance that is being torn down. Safe
  // to drop now that no thread can call it.
  this->event_emitter.reset();
}

void Gamepads::shutdown_entry(std::unique_ptr<GamepadEntry> entry) {
  if (entry == nullptr) {
    return;
  }
  entry->stop_thread.store(true, std::memory_order_release);
  if (entry->thread.joinable()) {
    entry->thread.join();
  }
  if (entry->device != nullptr) {
    // Balances the AddRef in on_gamepad_connected.
    entry->device->Release();
    entry->device = nullptr;
  }
  std::cout << "Gamepad thread stopped: " << entry->data.id << std::endl;
}

std::list<GamepadData> Gamepads::get_gamepads() {
  std::lock_guard<std::mutex> lock(this->gamepads_mutex);
  std::list<GamepadData> result;
  for (const auto& entry : this->gamepads) {
    result.push_back(entry->data);
  }
  return result;
}

void Gamepads::on_gamepad_connected(IGameInputDevice* device) {
  auto info = device->GetDeviceInfo();
  if (info == nullptr) {
    std::cerr << "Gamepad connected but failed to read info" << std::endl;
    return;
  }
  auto entry = std::make_unique<GamepadEntry>();
  entry->data.id = AppLocalDeviceIdToString(info->deviceId);
  entry->data.name =
      info->displayName != nullptr && info->displayName->data != nullptr
          ? info->displayName->data
          : "";
  entry->data.num_buttons = info->controllerButtonCount;
  entry->data.vendor_id = static_cast<int>(info->vendorId);
  entry->data.product_id = static_cast<int>(info->productId);

  // GameInput owns the device handed to this callback and can destroy it as
  // soon as the pad disconnects. Take our own reference so the polling thread
  // cannot outlive it; without this, GetCurrentReading reads freed memory.
  device->AddRef();
  entry->device = device;

  std::cout << "Gamepad connected: " << entry->data.id << " : "
            << entry->data.name << std::endl;

  std::lock_guard<std::mutex> lock(this->gamepads_mutex);
  if (this->stopping) {
    // Teardown already collected the entries it will join; starting a thread
    // now would leave it running past the release of g_gameInput.
    device->Release();
    return;
  }
  // Published before the thread starts, so stop() is guaranteed to see and join
  // it. The address is stable across the move into the list.
  auto* raw = entry.get();
  this->gamepads.push_back(std::move(entry));
  raw->thread = std::thread([this, raw]() { this->read_gamepad(raw); });
}

void Gamepads::on_gamepad_disconnected(IGameInputDevice* device) {
  auto info = device->GetDeviceInfo();
  if (info == nullptr) {
    std::cerr << "Gamepad disconnected but failed to read info" << std::endl;
    return;
  }
  std::string removeId = AppLocalDeviceIdToString(info->deviceId);
  std::cout << "Gamepad disconnected: " << removeId << std::endl;
  std::unique_ptr<GamepadEntry> removed;
  {
    std::lock_guard<std::mutex> lock(this->gamepads_mutex);
    for (auto it = this->gamepads.begin(); it != this->gamepads.end(); ++it) {
      if ((*it)->data.id == removeId) {
        removed = std::move(*it);
        this->gamepads.erase(it);
        break;
      }
    }
  }
  // Joined outside the lock: the thread can be mid-emit, and a listGamepads
  // call must not block behind it.
  shutdown_entry(std::move(removed));
}

void Gamepads::read_gamepad(GamepadEntry* entry) {
  GameInputGamepadState previous_state = {
      GameInputGamepadNone, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  while (!entry->stop_thread.load(std::memory_order_acquire)) {
    IGameInputReading* reading = nullptr;
    GameInputGamepadState state;
    // A failing call can leave `reading` untouched, so it starts null and the
    // result is checked before anything is dereferenced.
    if (SUCCEEDED(g_gameInput->GetCurrentReading(GameInputKindGamepad,
                                                 entry->device, &reading)) &&
        reading != nullptr) {
      if (reading->GetGamepadState(&state)) {
        if (are_states_different(previous_state, state)) {
          auto events = diff_states(previous_state, state);
          for (const auto& event : events) {
            if (event_emitter.has_value()) {
              (*event_emitter)(&entry->data, event);
            }
          }
        }
        previous_state = state;
      }
      // Released on every successful read, not just the ones that decoded into
      // a gamepad state.
      reading->Release();
    }

    Sleep(8);
  }
}
