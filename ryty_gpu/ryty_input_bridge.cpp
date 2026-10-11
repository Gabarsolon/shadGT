// Controller input for the Ryty build: shadGT's window and input handler fill its GameController
// states (keyboard bindings and SDL gamepads), and this thread forwards them to Ryty's libScePad,
// which is what the game reads. The game's rumble and light bar requests travel back the same way.

#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "input/controller.h"
#include "video_core/renderdoc.h"
#include "ryty_gpu/ryty_input_bridge.h"

namespace {

// Mirrors Ryty's PadInputState (core/libs/prx/libScePad/include/PadState.hpp).
struct RytyPadTouchPoint {
    bool active = false;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint8_t id = 0;
};

struct RytyPadInputState {
    std::uint32_t buttons = 0;
    std::array<std::uint8_t, 4> sticks{128, 128, 128, 128};
    std::uint8_t analogButtonsL2 = 0;
    std::uint8_t analogButtonsR2 = 0;
    bool touchLeft = false;
    bool touchRight = false;
    bool hasMotion = false;
    std::array<float, 3> accel{0.0f, 9.80665f, 0.0f};
    std::array<float, 3> gyro{0.0f, 0.0f, 0.0f};
    std::array<RytyPadTouchPoint, 2> touch{};
    std::uint8_t deviceKind = 0;
};

// Mirrors Ryty's PadOutputState: what the game asked of the controller (rumble, light bar).
struct RytyPadTriggerRequest {
    bool valid = false;
    std::uint8_t effect[11]{};
    std::uint8_t fallback = 0;
    std::uint32_t mode = 0;
};

struct RytyPadOutputState {
    std::uint32_t sequence = 0;
    std::uint8_t vibrationLarge = 0;
    std::uint8_t vibrationSmall = 0;
    bool lightBarValid = false;
    std::uint8_t lightBar[3]{};
    RytyPadTriggerRequest trigger[2];
    bool triggerTouched = false;
    bool motionEnabled = true;
    int vibrationMode = 0;
};

// Test hook: RYTY_TEST_SCRIPT="50:shot;52:cross;65:shot" presses buttons and saves game-only
// screenshots at the given seconds after the renderer starts, without touching the host keyboard.
// "r2on"/"r2off" hold the accelerator (R2, fully pressed) until released.
std::atomic<std::uint32_t> g_scripted_buttons{0};
std::atomic<bool> g_scripted_r2{false};

std::uint32_t ButtonFromName(std::string_view name) {
    using B = Libraries::Pad::OrbisPadButtonDataOffset;
    static const std::pair<std::string_view, B> names[] = {
        {"cross", B::Cross}, {"circle", B::Circle}, {"square", B::Square}, {"triangle", B::Triangle},
        {"options", B::Options}, {"up", B::Up}, {"down", B::Down}, {"left", B::Left}, {"right", B::Right},
        {"l1", B::L1}, {"r1", B::R1}, {"touchpad", B::TouchPad}};
    for (const auto& [key, button] : names) {
        if (key == name) return static_cast<std::uint32_t>(button);
    }
    return 0;
}

void RunTestScript(std::string script) {
    const auto start = std::chrono::steady_clock::now();
    std::size_t position = 0;
    while (position < script.size()) {
        const auto end = std::min(script.find(';', position), script.size());
        const std::string step = script.substr(position, end - position);
        position = end + 1;
        const auto colon = step.find(':');
        if (colon == std::string::npos) continue;
        const double at = std::stod(step.substr(0, colon));
        const std::string action = step.substr(colon + 1);
        std::this_thread::sleep_until(start + std::chrono::milliseconds(static_cast<long long>(at * 1000)));
        if (action == "r2on" || action == "r2off") {
            g_scripted_r2 = action == "r2on";
            LOG_INFO(Input, "Ryty test script: {} at {} s", action, at);
        } else if (action == "shot") {
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
            LOG_INFO(Input, "Ryty test script: screenshot at {} s", at);
        } else if (action == "shotwindow") {
            // What the window shows (the splash screen, overlays), not just the game's image.
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::WithOverlays);
            LOG_INFO(Input, "Ryty test script: window screenshot at {} s", at);
        } else if (action.starts_with("burst")) {
            // "burstN": N screenshot requests; one is taken per frame, so N consecutive frames.
            const int count = action.size() > 5 ? std::stoi(action.substr(5)) : 30;
            for (int i = 0; i < count; ++i) {
                VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
            }
            LOG_INFO(Input, "Ryty test script: {} screenshots from {} s", count, at);
        } else if (const auto button = ButtonFromName(action); button != 0) {
            g_scripted_buttons |= button;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            g_scripted_buttons &= ~button;
            LOG_INFO(Input, "Ryty test script: pressed {} at {} s", action, at);
        }
    }
}

// libScePad.prx: PadPublishInput (Microsoft ABI, exported by NID).
using PublishFn = void (*)(const RytyPadInputState&);
// libScePad.prx: PadFetchOutput (Microsoft ABI, exported by NID); true when the output changed.
using FetchOutputFn = bool (*)(std::uint32_t*, RytyPadOutputState*);

std::uint8_t ClampAxis(s32 value) {
    return static_cast<std::uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

void ForwardInput() {
    HMODULE pad = GetModuleHandleA("libScePad.prx");
    const auto publish = pad ? reinterpret_cast<PublishFn>(GetProcAddress(pad, "MCBKDgnIVB8")) : nullptr;
    if (publish == nullptr) {
        LOG_ERROR(Input, "Ryty: libScePad.prx has no PadPublishInput; controller input is disabled");
        return;
    }
    const auto fetch_output =
        reinterpret_cast<FetchOutputFn>(GetProcAddress(pad, "FDjxaWJZf9Y"));
    auto* controllers = Common::Singleton<Input::GameControllers>::Instance();
    RytyPadInputState last{};
    bool first = true;
    std::uint32_t output_seen = 0;
    RytyPadOutputState last_output{};
    while (true) {
        // Rumble and light bar requests go to the first controller, as shadGT's pad library
        // would send them (SetVibration takes the small motor first).
        if (RytyPadOutputState output; fetch_output && fetch_output(&output_seen, &output)) {
            auto& controller = *(*controllers)[0];
            if (output.vibrationLarge != last_output.vibrationLarge ||
                output.vibrationSmall != last_output.vibrationSmall) {
                const bool sent =
                    controller.SetVibration(output.vibrationSmall, output.vibrationLarge);
                static int logged = 0;
                if (logged++ < 10) {
                    LOG_INFO(Input, "Ryty: game rumble large {} small {} -> controller ({})",
                             output.vibrationLarge, output.vibrationSmall,
                             controller.m_sdl_gamepad ? (sent ? "sent" : "refused")
                                                      : "no gamepad");
                }
            }
            if (output.lightBarValid &&
                std::memcmp(output.lightBar, last_output.lightBar, sizeof(output.lightBar)) != 0) {
                controller.SetLightBarRGB(output.lightBar[0], output.lightBar[1],
                                          output.lightBar[2]);
            }
            last_output = output;
        }
        // The first controller carries the keyboard bindings and the first gamepad.
        const auto state = (*controllers)[0]->ReadState();
        RytyPadInputState input{};
        input.buttons = static_cast<std::uint32_t>(state.buttonsState) | g_scripted_buttons.load();
        input.sticks = {ClampAxis(state.axes[0]), ClampAxis(state.axes[1]), ClampAxis(state.axes[2]),
                        ClampAxis(state.axes[3])};
        input.analogButtonsL2 = ClampAxis(state.axes[4]);
        input.analogButtonsR2 = ClampAxis(state.axes[5]);
        if (g_scripted_r2) {
            input.buttons |= static_cast<std::uint32_t>(Libraries::Pad::OrbisPadButtonDataOffset::R2);
            input.analogButtonsR2 = 255;
        }
        if (first || std::memcmp(&input.buttons, &last.buttons, sizeof(input.buttons)) != 0 ||
            input.sticks != last.sticks || input.analogButtonsL2 != last.analogButtonsL2 ||
            input.analogButtonsR2 != last.analogButtonsR2) {
            publish(input);
            last = input;
            first = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

} // namespace

namespace Ryty {

void StartInputBridge() {
    std::thread(ForwardInput).detach();
    if (const char* script = std::getenv("RYTY_TEST_SCRIPT"); script != nullptr && *script != 0) {
        std::thread(RunTestScript, std::string(script)).detach();
    }
}

} // namespace Ryty
