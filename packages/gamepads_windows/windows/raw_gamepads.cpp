#include "raw_gamepads.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Gaming.Input.h>

#include <cmath>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "windowsapp")

using winrt::Windows::Gaming::Input::GameControllerSwitchPosition;
using winrt::Windows::Gaming::Input::RawGameController;

namespace {

// How often the polling thread reads every controller.
constexpr DWORD kPollIntervalMs = 8;
// The controller list is re-read every this many polls (about once a second).
constexpr int kPollsPerRefresh = 125;

struct RawDevice {
  RawGameController controller{nullptr};
  std::wstring non_roamable_id;
  GamepadData data;
  // Previous reading, in the units that get emitted.
  std::vector<bool> buttons;
  std::vector<int> hats;
  std::vector<int> axes;
  // False until the first reading has been taken: that one only seeds the
  // previous state, so a pad resting off-centre does not emit on connect.
  bool primed = false;
  // Buffers handed to GetCurrentReading.
  std::unique_ptr<bool[]> button_buffer;
  std::vector<GameControllerSwitchPosition> switch_buffer;
  std::vector<double> axis_buffer;
};

// SDL hat bitmask: 1 = up, 2 = right, 4 = down, 8 = left.
int hat_mask(GameControllerSwitchPosition position) {
  switch (position) {
    case GameControllerSwitchPosition::Up:
      return 1;
    case GameControllerSwitchPosition::UpRight:
      return 1 | 2;
    case GameControllerSwitchPosition::Right:
      return 2;
    case GameControllerSwitchPosition::DownRight:
      return 4 | 2;
    case GameControllerSwitchPosition::Down:
      return 4;
    case GameControllerSwitchPosition::DownLeft:
      return 4 | 8;
    case GameControllerSwitchPosition::Left:
      return 8;
    case GameControllerSwitchPosition::UpLeft:
      return 1 | 8;
    default:
      return 0;
  }
}

// RawGameController reports axes in 0..1; SDL mappings expect -32768..32767.
int sdl_axis(double value) {
  return static_cast<int>(std::lround(value * 65535.0)) - 32768;
}

// Windows pads the id with a trailing NUL that is counted in its length.
std::wstring non_roamable_id(const RawGameController& controller) {
  std::wstring id(controller.NonRoamableId());
  while (!id.empty() && id.back() == L'\0') {
    id.pop_back();
  }
  return id;
}

RawDevice make_device(const RawGameController& controller) {
  RawDevice device;
  device.controller = controller;
  device.non_roamable_id = non_roamable_id(controller);
  device.data.id = winrt::to_string(device.non_roamable_id);
  device.data.name = winrt::to_string(controller.DisplayName());
  device.data.num_buttons = controller.ButtonCount();
  device.data.vendor_id = controller.HardwareVendorId();
  device.data.product_id = controller.HardwareProductId();

  const size_t button_count = static_cast<size_t>(controller.ButtonCount());
  device.buttons.resize(button_count);
  device.hats.resize(static_cast<size_t>(controller.SwitchCount()));
  device.axes.resize(static_cast<size_t>(controller.AxisCount()));
  // A zero-length array_view is fine, a zero-length new[] is not worth it.
  device.button_buffer = std::make_unique<bool[]>(button_count + 1);
  device.switch_buffer.resize(device.hats.size());
  device.axis_buffer.resize(device.axes.size());
  return device;
}

// Brings `devices` in line with the controllers Windows currently reports,
// keeping the state of the ones that are still connected.
void refresh_devices(std::vector<RawDevice>& devices) {
  std::vector<RawDevice> current;
  for (const auto& controller : RawGameController::RawGameControllers()) {
    const std::wstring id = non_roamable_id(controller);
    bool kept = false;
    for (auto& device : devices) {
      if (device.controller != nullptr && device.non_roamable_id == id) {
        current.push_back(std::move(device));
        device.controller = nullptr;
        kept = true;
        break;
      }
    }
    if (!kept) {
      current.push_back(make_device(controller));
    }
  }
  devices.swap(current);
}

}  // namespace

void RawGamepads::start(Emitter new_emitter, IsClaimed new_is_claimed) {
  this->emitter = std::move(new_emitter);
  this->is_claimed = std::move(new_is_claimed);
  this->stop_thread.store(false, std::memory_order_release);
  this->thread = std::thread([this]() { this->run(); });
}

void RawGamepads::stop() {
  this->stop_thread.store(true, std::memory_order_release);
  if (this->thread.joinable()) {
    this->thread.join();
  }
  std::lock_guard<std::mutex> lock(this->gamepads_mutex);
  this->gamepads.clear();
}

std::list<GamepadData> RawGamepads::get_gamepads() {
  std::lock_guard<std::mutex> lock(this->gamepads_mutex);
  return this->gamepads;
}

void RawGamepads::run() {
  try {
    winrt::init_apartment();
  } catch (const winrt::hresult_error&) {
    std::cerr << "Failed to initialize raw gamepad support" << std::endl;
    return;
  }

  {
    // Declared inside the apartment's lifetime so every WinRT object is
    // released before uninit_apartment.
    std::vector<RawDevice> devices;
    winrt::event_token added_token{};
    winrt::event_token removed_token{};
    try {
      // The handlers do nothing: changes are picked up by polling below.
      // Subscribing is a precaution against the list staying empty for a
      // process that never listens for arrivals.
      added_token = RawGameController::RawGameControllerAdded(
          [](const auto&, const RawGameController&) {});
      removed_token = RawGameController::RawGameControllerRemoved(
          [](const auto&, const RawGameController&) {});
    } catch (const winrt::hresult_error&) {
      std::cerr << "Failed to subscribe to raw gamepad changes" << std::endl;
    }

    int polls_until_refresh = 0;
    while (!this->stop_thread.load(std::memory_order_acquire)) {
      try {
        if (polls_until_refresh-- <= 0) {
          polls_until_refresh = kPollsPerRefresh;
          refresh_devices(devices);
        }

        std::list<GamepadData> visible;
        for (auto& device : devices) {
          if (this->is_claimed(device.data.vendor_id,
                               device.data.product_id)) {
            // Reported by GameInput. Forget our state so a later hand-over
            // back to this backend starts from a fresh reading.
            device.primed = false;
            continue;
          }
          visible.push_back(device.data);

          const size_t button_count = device.buttons.size();
          device.controller.GetCurrentReading(
              winrt::array_view<bool>(device.button_buffer.get(),
                                      device.button_buffer.get() +
                                          button_count),
              device.switch_buffer, device.axis_buffer);

          const int time = static_cast<int>(std::time(nullptr));
          for (size_t i = 0; i < button_count; ++i) {
            const bool pressed = device.button_buffer[i];
            if (device.primed && pressed != device.buttons[i]) {
              this->emitter(&device.data,
                            {time, "button", "raw:b" + std::to_string(i),
                             pressed ? 1.0 : 0.0});
            }
            device.buttons[i] = pressed;
          }
          for (size_t i = 0; i < device.hats.size(); ++i) {
            const int mask = hat_mask(device.switch_buffer[i]);
            if (device.primed && mask != device.hats[i]) {
              this->emitter(&device.data,
                            {time, "analog", "raw:h" + std::to_string(i),
                             static_cast<double>(mask)});
            }
            device.hats[i] = mask;
          }
          for (size_t i = 0; i < device.axes.size(); ++i) {
            const int value = sdl_axis(device.axis_buffer[i]);
            if (device.primed && value != device.axes[i]) {
              this->emitter(&device.data,
                            {time, "analog", "raw:a" + std::to_string(i),
                             static_cast<double>(value)});
            }
            device.axes[i] = value;
          }
          device.primed = true;
        }

        {
          std::lock_guard<std::mutex> lock(this->gamepads_mutex);
          this->gamepads.swap(visible);
        }
      } catch (const winrt::hresult_error&) {
        // A controller can vanish between the list refresh and the read.
        // Drop everything and re-enumerate on the next pass.
        devices.clear();
        polls_until_refresh = 0;
      }

      Sleep(kPollIntervalMs);
    }

    try {
      if (added_token) {
        RawGameController::RawGameControllerAdded(added_token);
      }
      if (removed_token) {
        RawGameController::RawGameControllerRemoved(removed_token);
      }
    } catch (const winrt::hresult_error&) {
    }
  }

  winrt::uninit_apartment();
}
