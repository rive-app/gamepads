import 'package:gamepads/src/api/gamepad_axis.dart';
import 'package:gamepads/src/api/gamepad_button.dart';
import 'package:gamepads/src/gamepad_normalizer.dart';
import 'package:gamepads/src/mappings/controller_database.dart';
import 'package:gamepads/src/mappings/platform_mapping.dart';

/// Mapping for Windows gamepad events.
///
/// The Windows plugin has two sources of events:
///
/// **GameInput gamepads** send named strings (e.g. "a", "b",
/// "leftThumbstickX"), so a single default mapping works for every
/// controller GameInput classifies as a gamepad. No VID/PID lookup is
/// needed.
///
/// Button keys: "a", "b", "x", "y", "leftShoulder", "rightShoulder",
/// "view", "menu", "leftThumbstick", "rightThumbstick",
/// "dpadUp", "dpadDown", "dpadLeft", "dpadRight"
///
/// Axis keys and ranges:
/// - leftThumbstickX/Y, rightThumbstickX/Y: -1.0 to 1.0
/// - leftTrigger, rightTrigger: 0.0 to 1.0
///
/// **Raw controllers** (pads GameInput does not classify as a gamepad,
/// e.g. PlayStation or Switch controllers) send indexed keys prefixed
/// with [rawKeyPrefix]: "raw:b3" for button 3, "raw:a1" for axis 1 and
/// "raw:h0" for hat 0. These follow the SDL joystick conventions (axes in
/// [-32768, 32767], hats as an up/right/down/left bitmask), so the
/// mapping comes from the SDL controller database by VID/PID. Raw events
/// from a controller that is not in the database are dropped.
class WindowsMapping extends PlatformMapping {
  /// Prefix of the keys the plugin's raw controller backend emits.
  static const rawKeyPrefix = 'raw:';

  static const _mapping = _WindowsControllerMapping.defaultMapping;

  /// SDL database entry for this device, used for raw events only.
  final ControllerMapping? _rawMapping;

  WindowsMapping() : _rawMapping = null;

  WindowsMapping._forDevice(this._rawMapping);

  @override
  PlatformMapping forDevice({int? vendorId, int? productId}) {
    if (vendorId == null || productId == null) {
      return this;
    }
    return WindowsMapping._forDevice(
      ControllerDatabase.lookup(
        vendorId: vendorId,
        productId: productId,
        platform: GamepadPlatform.windows,
      ),
    );
  }

  /// Returns the index part of a raw key of the given [kind] ('b', 'a' or
  /// 'h'), or `null` if [key] is not a raw key of that kind.
  static String? _rawIndex(String key, String kind) {
    final prefix = '$rawKeyPrefix$kind';
    if (!key.startsWith(prefix)) {
      return null;
    }
    return key.substring(prefix.length);
  }

  @override
  NormalizedButton? normalizeButton(String key, double value) {
    final rawIndex = _rawIndex(key, 'b');
    final button = rawIndex != null
        ? _rawMapping?.buttons[rawIndex]
        : _mapping.buttons[key];
    if (button == null) {
      return null;
    }
    return NormalizedButton(button, value != 0 ? 1.0 : 0.0);
  }

  @override
  List<NormalizedAxis> normalizeAxis(String key, double value) {
    final rawIndex = _rawIndex(key, 'a');
    if (rawIndex != null) {
      final rawMapping = _rawMapping;
      final axisMappings = rawMapping?.axes[rawIndex];
      if (rawMapping == null || axisMappings == null) {
        return const [];
      }
      return [
        for (final axisMapping in axisMappings)
          NormalizedAxis(
            axisMapping.axis,
            rawMapping.normalizeAxisValue(axisMapping, value),
          ),
      ];
    }

    final axisInfo = _mapping.axes[key];
    if (axisInfo == null) {
      return const [];
    }

    final normalized = _normalizeAxisValue(
      value,
      axisInfo.axis,
      axisInfo.min,
      axisInfo.max,
    );
    return [NormalizedAxis(axisInfo.axis, normalized)];
  }

  @override
  List<NormalizedButton> normalizeDpadAxis(String key, double value) {
    final rawIndex = _rawIndex(key, 'h');
    if (rawIndex == null) {
      return const [];
    }
    final hatButtons = _rawMapping?.dpadHats[rawIndex];
    if (hatButtons == null) {
      return const [];
    }
    // The value is the hat's current bitmask, so every direction is
    // reported: the ones no longer held get released.
    final mask = value.toInt();
    return [
      for (final entry in hatButtons.entries)
        NormalizedButton(entry.value, mask & entry.key != 0 ? 1.0 : 0.0),
    ];
  }

  static double _normalizeAxisValue(
    double value,
    GamepadAxis axis,
    double min,
    double max,
  ) {
    final isTrigger =
        axis == GamepadAxis.leftTrigger || axis == GamepadAxis.rightTrigger;

    if (isTrigger) {
      // Normalize from [min, max] to [0.0, 1.0].
      return (value - min) / (max - min);
    }

    // Normalize from [min, max] to [-1.0, 1.0].
    return 2.0 * (value - min) / (max - min) - 1.0;
  }
}

class _WindowsAxisInfo {
  final GamepadAxis axis;
  final double min;
  final double max;

  const _WindowsAxisInfo(this.axis, this.min, this.max);
}

class _WindowsControllerMapping {
  final Map<String, GamepadButton> buttons;
  final Map<String, _WindowsAxisInfo> axes;

  const _WindowsControllerMapping({
    required this.buttons,
    required this.axes,
  });

  /// Default mapping matching the GameInput API key strings.
  static const defaultMapping = _WindowsControllerMapping(
    buttons: {
      'a': GamepadButton.a,
      'b': GamepadButton.b,
      'x': GamepadButton.x,
      'y': GamepadButton.y,
      'dpadUp': GamepadButton.dpadUp,
      'dpadRight': GamepadButton.dpadRight,
      'dpadDown': GamepadButton.dpadDown,
      'dpadLeft': GamepadButton.dpadLeft,
      'leftShoulder': GamepadButton.leftBumper,
      'rightShoulder': GamepadButton.rightBumper,
      'view': GamepadButton.back,
      'menu': GamepadButton.start,
      'leftThumbstick': GamepadButton.leftStick,
      'rightThumbstick': GamepadButton.rightStick,
    },
    axes: {
      'leftThumbstickX': _WindowsAxisInfo(
        GamepadAxis.leftStickX,
        -1.0,
        1.0,
      ),
      'leftThumbstickY': _WindowsAxisInfo(
        GamepadAxis.leftStickY,
        -1.0,
        1.0,
      ),
      'leftTrigger': _WindowsAxisInfo(
        GamepadAxis.leftTrigger,
        0.0,
        1.0,
      ),
      'rightThumbstickX': _WindowsAxisInfo(
        GamepadAxis.rightStickX,
        -1.0,
        1.0,
      ),
      'rightThumbstickY': _WindowsAxisInfo(
        GamepadAxis.rightStickY,
        -1.0,
        1.0,
      ),
      'rightTrigger': _WindowsAxisInfo(
        GamepadAxis.rightTrigger,
        0.0,
        1.0,
      ),
    },
  );
}
