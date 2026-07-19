#include "input/gamepad_device.h"

#include <linux/input.h>
#include <linux/joystick.h>

#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace jetson::input {
namespace {

constexpr size_t kBitsPerWord = sizeof(unsigned long) * 8;

template <size_t N>
bool HasBit(const std::array<unsigned long, N> &bits, unsigned int bit) {
    const size_t word = bit / kBitsPerWord;
    return word < bits.size() &&
           (bits[word] & (1UL << (bit % kBitsPerWord))) != 0;
}

std::string Trim(std::string value) {
    auto blank = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                             [&](char c) { return !blank(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [&](char c) { return !blank(c); }).base(),
                value.end());
    return value;
}

std::string ReadOneLine(const std::string &path) {
    std::ifstream input(path);
    std::string value;
    std::getline(input, value);
    return Trim(std::move(value));
}

std::string Basename(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::vector<std::string> GlobPaths(const char *pattern) {
    std::vector<std::string> paths;
    glob_t matches{};
    if (glob(pattern, 0, nullptr, &matches) == 0 && matches.gl_pathv) {
        paths.reserve(matches.gl_pathc);
        for (size_t i = 0; i < matches.gl_pathc; ++i) {
            if (matches.gl_pathv[i]) paths.emplace_back(matches.gl_pathv[i]);
        }
    }
    globfree(&matches);
    return paths;
}

std::string ResolvePath(const std::string &path) {
    char resolved[PATH_MAX]{};
    return realpath(path.c_str(), resolved) ? std::string(resolved) : path;
}

bool EventFdIsGamepad(int fd) {
    std::array<unsigned long, (EV_MAX / kBitsPerWord) + 1> event_bits{};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(event_bits)), event_bits.data()) < 0 ||
        !HasBit(event_bits, EV_KEY)) {
        return false;
    }

    std::array<unsigned long, (KEY_MAX / kBitsPerWord) + 1> key_bits{};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits.data()) < 0)
        return false;
    // BTN_GAMEPAD and BTN_SOUTH intentionally share the same kernel code.
    return HasBit(key_bits, BTN_GAMEPAD);
}

/* Sysfs remains readable on installations where the service user has not yet
 * been added to the input group. The kernel prints capability words from high
 * to low; each word maps to native unsigned-long width for the running ABI. */
bool SysfsCapabilityHasBit(const std::string &path, unsigned int bit) {
    const std::string text = ReadOneLine(path);
    if (text.empty()) return false;

    std::istringstream stream(text);
    std::vector<std::string> words;
    std::string word;
    while (stream >> word) words.push_back(word);
    const size_t index_from_right = bit / kBitsPerWord;
    if (index_from_right >= words.size()) return false;

    const std::string &selected = words[words.size() - 1 - index_from_right];
    char *end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(selected.c_str(), &end, 16);
    if (errno != 0 || !end || *end != '\0') return false;
    return (value & (1ULL << (bit % kBitsPerWord))) != 0;
}

std::string EventName(int fd, const std::string &path) {
    char name[256]{};
    if (fd >= 0 && ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && name[0])
        return name;
    return ReadOneLine("/sys/class/input/" + Basename(path) + "/device/name");
}

std::string JoystickName(int fd, const std::string &path) {
    char name[256]{};
    if (fd >= 0 && ioctl(fd, JSIOCGNAME(sizeof(name)), name) >= 0 && name[0])
        return name;
    return ReadOneLine("/sys/class/input/" + Basename(path) + "/device/name");
}

std::string EventUniqueId(int fd) {
    char unique[256]{};
    if (fd >= 0 && ioctl(fd, EVIOCGUNIQ(sizeof(unique)), unique) >= 0)
        return unique;
    return {};
}

std::string EventHardwareRoot(const std::string &path) {
    const std::string sysfs = "/sys/class/input/" + Basename(path) + "/device";
    char resolved[PATH_MAX]{};
    if (!realpath(sysfs.c_str(), resolved)) return {};
    std::string value = resolved;
    // eventN -> .../<hid device>/input/inputN. All gamepad/touchpad sibling
    // nodes share the prefix before this final input/inputN component.
    const size_t input = value.rfind("/input/input");
    if (input != std::string::npos) value.resize(input);
    return value;
}

bool IsTouchpadEvent(int fd, const std::string &name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.find("touchpad") != std::string::npos) return true;

    std::array<unsigned long, (INPUT_PROP_MAX / kBitsPerWord) + 1> props{};
    return fd >= 0 && ioctl(fd, EVIOCGPROP(sizeof(props)), props.data()) >= 0 &&
           HasBit(props, INPUT_PROP_BUTTONPAD);
}

float ClampUnit(float value) {
    return std::max(-1.0f, std::min(1.0f, value));
}

float ClampTrigger(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

} // namespace

GamepadDeviceInfo DetectGamepad() {
    GamepadDeviceInfo inaccessible;

    for (const std::string &path : GlobPaths("/dev/input/event*")) {
        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            const bool gamepad = EventFdIsGamepad(fd);
            const std::string name = gamepad ? EventName(fd, path) : std::string();
            close(fd);
            if (!gamepad) continue;
            return {true, true, GamepadBackend::Evdev, path,
                    name.empty() ? "Tay cầm" : name};
        }

        const std::string node = Basename(path);
        if (!inaccessible.connected &&
            SysfsCapabilityHasBit("/sys/class/input/" + node +
                                      "/device/capabilities/key",
                                  BTN_GAMEPAD)) {
            inaccessible = {true, false, GamepadBackend::Evdev, path,
                            EventName(-1, path)};
            if (inaccessible.name.empty()) inaccessible.name = "Tay cầm";
        }
    }

    // joydev is optional on modern systems, but is still useful for older USB
    // adapters. Resolve aliases so by-id/by-path duplicates do not win twice.
    std::vector<std::string> joystick_paths = GlobPaths("/dev/input/js*");
    const auto by_id = GlobPaths("/dev/input/by-id/*-joystick");
    const auto by_path = GlobPaths("/dev/input/by-path/*-joystick");
    joystick_paths.insert(joystick_paths.end(), by_id.begin(), by_id.end());
    joystick_paths.insert(joystick_paths.end(), by_path.begin(), by_path.end());
    std::unordered_set<std::string> seen;
    for (const std::string &candidate : joystick_paths) {
        const std::string path = ResolvePath(candidate);
        if (!seen.insert(path).second) continue;
        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        std::string name = JoystickName(fd, path);
        close(fd);
        return {true, true, GamepadBackend::Joystick, path,
                name.empty() ? "Tay cầm" : name};
    }

    return inaccessible;
}

struct GamepadDevice::Impl {
    struct Source {
        int fd = -1;
        bool touchpad = false;
        bool primary = false;
    };

    struct AxisRange {
        bool valid = false;
        int minimum = -32767;
        int maximum = 32767;
        int flat = 0;
    };

    GamepadBackend backend = GamepadBackend::Evdev;
    std::string primary_path;
    std::string error;
    std::vector<Source> sources;
    std::array<AxisRange, ABS_CNT> axes{};
    std::array<unsigned char, ABS_CNT> js_axis_map{};
    std::array<unsigned short, KEY_MAX - BTN_MISC + 1> js_button_map{};
    unsigned char js_axis_count = 0;
    unsigned char js_button_count = 0;

    bool key_dpad_up = false;
    bool key_dpad_down = false;
    bool key_dpad_left = false;
    bool key_dpad_right = false;
    int hat_x = 0;
    int hat_y = 0;
    bool touchpad_button = false;
    bool touchpad_contact = false;
    bool needs_sync = false;

    void Close() {
        for (auto &source : sources) {
            if (source.fd >= 0) close(source.fd);
            source.fd = -1;
        }
        sources.clear();
        primary_path.clear();
        axes = {};
        key_dpad_up = key_dpad_down = key_dpad_left = key_dpad_right = false;
        hat_x = hat_y = 0;
        touchpad_button = touchpad_contact = false;
        needs_sync = false;
    }

    float Bipolar(unsigned int code, int value) const {
        if (code >= axes.size() || !axes[code].valid) {
            return ClampUnit(static_cast<float>(value) / 32767.0f);
        }
        const AxisRange &range = axes[code];
        const float center = (range.minimum + range.maximum) * 0.5f;
        const float distance = static_cast<float>(value) - center;
        const float half = distance >= 0.0f ? range.maximum - center
                                            : center - range.minimum;
        if (half <= 0.0f) return 0.0f;
        if (std::fabs(distance) <= range.flat) return 0.0f;
        return ClampUnit(distance / half);
    }

    float Trigger(unsigned int code, int value) const {
        if (code >= axes.size() || !axes[code].valid)
            return ClampTrigger((static_cast<float>(value) + 32767.0f) / 65534.0f);
        const AxisRange &range = axes[code];
        const int span = range.maximum - range.minimum;
        if (span <= 0) return 0.0f;
        return ClampTrigger(static_cast<float>(value - range.minimum) / span);
    }

    void UpdateDpad(GamepadSnapshot &state) const {
        state.dpad_up = key_dpad_up || hat_y < 0;
        state.dpad_down = key_dpad_down || hat_y > 0;
        state.dpad_left = key_dpad_left || hat_x < 0;
        state.dpad_right = key_dpad_right || hat_x > 0;
    }

    void ApplyKey(GamepadSnapshot &state, unsigned int code, bool pressed,
                  bool touchpad_source, bool record) {
        const char *label = nullptr;
        if (touchpad_source && code == BTN_LEFT) {
            touchpad_button = pressed;
            state.touchpad = touchpad_button || touchpad_contact;
            label = "Touchpad";
        } else if (touchpad_source && code == BTN_TOUCH) {
            touchpad_contact = pressed;
            state.touchpad = touchpad_button || touchpad_contact;
            label = "Touchpad";
        } else {
            switch (code) {
                case BTN_GAMEPAD: state.cross = pressed; label = "Nút ×"; break;
                case BTN_EAST: state.circle = pressed; label = "Nút ○"; break;
                case BTN_NORTH: state.triangle = pressed; label = "Nút △"; break;
                case BTN_WEST: state.square = pressed; label = "Nút □"; break;
                case BTN_TL: state.l1 = pressed; label = "L1"; break;
                case BTN_TR: state.r1 = pressed; label = "R1"; break;
                case BTN_TL2: state.l2_button = pressed; label = "L2"; break;
                case BTN_TR2: state.r2_button = pressed; label = "R2"; break;
                case BTN_THUMBL: state.l3 = pressed; label = "L3"; break;
                case BTN_THUMBR: state.r3 = pressed; label = "R3"; break;
                case BTN_SELECT: state.create = pressed; label = "Create"; break;
                case BTN_START: state.options = pressed; label = "Options"; break;
                case BTN_MODE: state.ps = pressed; label = "PS"; break;
#ifdef BTN_DPAD_UP
                case BTN_DPAD_UP: key_dpad_up = pressed; label = "D-pad lên"; break;
                case BTN_DPAD_DOWN: key_dpad_down = pressed; label = "D-pad xuống"; break;
                case BTN_DPAD_LEFT: key_dpad_left = pressed; label = "D-pad trái"; break;
                case BTN_DPAD_RIGHT: key_dpad_right = pressed; label = "D-pad phải"; break;
#endif
                default: break;
            }
            UpdateDpad(state);
        }
        if (record && label) {
            state.last_input = std::string(label) + (pressed ? " · nhấn" : " · thả");
        }
    }

    void ApplyAbs(GamepadSnapshot &state, unsigned int code, int value,
                  bool record) {
        const char *label = nullptr;
        switch (code) {
            case ABS_X: state.left_x = Bipolar(code, value); label = "Stick trái"; break;
            case ABS_Y: state.left_y = Bipolar(code, value); label = "Stick trái"; break;
            case ABS_RX: state.right_x = Bipolar(code, value); label = "Stick phải"; break;
            case ABS_RY: state.right_y = Bipolar(code, value); label = "Stick phải"; break;
            case ABS_Z:
                state.has_l2_axis = true;
                state.l2 = Trigger(code, value);
                label = "L2";
                break;
            case ABS_RZ:
                state.has_r2_axis = true;
                state.r2 = Trigger(code, value);
                label = "R2";
                break;
            case ABS_HAT0X:
                hat_x = value;
                UpdateDpad(state);
                label = value < 0 ? "D-pad trái" : value > 0 ? "D-pad phải" : "D-pad";
                break;
            case ABS_HAT0Y:
                hat_y = value;
                UpdateDpad(state);
                label = value < 0 ? "D-pad lên" : value > 0 ? "D-pad xuống" : "D-pad";
                break;
            default: break;
        }
        if (!record || !label) return;
        bool meaningful = true;
        if (code == ABS_X) meaningful = std::fabs(state.left_x) > 0.25f;
        else if (code == ABS_Y) meaningful = std::fabs(state.left_y) > 0.25f;
        else if (code == ABS_RX) meaningful = std::fabs(state.right_x) > 0.25f;
        else if (code == ABS_RY) meaningful = std::fabs(state.right_y) > 0.25f;
        else if (code == ABS_Z) meaningful = state.l2 > 0.08f;
        else if (code == ABS_RZ) meaningful = state.r2 > 0.08f;
        if (meaningful) state.last_input = label;
    }

    void SyncEvdev(GamepadSnapshot &state) {
        state = {};
        state.connected = !sources.empty() && sources.front().fd >= 0;
        key_dpad_up = key_dpad_down = key_dpad_left = key_dpad_right = false;
        hat_x = hat_y = 0;
        touchpad_button = touchpad_contact = false;

        for (const Source &source : sources) {
            if (source.fd < 0) continue;
            std::array<unsigned long, (KEY_MAX / kBitsPerWord) + 1> keys{};
            if (ioctl(source.fd, EVIOCGKEY(sizeof(keys)), keys.data()) >= 0) {
                for (unsigned int code = 0; code <= KEY_MAX; ++code) {
                    if (HasBit(keys, code))
                        ApplyKey(state, code, true, source.touchpad, false);
                }
            }
            if (!source.primary) continue;
            for (unsigned int code : {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z,
                                      ABS_RZ, ABS_HAT0X, ABS_HAT0Y}) {
                input_absinfo info{};
                if (ioctl(source.fd, EVIOCGABS(code), &info) < 0) continue;
                axes[code] = {true, info.minimum, info.maximum, info.flat};
                ApplyAbs(state, code, info.value, false);
            }
        }
    }
};

GamepadDevice::GamepadDevice() : impl_(std::make_unique<Impl>()) {}
GamepadDevice::~GamepadDevice() { impl_->Close(); }

bool GamepadDevice::Open(const GamepadDeviceInfo &device) {
    impl_->Close();
    impl_->error.clear();
    if (!device.connected || device.path.empty()) {
        impl_->error = "Chưa phát hiện tay cầm";
        return false;
    }

    const int primary = open(device.path.c_str(),
                             O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (primary < 0) {
        impl_->error = std::string("Không thể đọc ") + device.path + ": " +
                       std::strerror(errno);
        return false;
    }

    impl_->backend = device.backend;
    impl_->primary_path = device.path;
    impl_->sources.push_back({primary, false, true});

    if (device.backend == GamepadBackend::Evdev) {
        if (!EventFdIsGamepad(primary)) {
            impl_->error = "Thiết bị không còn cung cấp gamepad input";
            impl_->Close();
            return false;
        }

        // DualSense exposes its clickable touchpad as a sibling event node
        // (BTN_LEFT + INPUT_PROP_BUTTONPAD), not on the primary gamepad node.
        const std::string primary_root = EventHardwareRoot(device.path);
        const std::string primary_unique = EventUniqueId(primary);
        for (const std::string &path : GlobPaths("/dev/input/event*")) {
            if (ResolvePath(path) == ResolvePath(device.path)) continue;
            const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            const std::string root = EventHardwareRoot(path);
            const std::string unique = EventUniqueId(fd);
            const bool sibling = (!primary_unique.empty() && unique == primary_unique) ||
                                 (!primary_root.empty() && root == primary_root);
            const std::string name = EventName(fd, path);
            if (sibling && IsTouchpadEvent(fd, name))
                impl_->sources.push_back({fd, true, false});
            else
                close(fd);
        }
        impl_->needs_sync = true;
    } else {
        ioctl(primary, JSIOCGAXES, &impl_->js_axis_count);
        ioctl(primary, JSIOCGBUTTONS, &impl_->js_button_count);
        impl_->js_axis_map.fill(0);
        impl_->js_button_map.fill(0);
        ioctl(primary, JSIOCGAXMAP, impl_->js_axis_map.data());
        ioctl(primary, JSIOCGBTNMAP, impl_->js_button_map.data());
        for (unsigned int code : {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ,
                                  ABS_HAT0X, ABS_HAT0Y}) {
            impl_->axes[code] = {true, -32767, 32767, 2048};
        }
    }
    return true;
}

void GamepadDevice::Close() { impl_->Close(); }

bool GamepadDevice::Poll(GamepadSnapshot &snapshot) {
    if (impl_->sources.empty()) return false;
    bool changed = false;

    if (impl_->backend == GamepadBackend::Evdev && impl_->needs_sync) {
        impl_->SyncEvdev(snapshot);
        impl_->needs_sync = false;
        changed = true;
    }

    if (impl_->backend == GamepadBackend::Joystick) {
        Impl::Source &source = impl_->sources.front();
        if (source.fd < 0) return changed;
        for (;;) {
            js_event event{};
            const ssize_t bytes = read(source.fd, &event, sizeof(event));
            if (bytes == static_cast<ssize_t>(sizeof(event))) {
                snapshot.connected = true;
                const unsigned char type = event.type & ~JS_EVENT_INIT;
                const bool record = (event.type & JS_EVENT_INIT) == 0;
                if (type == JS_EVENT_BUTTON &&
                    event.number < impl_->js_button_count &&
                    event.number < impl_->js_button_map.size()) {
                    impl_->ApplyKey(snapshot, impl_->js_button_map[event.number],
                                    event.value != 0, false, record);
                    changed = true;
                } else if (type == JS_EVENT_AXIS &&
                           event.number < impl_->js_axis_count &&
                           event.number < impl_->js_axis_map.size()) {
                    impl_->ApplyAbs(snapshot, impl_->js_axis_map[event.number],
                                    event.value, record);
                    changed = true;
                }
                continue;
            }
            if (bytes == 0 || (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(source.fd);
                source.fd = -1;
                impl_->error = "Tay cầm đã ngắt kết nối";
                snapshot = {};
                snapshot.last_input = impl_->error;
                changed = true;
            }
            break;
        }
        return changed;
    }

    bool resync = false;
    for (Impl::Source &source : impl_->sources) {
        if (source.fd < 0) continue;
        for (;;) {
            std::array<input_event, 32> events{};
            const ssize_t bytes = read(source.fd, events.data(), sizeof(events));
            if (bytes > 0) {
                const size_t count = static_cast<size_t>(bytes) / sizeof(input_event);
                for (size_t i = 0; i < count; ++i) {
                    const input_event &event = events[i];
                    if (event.type == EV_SYN && event.code == SYN_DROPPED) {
                        resync = true;
                    } else if (event.type == EV_KEY) {
                        impl_->ApplyKey(snapshot, event.code, event.value != 0,
                                        source.touchpad, true);
                        changed = true;
                    } else if (event.type == EV_ABS && source.primary) {
                        impl_->ApplyAbs(snapshot, event.code, event.value, true);
                        changed = true;
                    }
                }
                continue;
            }
            if (bytes == 0 || (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                close(source.fd);
                source.fd = -1;
                if (source.primary) {
                    impl_->error = "Tay cầm đã ngắt kết nối";
                    snapshot = {};
                    snapshot.last_input = impl_->error;
                    changed = true;
                } else if (source.touchpad) {
                    impl_->touchpad_button = false;
                    impl_->touchpad_contact = false;
                    snapshot.touchpad = false;
                    changed = true;
                }
            }
            break;
        }
    }
    if (resync) {
        impl_->SyncEvdev(snapshot);
        changed = true;
    }
    return changed;
}

const std::string &GamepadDevice::LastError() const { return impl_->error; }

} // namespace jetson::input
