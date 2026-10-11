#include "xrfg/status_panel_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <utility>

namespace xrfg {
namespace {
// Daniel Hepper's public-domain 8x8 font: one byte a row, top first, the
// least significant bit the leftmost pixel. See THIRD_PARTY.md.
#include "xrfg/third_party/font8x8_basic.h"
}  // namespace

StatusPanelMode parse_status_panel_mode(std::string_view value) noexcept {
    if (value == "off") return StatusPanelMode::off;
    if (value == "always") return StatusPanelMode::always;
    return StatusPanelMode::gesture;
}

const char* status_panel_mode_name(StatusPanelMode mode) noexcept {
    switch (mode) {
    case StatusPanelMode::off: return "off";
    case StatusPanelMode::always: return "always";
    default: return "gesture";
    }
}

namespace {

[[nodiscard]] Quaternion multiply(const Quaternion& a, const Quaternion& b) noexcept {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

[[nodiscard]] Vec3 rotate(const Quaternion& q, const Vec3& v) noexcept {
    // v + 2 q.xyz x (q.xyz x v + w v)
    const Vec3 c{q.y * v.z - q.z * v.y + q.w * v.x,
                 q.z * v.x - q.x * v.z + q.w * v.y,
                 q.x * v.y - q.y * v.x + q.w * v.z};
    return {v.x + 2.0F * (q.y * c.z - q.z * c.y),
            v.y + 2.0F * (q.z * c.x - q.x * c.z),
            v.z + 2.0F * (q.x * c.y - q.y * c.x)};
}

}  // namespace

bool grip_upside_down(const Quaternion& grip, float angle_degrees) noexcept {
    const Quaternion unit = normalize(grip);
    // The degrees the user thinks in become the cosine the up axis is
    // compared with: past 120 degrees, its height is below -0.5.
    const float limit = std::cos(std::clamp(angle_degrees, 45.0F, 179.0F) *
                                 3.14159265358979323846F / 180.0F);
    return rotate(unit, {0.0F, 1.0F, 0.0F}).y < limit;
}

bool FlipGesture::observe(std::int64_t now_ns, bool left_flipped, bool right_flipped) noexcept {
    if (left_flipped || right_flipped) {
        upright_since_ = -1;
        // The panel stays on the hand that brought it up while that hand is
        // still turned; the other hand only takes it once that one is not.
        const bool kept = (hand_ == 0 && left_flipped) || (hand_ == 1 && right_flipped);
        if (!kept) hand_ = left_flipped ? 0 : 1;
        if (flipped_since_ < 0) {
            flipped_since_ = now_ns;
        } else if (!visible_ && now_ns - flipped_since_ >= kFlipShowDelayNs) {
            visible_ = true;
        }
    } else {
        flipped_since_ = -1;
        if (visible_) {
            if (upright_since_ < 0) {
                upright_since_ = now_ns;
            } else if (now_ns - upright_since_ >= kFlipHideDelayNs) {
                visible_ = false;
                upright_since_ = -1;
                hand_ = -1;
            }
        } else {
            hand_ = -1;
        }
    }
    return visible_;
}

Pose panel_pose_on_grip(const Pose& grip, float panel_height_m) noexcept {
    // Half a turn about the grip's Z, the controller's length. Upside down,
    // that turns the panel back upright, facing along +Z: back at the user.
    constexpr Quaternion kHalfTurnZ{0.0F, 0.0F, 1.0F, 0.0F};
    const Quaternion orientation = normalize(grip.orientation);
    // Along the grip's -Y, which is up while the controller is upside down:
    // xrFPS's 11 cm for its own small panel, here half this one's height and
    // a 5 cm margin, so its lower edge clears the controller.
    const float lift = std::max(panel_height_m, 0.0F) * 0.5F + 0.05F;
    const Vec3 offset = rotate(orientation, {0.0F, -lift, 0.0F});
    return {normalize(multiply(orientation, kHalfTurnZ)),
            {grip.position.x + offset.x, grip.position.y + offset.y, grip.position.z + offset.z}};
}

namespace {

// What actually makes the frames: the method a configuration comes down to,
// so the one asked for and the one running can be compared.
enum class Engine {
    none,
    flow,
    vectors,
    hybrid,
    extrapolate_flow,
    extrapolate_vectors,
    extrapolate_both,
    native,
};

[[nodiscard]] bool uses_vectors(Engine engine) noexcept {
    return engine == Engine::vectors || engine == Engine::hybrid ||
        engine == Engine::extrapolate_vectors || engine == Engine::extrapolate_both;
}

// The asked engine is the best case: a game that gives DLSS vectors. A game
// without them runs the flow instead, which the panel shows as a fallback.
[[nodiscard]] Engine asked_engine(const PanelMethod& method) noexcept {
    if (method.native) return Engine::native;
    if (method.extrapolate != 0) {
        if (!method.game_vectors) return Engine::extrapolate_flow;
        return method.extrapolate == 2 ? Engine::extrapolate_both : Engine::extrapolate_vectors;
    }
    if (method.game_vectors) return method.hybrid ? Engine::hybrid : Engine::vectors;
    return Engine::flow;
}

// Whether the game's guides went into this window's pairs. A window with no
// pair either way - the panel's first, after a while hidden - falls back on
// the last pair's report.
[[nodiscard]] bool vectors_in_use(const PanelVectors& vectors) noexcept {
    if (vectors.used > 0) return true;
    return vectors.temporal_rejections == 0 && vectors.invalid_rejections == 0 &&
        vectors.status == PanelVectorStatus::used;
}

[[nodiscard]] Engine running_engine(const StatusPanelInput& input) noexcept {
    const PanelMethod& method = input.running;
    if (!input.rates.generating) return Engine::none;
    const bool used = vectors_in_use(input.vectors);
    // A native pair NGX skipped still goes out, as a copy of the real frame:
    // only a pair that used the guides was generated.
    if (method.native) return used ? Engine::native : Engine::none;
    if (method.extrapolate != 0) {
        if (!used) return Engine::extrapolate_flow;
        return method.extrapolate == 2 ? Engine::extrapolate_both : Engine::extrapolate_vectors;
    }
    if (method.hybrid && used) return Engine::hybrid;
    if (method.game_vectors && used) return Engine::vectors;
    return Engine::flow;
}

[[nodiscard]] std::string flow_text(const PanelMethod& method) {
    std::string text = method.flow == PanelFlow::nvidia ? "NVIDIA flow " : "FidelityFX flow ";
    if (method.flow == PanelFlow::nvidia) {
        text += method.preset == PanelPreset::slow ? "slow "
            : method.preset == PanelPreset::fast  ? "fast "
                                                  : "medium ";
    }
    text += std::to_string(method.flow_scale) + "%";
    if (method.flow == PanelFlow::nvidia && method.both_ways) text += " both ways";
    return text;
}

[[nodiscard]] std::string shape_text(const PanelMethod& method) {
    std::string text = method.frames > 2 ? ", 3X" : ", 2X";
    if (method.frames_auto) text += " (auto)";
    if (method.deep) text += ", deep";
    return text;
}

[[nodiscard]] std::string engine_text(Engine engine, const PanelMethod& method) {
    const std::string warp = method.mesh ? "" : " (gather)";
    switch (engine) {
    case Engine::none: return "Real frames only";
    case Engine::native:
        return "DLSS Frame Generation " + std::to_string(method.native_scale) + "%" +
            (method.native_camera ? " + camera" : "");
    case Engine::extrapolate_flow: return "Extrapolate" + warp + ", FidelityFX flow";
    case Engine::extrapolate_vectors: return "Extrapolate" + warp + ", DLSS vectors and depth";
    case Engine::extrapolate_both: return "Extrapolate" + warp + ", DLSS vectors + FidelityFX flow";
    case Engine::hybrid: return "Hybrid: DLSS vectors + FidelityFX flow";
    case Engine::vectors: return "DLSS vectors";
    default: return flow_text(method);
    }
}

[[nodiscard]] std::string format(const char* pattern, double value) {
    char buffer[48]{};
    std::snprintf(buffer, sizeof(buffer), pattern, value);
    return buffer;
}

[[nodiscard]] std::string whole(float value) {
    return std::to_string(static_cast<long>(std::lround(std::isfinite(value) ? std::max(value, 0.0F) : 0.0F)));
}

[[nodiscard]] const char* vector_status_text(const PanelVectors& vectors) noexcept {
    switch (vectors.status) {
    case PanelVectorStatus::disabled: return "off";
    case PanelVectorStatus::output_not_direct: return "unused: DLSS output is not the eye image";
    case PanelVectorStatus::queue_mismatch: return "unused: DLSS ran on another queue";
    case PanelVectorStatus::invalid_input: return "refused: invalid";
    case PanelVectorStatus::temporal_mismatch: return "refused: they did not match the frames";
    case PanelVectorStatus::used: return "in use";
    case PanelVectorStatus::waiting_for_depth: return "waiting for the game's depth";
    case PanelVectorStatus::native_unavailable: return "DLSS Frame Generation could not run";
    case PanelVectorStatus::multi_frame_unsupported: return "this GPU's DLSS FG makes one frame a pair";
    case PanelVectorStatus::no_matching_output: return "unused: no DLSS output is the eye images' size";
    default:
        return vectors.published == 0 ? "none from this game yet" : "none in the last moment";
    }
}

// Why OFXR is not following the game's vectors, for a method that would.
[[nodiscard]] std::string vector_fallback(const StatusPanelInput& input) {
    if (input.vectors.published == 0) return "No DLSS vectors from this game";
    switch (input.vectors.status) {
    case PanelVectorStatus::temporal_mismatch: return "The game's DLSS vectors did not match the frames";
    case PanelVectorStatus::invalid_input: return "The game's DLSS vectors were refused as invalid";
    case PanelVectorStatus::output_not_direct: return "DLSS vectors unused: DLSS output is not the eye image";
    case PanelVectorStatus::queue_mismatch: return "DLSS vectors unused: DLSS ran on another queue";
    case PanelVectorStatus::no_matching_output: return "DLSS vectors unused: no DLSS output is the eye images' size";
    case PanelVectorStatus::waiting_for_depth: return "Waiting for the game's depth";
    default: return "The game's DLSS vectors stopped arriving";
    }
}

[[nodiscard]] const char* bypass_text(PanelBypass bypass) noexcept {
    switch (bypass) {
    case PanelBypass::cooldown: return "Holding off for a moment after a change";
    case PanelBypass::quarantine: return "Holding off for a second: the game's frames changed";
    case PanelBypass::no_projection: return "Nothing to generate from: no eye images in the frame";
    case PanelBypass::waiting_ahead: return "Passing frames through: the game waits ahead";
    default: return "Waiting for frames to generate from";
    }
}

[[nodiscard]] const char* graphics_text(PanelGraphics graphics) noexcept {
    switch (graphics) {
    case PanelGraphics::d3d12: return "D3D12";
    case PanelGraphics::d3d11_bridge: return "D3D11 through the D3D12 bridge";
    case PanelGraphics::d3d11_interop: return "D3D11 interop";
    case PanelGraphics::vulkan_bridge: return "Vulkan through the D3D12 bridge";
    case PanelGraphics::vulkan_interop: return "Vulkan interop";
    default: return "Graphics API not supported";
    }
}

[[nodiscard]] int severity(PanelTone tone) noexcept {
    return tone == PanelTone::bad ? 2 : tone == PanelTone::warn ? 1 : 0;
}

// The asked method as the session would run it: 3X and extrapolation run
// the shallow pipeline whatever "Prefer FPS over latency" says.
[[nodiscard]] PanelMethod normalized(PanelMethod method) noexcept {
    if (method.frames > 2 || method.extrapolate != 0) method.deep = false;
    if (!method.game_vectors) method.hybrid = false;
    if (method.native) {
        method.hybrid = false;
        method.extrapolate = 0;
    }
    return method;
}

}  // namespace

std::string describe_asked(const PanelMethod& method) {
    const PanelMethod asked = normalized(method);
    return engine_text(asked_engine(asked), asked) + shape_text(asked);
}

std::string describe_running(const StatusPanelInput& input) {
    const Engine engine = running_engine(input);
    if (engine == Engine::none) return engine_text(engine, input.running);
    return engine_text(engine, input.running) + shape_text(input.running);
}

std::vector<PanelEditable> panel_editables(const StatusPanelInput& input) {
    std::vector<PanelEditable> rows;
    const PanelMethod& asked = input.asked;
    {
        // Not "Frames": the status has a line of that name.
        PanelEditable frames{PanelSetting::frames, "Per frame", {"2X", "3X", "auto"},
                             asked.frames_auto ? 2 : asked.frames > 2 ? 1 : 0};
        if (input.graphics == PanelGraphics::d3d11_interop) {
            frames.available = false;
            frames.note = "not through the D3D11 interop";
        } else if (asked.native && input.native_single_frame) {
            frames.available = false;
            frames.note = "this GPU's DLSS Frame Generation makes one frame";
        } else if (input.triple_fixed) {
            frames.live = false;
            frames.note = "from the next game start";
        }
        rows.push_back(std::move(frames));
    }
    {
        PanelEditable method{PanelSetting::method, "Method", {"OFXR", "DLSS Frame Generation"},
                             asked.native ? 1 : 0};
        if (!input.native_available && !asked.native) {
            method.available = false;
            method.note = "DLSS Frame Generation is unavailable here";
        }
        rows.push_back(std::move(method));
    }
    if (asked.native) {
        rows.push_back({PanelSetting::native_scale, "Resolution", {"50%", "67%", "100%"},
                        asked.native_scale >= 100 ? 2 : asked.native_scale >= 67 ? 1 : 0});
        rows.push_back({PanelSetting::native_camera, "Camera", {"off", "on"}, asked.native_camera ? 1 : 0,
                        false, true, "from the next game start"});
    } else {
        PanelEditable flow{PanelSetting::flow, "Flow",
                           {"FidelityFX", "NVIDIA slow", "NVIDIA medium", "NVIDIA fast"},
                           asked.flow == PanelFlow::fidelity_fx ? 0 : 1 + static_cast<int>(asked.preset)};
        if (input.nvidia_unavailable) {
            flow.choices = {"FidelityFX"};
            flow.current = 0;
            flow.available = false;
            flow.note = "NVIDIA optical flow is unavailable here";
        }
        rows.push_back(std::move(flow));
        rows.push_back({PanelSetting::flow_scale, "Flow scale", {"100%", "75%", "50%"},
                        asked.flow_scale >= 100 ? 0 : asked.flow_scale >= 75 ? 1 : 2});
        if (asked.flow == PanelFlow::nvidia) {
            rows.push_back({PanelSetting::both_ways, "Both ways", {"off", "on"}, asked.both_ways ? 1 : 0});
        }
        // The hybrid follows at once; extrapolation shapes the session.
        rows.push_back({PanelSetting::ofxr_mode, "Mode", {"interpolate", "hybrid", "extrapolate"},
                        asked.extrapolate != 0 ? 2 : asked.hybrid ? 1 : 0, true, true,
                        asked.extrapolate != 0 ? "" : ""});
    }
    rows.push_back({PanelSetting::pipeline, "Buffering", {"deep", "shallow"}, asked.deep ? 0 : 1, false, true,
                    "from the next game start"});
    rows.push_back({PanelSetting::pause, "Generation", {"running", "paused"}, input.paused ? 1 : 0});
    return rows;
}

PanelCommand PanelNavigator::feed(const PanelInputSample& sample, std::int64_t now_ns) noexcept {
    if (!sample.active) {
        reset();
        return PanelCommand::none;
    }
    PanelCommand command = PanelCommand::none;
    // The trigger applies once it has been held kPanelConfirmHoldNs, once
    // per press: a pull in play with the controller turned over - a reload,
    // a throw - must not change a setting. A staged choice says so.
    if (!sample.confirm) {
        confirm_since_ = -1;
        confirm_fired_ = false;
    } else {
        if (confirm_since_ < 0) confirm_since_ = now_ns;
        if (!confirm_fired_ && now_ns - confirm_since_ >= kPanelConfirmHoldNs) {
            confirm_fired_ = true;
            command = PanelCommand::confirm;
        }
    }
    const float ax = std::abs(sample.x), ay = std::abs(sample.y);
    int direction_x = 0, direction_y = 0;
    if (ay >= ax && ay >= kPanelDeadzone) {
        direction_y = sample.y > 0.0F ? 1 : -1;
    } else if (ax > ay && ax >= kPanelDeadzone) {
        direction_x = sample.x > 0.0F ? 1 : -1;
    }
    const auto step = [&](int x, int y) {
        return y > 0 ? PanelCommand::up : y < 0 ? PanelCommand::down
            : x > 0  ? PanelCommand::right : PanelCommand::left;
    };
    if (ax < kPanelRearm && ay < kPanelRearm) {
        held_x_ = held_y_ = 0;
        held_since_ = last_repeat_ = -1;
    } else if (direction_x != 0 || direction_y != 0) {
        if (held_x_ == 0 && held_y_ == 0) {
            held_x_ = direction_x;
            held_y_ = direction_y;
            held_since_ = last_repeat_ = now_ns;
            if (command == PanelCommand::none) command = step(direction_x, direction_y);
        } else if (direction_x == held_x_ && direction_y == held_y_) {
            if (now_ns - held_since_ >= kPanelRepeatDelayNs &&
                now_ns - last_repeat_ >= kPanelRepeatPeriodNs) {
                last_repeat_ = now_ns;
                if (command == PanelCommand::none) command = step(direction_x, direction_y);
            }
        } else {
            // A new direction without a release is a new press: a thumb
            // rolling from down to right to stage a choice does not pass
            // through the centre, and the test driver's stick did the same
            // once when two of its steps fell into one frame, so the row's
            // choice was never staged.
            held_x_ = direction_x;
            held_y_ = direction_y;
            held_since_ = last_repeat_ = now_ns;
            if (command == PanelCommand::none) command = step(direction_x, direction_y);
        }
    }
    return command;
}

std::optional<PanelChange> panel_cursor_apply(
    PanelCursor& cursor, const std::vector<PanelEditable>& editables, PanelCommand command,
    std::int64_t now_ns) noexcept {
    if (editables.empty() || command == PanelCommand::none) return std::nullopt;
    const int count = static_cast<int>(editables.size());
    if (cursor.row < 0 || cursor.row >= count) {
        // The first command brings the cursor up and does nothing else, so
        // a glance at the panel never changes anything.
        cursor.row = std::clamp(cursor.row, 0, count - 1);
        cursor.staged = -1;
        cursor.staged_at_ns = -1;
        return std::nullopt;
    }
    switch (command) {
    case PanelCommand::up:
    case PanelCommand::down:
        cursor.row = (cursor.row + (command == PanelCommand::down ? 1 : count - 1)) % count;
        cursor.staged = -1;
        cursor.staged_at_ns = -1;
        break;
    case PanelCommand::left:
    case PanelCommand::right: {
        const PanelEditable& row = editables[static_cast<std::size_t>(cursor.row)];
        const int choices = static_cast<int>(row.choices.size());
        if (!row.available || choices < 2) break;
        const int from = cursor.staged >= 0 ? cursor.staged : std::clamp(row.current, 0, choices - 1);
        const int to = (from + (command == PanelCommand::right ? 1 : choices - 1)) % choices;
        cursor.staged = to == row.current ? -1 : to;
        cursor.staged_at_ns = cursor.staged >= 0 ? now_ns : -1;
        break;
    }
    case PanelCommand::confirm:
        if (cursor.staged >= 0) {
            const PanelChange change{editables[static_cast<std::size_t>(cursor.row)].setting, cursor.staged};
            cursor.staged = -1;
            cursor.staged_at_ns = -1;
            return change;
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

void panel_cursor_expire(PanelCursor& cursor, std::int64_t now_ns) noexcept {
    if (cursor.staged >= 0 && cursor.staged_at_ns >= 0 && now_ns - cursor.staged_at_ns > kPanelStageTimeoutNs) {
        cursor.staged = -1;
        cursor.staged_at_ns = -1;
    }
}

StatusPanelText status_panel_text(const StatusPanelInput& input, const PanelCursor* cursor) {
    StatusPanelText text;
    // A fallback is the session running something other than what it was
    // started with; a newer choice in the tray is not one, only pending.
    const PanelMethod asked = normalized(input.asked);
    const PanelMethod set = normalized(input.session);
    const PanelMethod& running = input.running;
    const Engine wanted = asked_engine(set);
    const Engine engine = running_engine(input);
    const bool held = input.paused || !input.enabled || input.budget_exhausted;

    std::vector<PanelLine> reasons;
    const auto reason = [&](std::string line, PanelTone tone) {
        reasons.push_back({{}, std::move(line), tone});
    };
    if (input.paused) {
        reason("Paused from the tray: real frames only", PanelTone::warn);
    } else if (!input.enabled) {
        reason(input.settings_failed ? "A settings change failed: off until the next one"
                                     : "Generation is off in the tray",
               PanelTone::bad);
    } else if (input.budget_exhausted) {
        reason("The runtime refused a swapchain: real frames only this session", PanelTone::bad);
    } else if (engine == Engine::none && !(set.native && input.rates.generating)) {
        reason(bypass_text(input.bypass), PanelTone::warn);
    }
    if (input.declined_images > 0) {
        reason(std::to_string(input.declined_images) +
                   (input.declined_images == 1 ? " eye image could not be set up"
                                               : " eye images could not be set up"),
               PanelTone::warn);
    }
    if (!held) {
        if (set.native) {
            if (!input.native_available) {
                reason("DLSS Frame Generation is unavailable on this GPU or driver", PanelTone::bad);
            } else if (engine == Engine::none && input.rates.generating) {
                switch (input.vectors.status) {
                case PanelVectorStatus::native_unavailable:
                    reason("NGX could not run DLSS Frame Generation", PanelTone::bad);
                    break;
                case PanelVectorStatus::waiting_for_depth:
                    reason("Waiting for the game's depth", PanelTone::warn);
                    break;
                case PanelVectorStatus::invalid_input:
                    reason("The game's DLSS guides were refused as invalid", PanelTone::warn);
                    break;
                // Guides the game gives but that cannot be used, which this
                // line once called still awaited.
                case PanelVectorStatus::temporal_mismatch:
                    reason("The game's DLSS guides did not match the frames", PanelTone::warn);
                    break;
                case PanelVectorStatus::no_matching_output:
                    reason("The game's DLSS output is not the eye images' size", PanelTone::warn);
                    break;
                case PanelVectorStatus::queue_mismatch:
                    reason("The game's DLSS ran on another queue", PanelTone::warn);
                    break;
                default:
                    reason(input.vectors.published == 0 ? "No DLSS guides from this game yet"
                                                        : "Waiting for the game's DLSS guides",
                           PanelTone::warn);
                    break;
                }
            }
            if (set.frames > 2 && input.native_single_frame) {
                reason("This GPU's DLSS Frame Generation makes one frame: 2X", PanelTone::warn);
            }
        } else if (engine != Engine::none) {
            if (uses_vectors(wanted) && !uses_vectors(engine)) {
                reason(wanted == Engine::hybrid && !input.vectors_published
                           ? "The hybrid waits for the game's first DLSS vectors"
                           : vector_fallback(input),
                       PanelTone::warn);
            }
            if (set.flow == PanelFlow::nvidia && input.nvidia_unavailable &&
                (engine == Engine::flow || wanted == Engine::flow)) {
                reason("NVIDIA optical flow is unavailable here: FidelityFX", PanelTone::warn);
            }
        }
        // 3X follows the tray at once, so it is the tray's choice that is
        // compared here.
        if (asked.frames > running.frames && !(set.native && input.native_single_frame)) {
            if (input.shallow_fallback) {
                reason("The runtime refused a fourth swapchain: 2X", PanelTone::warn);
            } else if (input.graphics == PanelGraphics::d3d11_interop) {
                reason("3X is not available through the D3D11 interop", PanelTone::warn);
            } else if (input.triple_fixed) {
                reason("3X needs a game restart: it started without Prefer FPS over latency",
                       PanelTone::warn);
            } else {
                reason("Switching to 3X", PanelTone::dim);
            }
        } else if (asked.frames < running.frames) {
            reason("Switching to 2X", PanelTone::dim);
        }
        if (set.deep && !running.deep && running.frames <= 2 && input.shallow_fallback) {
            reason("The runtime refused a fourth swapchain: shallow pipeline", PanelTone::warn);
        }
        // Everything else the tray changes is read when the game starts.
        PanelMethod pending = input.asked;
        pending.frames = input.session.frames;
        pending = normalized(pending);
        const auto same = [](const PanelMethod& a, const PanelMethod& b) {
            return a.native == b.native &&
                (!a.native || (a.native_scale == b.native_scale && a.native_camera == b.native_camera)) &&
                a.flow == b.flow && a.preset == b.preset && a.flow_scale == b.flow_scale &&
                a.both_ways == b.both_ways && a.game_vectors == b.game_vectors &&
                a.hybrid == b.hybrid && a.extrapolate == b.extrapolate &&
                (a.extrapolate == 0 || a.mesh == b.mesh) && a.deep == b.deep;
        };
        if (!same(pending, set)) {
            reason("The tray's newer choice starts with the next game start", PanelTone::dim);
        }
    }

    int worst = 0;
    for (const PanelLine& line : reasons) worst = std::max(worst, severity(line.tone));
    if (input.paused) {
        text.state = "PAUSED";
        text.state_tone = PanelTone::warn;
    } else if (!input.enabled || input.budget_exhausted) {
        text.state = "OFF";
        text.state_tone = PanelTone::bad;
    } else if (engine == Engine::none) {
        text.state = "NOT GENERATING";
        text.state_tone = PanelTone::bad;
    } else if (worst > 0) {
        text.state = "FALLBACK";
        text.state_tone = PanelTone::warn;
    } else {
        text.state = "GENERATING";
        text.state_tone = PanelTone::good;
    }

    auto& lines = text.lines;
    lines.push_back({"Asked", describe_asked(input.asked), PanelTone::normal});
    lines.push_back({"Running", describe_running(input),
                     engine == Engine::none ? PanelTone::bad
                         : worst > 0        ? PanelTone::warn
                                            : PanelTone::good});
    for (std::size_t index = 0; index < reasons.size(); ++index) {
        reasons[index].label = index == 0 ? "Why" : "";
        lines.push_back(std::move(reasons[index]));
    }
    if (set.native || set.game_vectors) {
        const PanelVectors& vectors = input.vectors;
        const std::uint64_t refused = vectors.temporal_rejections + vectors.invalid_rejections;
        PanelLine line{set.native ? "Guides" : "Vectors", {}, PanelTone::dim};
        if (vectors.used > 0) {
            const double share = static_cast<double>(vectors.used) /
                static_cast<double>(vectors.used + refused);
            line.text = "used on " + format("%.0f", share * 100.0) + "% of pairs";
            if (refused > 0) line.text += ", " + std::to_string(refused) + " refused";
            if (set.native && vectors.native_resets > 0) {
                const double reset = std::min(1.0, static_cast<double>(vectors.native_resets) /
                                                       static_cast<double>(vectors.used));
                line.text += ", NGX reset on " + format("%.0f", reset * 100.0) + "%";
            }
            line.tone = share >= 0.9 ? PanelTone::good : PanelTone::warn;
        } else {
            line.text = vector_status_text(vectors);
            line.tone = refused > 0 ? PanelTone::warn
                : vectors_in_use(vectors) ? PanelTone::good
                                          : PanelTone::dim;
        }
        lines.push_back(std::move(line));
    }
    lines.push_back({"Pipeline",
                     std::string(running.deep ? "deep" : "shallow") +
                         (input.presenter ? ", presenter thread" : ", on the game's thread"),
                     PanelTone::normal});
    {
        PanelLine line{"Latency", "none added", PanelTone::dim};
        // A native pair NGX skipped still goes out, the real frame's copy
        // where the generated frame would be, so the real frame is held as
        // long as when it is generated.
        if (engine != Engine::none || (input.running.native && input.rates.generating)) {
            // The README's account: interpolation shows each real frame a
            // display frame later (two at 3X), the deeper pipeline one more,
            // extrapolation none.
            const std::uint32_t frames =
                (running.extrapolate != 0 ? 0U : running.frames - 1U) + (running.deep ? 1U : 0U);
            if (frames > 0) {
                line.text = "+" + std::to_string(frames) + (frames == 1 ? " frame" : " frames");
                if (input.rates.refresh_hz > 0.0F) {
                    line.text += " (" + whole(1000.0F * static_cast<float>(frames) /
                                              input.rates.refresh_hz) + " ms)";
                }
                line.text += " added";
                line.tone = PanelTone::normal;
            }
            if (input.promise_periods > 0) {
                line.text += ", promise +" + std::to_string(input.promise_periods);
            }
        }
        lines.push_back(std::move(line));
    }
    if (input.gpu_ms >= 0.0F) {
        lines.push_back({"GPU", format("%.2f ms a pair", input.gpu_ms), PanelTone::normal});
    } else {
        lines.push_back({"GPU",
                         input.gpu_timing ? "measuring" : "shown while the flight recorder is on",
                         PanelTone::dim});
    }
    {
        std::string frames = input.rates.repeats >= 0.05F
            ? format("%.1f repeats/s", input.rates.repeats)
            : std::string("no repeats");
        if (input.rates.delivered >= 0.0F) {
            frames += ", headset got " + format("%.1f/s", input.rates.delivered);
        }
        lines.push_back({"Frames", std::move(frames), PanelTone::normal});
    }
    // A game short of what the frames need: each real frame then shows for
    // an uneven number of refreshes, which reads as judder or doubling
    // whatever the generated frames look like. Not a fallback, so it is a
    // line of its own rather than a reason.
    if (input.rates.generating && input.rates.refresh_hz > 0.0F && input.rates.game > 0.0F &&
        running.frames >= 2) {
        const float needed = input.rates.refresh_hz / static_cast<float>(running.frames);
        if (input.rates.game < needed * 0.9F) {
            const bool triple = running.frames == 2 && !(running.native && input.native_single_frame);
            lines.push_back({"Pace",
                             "game " + whole(input.rates.game) + "/s is under the " + whole(needed) +
                                 " that " + std::to_string(running.frames) + "X needs at " +
                                 whole(input.rates.refresh_hz) + " Hz, so frames repeat: try " +
                                 (triple ? "3X, Automatic, or a lower refresh rate"
                                         : "a lower refresh rate"),
                             PanelTone::warn});
        }
    }
    {
        std::string session = graphics_text(input.graphics);
        if (!input.runtime.empty()) {
            session += " on " + input.runtime.substr(0, 32);
        }
        lines.push_back({"Session", std::move(session), PanelTone::dim});
    }
    // The settings, for the controls on the controller the panel sits on.
    if (input.controls_bound) {
        const auto editables = panel_editables(input);
        const bool shown = cursor != nullptr && cursor->row >= 0;
        lines.push_back({"Settings",
                         shown ? "stick: choose and change, trigger: apply"
                               : "turn the stick to choose a setting",
                         PanelTone::dim});
        for (std::size_t index = 0; index < editables.size(); ++index) {
            const PanelEditable& row = editables[index];
            const int choices = static_cast<int>(row.choices.size());
            const bool highlighted = shown && cursor->row == static_cast<int>(index);
            const int staged = highlighted && cursor->staged >= 0 && cursor->staged < choices ? cursor->staged : -1;
            PanelLine line{row.label, {}, row.available ? PanelTone::normal : PanelTone::dim};
            const std::string current =
                choices > 0 ? row.choices[static_cast<std::size_t>(std::clamp(row.current, 0, choices - 1))]
                            : std::string("-");
            if (staged >= 0) {
                line.text = current + " > " + row.choices[static_cast<std::size_t>(staged)] + ", hold the trigger to apply";
                line.tone = PanelTone::accent;
            } else if (highlighted && row.available && choices > 1) {
                line.text = "< " + current + " >";
            } else {
                line.text = current;
            }
            if (!row.note.empty()) line.text += " (" + row.note + ")";
            line.highlighted = highlighted;
            lines.push_back(std::move(line));
        }
    }
    return text;
}

namespace {

constexpr int kGlyph = 8;

// Glyph masks at 1, 2 and 4 times the font's size. The larger two are grown
// by Scale2x (EPX): each pixel splits into four, and a corner takes the
// colour of the two neighbours that meet there when they agree. Diagonals
// come out as steps of the new size rather than blocks of the old, which
// reads markedly better at the size the panel is seen at than plain
// doubling, and it is all done once, here.
struct GlyphSet {
    int size{};
    std::vector<std::uint8_t> masks; // 128 glyphs, size x size each
};

[[nodiscard]] std::vector<std::uint8_t> scale2x(const std::vector<std::uint8_t>& source, int size) {
    std::vector<std::uint8_t> output(static_cast<std::size_t>(size) * size * 4);
    const auto at = [&](int x, int y) -> std::uint8_t {
        return x < 0 || y < 0 || x >= size || y >= size ? 0 : source[static_cast<std::size_t>(y) * size + x];
    };
    const int out_size = size * 2;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::uint8_t p = at(x, y), a = at(x, y - 1), b = at(x + 1, y),
                               c = at(x - 1, y), d = at(x, y + 1);
            std::uint8_t e0 = p, e1 = p, e2 = p, e3 = p;
            if (c == a && c != d && a != b) e0 = a;
            if (a == b && a != c && b != d) e1 = b;
            if (d == c && d != b && c != a) e2 = c;
            if (b == d && b != a && d != c) e3 = d;
            const std::size_t top = static_cast<std::size_t>(y * 2) * out_size + x * 2;
            output[top] = e0;
            output[top + 1] = e1;
            output[top + out_size] = e2;
            output[top + out_size + 1] = e3;
        }
    }
    return output;
}

[[nodiscard]] const GlyphSet& glyphs(int scale) {
    static std::array<GlyphSet, 3> sets;
    static std::once_flag once;
    std::call_once(once, [] {
        for (std::size_t level = 0; level < sets.size(); ++level) {
            GlyphSet& set = sets[level];
            set.size = kGlyph << level;
            set.masks.resize(static_cast<std::size_t>(set.size) * set.size * 128);
        }
        for (int code = 0; code < 128; ++code) {
            std::vector<std::uint8_t> mask(kGlyph * kGlyph);
            for (int y = 0; y < kGlyph; ++y)
                for (int x = 0; x < kGlyph; ++x)
                    mask[static_cast<std::size_t>(y) * kGlyph + x] =
                        (font8x8_basic[code][y] >> x) & 1U;
            int size = kGlyph;
            for (std::size_t level = 0; level < sets.size(); ++level) {
                if (level > 0) {
                    mask = scale2x(mask, size);
                    size *= 2;
                }
                std::copy(mask.begin(), mask.end(),
                          sets[level].masks.begin() + static_cast<std::ptrdiff_t>(code) * size * size);
            }
        }
    });
    return sets[scale >= 4 ? 2 : scale >= 2 ? 1 : 0];
}

struct Rgba {
    std::uint8_t r, g, b, a;
};

// Colours as they should look, in sRGB.
constexpr Rgba kBackground{14, 18, 28, 232};
constexpr Rgba kBorder{64, 74, 98, 255};
constexpr Rgba kRule{48, 56, 76, 255};
constexpr Rgba kHighlight{40, 54, 86, 255};
constexpr Rgba kTones[]{
    {238, 241, 247, 255}, // normal
    {148, 158, 178, 255}, // dim
    {92, 224, 142, 255},  // good
    {255, 192, 72, 255},  // warn
    {255, 98, 98, 255},   // bad
    {112, 202, 255, 255}, // accent
};

class Canvas {
public:
    Canvas(std::uint32_t width, std::uint32_t height, bool bgra, bool linear)
        : width_(static_cast<int>(width)), height_(static_cast<int>(height)), bgra_(bgra),
          linear_(linear), pixels_(static_cast<std::size_t>(width) * height, 0) {}

    [[nodiscard]] std::uint32_t pack(Rgba color, float coverage = 1.0F) const noexcept {
        const auto channel = [&](std::uint8_t value) {
            float c = static_cast<float>(value) / 255.0F;
            if (linear_) c = c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
            return c;
        };
        const float alpha = static_cast<float>(color.a) / 255.0F * std::clamp(coverage, 0.0F, 1.0F);
        const auto byte = [](float value) {
            return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
        };
        // Premultiplied: the quad blends source alpha without the
        // unpremultiplied flag, as the counter's does.
        const std::uint32_t r = byte(channel(color.r) * alpha);
        const std::uint32_t g = byte(channel(color.g) * alpha);
        const std::uint32_t b = byte(channel(color.b) * alpha);
        const std::uint32_t a = byte(alpha);
        return (a << 24) | (bgra_ ? b | (g << 8) | (r << 16) : r | (g << 8) | (b << 16));
    }

    // A rounded rectangle over the top `used` rows of the canvas with a thin
    // border, its corners softened by coverage so the edge does not
    // stair-step; the rows below stay transparent. Only the corners need the
    // arithmetic, which matters on the game's render thread.
    void panel(int used, int radius, int border) {
        used = std::clamp(used, 2 * radius + 2, height_);
        const std::uint32_t fill = pack(kBackground);
        const std::uint32_t edge = pack(kBorder);
        std::fill(pixels_.begin(), pixels_.begin() + static_cast<std::ptrdiff_t>(used) * width_, fill);
        for (int y = 0; y < used; ++y) {
            const auto row = pixels_.begin() + static_cast<std::ptrdiff_t>(y) * width_;
            if (y < border || y >= used - border) {
                std::fill(row, row + width_, edge);
            } else {
                std::fill(row, row + border, edge);
                std::fill(row + width_ - border, row + width_, edge);
            }
        }
        for (int y = 0; y <= radius; ++y) {
            for (int x = 0; x <= radius; ++x) {
                const float dx = static_cast<float>(radius - x) - 0.5F;
                const float dy = static_cast<float>(radius - y) - 0.5F;
                const float outside = std::sqrt(std::max(dx, 0.0F) * std::max(dx, 0.0F) +
                                                std::max(dy, 0.0F) * std::max(dy, 0.0F)) -
                    static_cast<float>(radius);
                const float coverage = std::clamp(0.5F - outside, 0.0F, 1.0F);
                const bool on_edge = outside > -static_cast<float>(border);
                const std::uint32_t value = coverage <= 0.0F ? 0U
                    : coverage < 1.0F                        ? pack(on_edge ? kBorder : kBackground, coverage)
                    : on_edge                                ? edge
                                                             : fill;
                for (const auto& [cx, cy] : {std::pair{x, y}, std::pair{width_ - 1 - x, y},
                                             std::pair{x, used - 1 - y},
                                             std::pair{width_ - 1 - x, used - 1 - y}}) {
                    pixels_[static_cast<std::size_t>(cy) * width_ + cx] = value;
                }
            }
        }
    }

    // The bar behind the setting the cursor stands on.
    void bar(int x0, int x1, int y0, int y1) {
        const std::uint32_t fill = pack(kHighlight);
        for (int y = std::max(y0, 0); y < std::min(y1, height_); ++y) {
            const auto row = pixels_.begin() + static_cast<std::ptrdiff_t>(y) * width_;
            std::fill(row + std::max(x0, 0), row + std::min(x1, width_), fill);
        }
    }
    void rule(int x0, int x1, int y, int thickness) {
        const std::uint32_t color = pack(kRule);
        for (int row = y; row < y + thickness && row < height_; ++row)
            for (int x = std::max(x0, 0); x < std::min(x1, width_); ++x)
                pixels_[static_cast<std::size_t>(row) * width_ + x] = color;
    }

    // Draws ASCII text with its top-left corner at (x, y), clipped to the
    // canvas; anything outside printable ASCII draws as '?'. Returns where
    // the next character would go.
    int text(int x, int y, std::string_view value, int scale, PanelTone tone) {
        const GlyphSet& set = glyphs(scale);
        const std::uint32_t color = pack(kTones[static_cast<int>(tone)]);
        for (char raw : value) {
            const auto code = static_cast<unsigned char>(raw);
            const int glyph = code >= 32 && code < 127 ? code : '?';
            const std::uint8_t* mask =
                set.masks.data() + static_cast<std::size_t>(glyph) * set.size * set.size;
            for (int row = 0; row < set.size; ++row) {
                const int py = y + row;
                if (py < 0 || py >= height_) continue;
                for (int column = 0; column < set.size; ++column) {
                    const int px = x + column;
                    if (px < 0 || px >= width_ || !mask[row * set.size + column]) continue;
                    pixels_[static_cast<std::size_t>(py) * width_ + px] = color;
                }
            }
            x += set.size;
        }
        return x;
    }

    std::vector<std::uint32_t> take() { return std::move(pixels_); }

private:
    int width_, height_;
    bool bgra_, linear_;
    std::vector<std::uint32_t> pixels_;
};

// Breaks a line at spaces so each piece is at most `columns` characters; a
// word longer than that is cut.
[[nodiscard]] std::vector<std::string> wrap(std::string_view text, std::size_t columns) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(' ', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view word = text.substr(start, end - start);
        while (word.size() > columns) {
            if (!current.empty()) {
                lines.push_back(std::move(current));
                current.clear();
            }
            lines.emplace_back(word.substr(0, columns));
            word.remove_prefix(columns);
        }
        if (!current.empty() && current.size() + 1 + word.size() > columns) {
            lines.push_back(std::move(current));
            current.clear();
        }
        if (!current.empty()) current += ' ';
        current += word;
        start = end + 1;
    }
    if (!current.empty() || lines.empty()) lines.push_back(std::move(current));
    return lines;
}

}  // namespace

std::vector<std::uint32_t> rasterize_status_panel(
    std::uint32_t width,
    std::uint32_t height,
    const StatusPanelInput& input,
    bool bgra,
    bool linear,
    std::uint32_t* used_height,
    const PanelCursor* cursor) {
    // Laid out in units of a 512-wide panel: the body text is one font size
    // to a unit, the four big numbers two.
    if (width < 512 || width > 2048 || height < width / 2 || height > width) return {};
    const int unit = static_cast<int>(width / 512);
    const int body = unit;
    const int big = unit * 2;
    const int glyph = kGlyph * body;
    const int margin = 12 * unit;
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    const StatusPanelText text = status_panel_text(input, cursor);

    // The rows come first, so the panel can be cut to them: a label column,
    // then text wrapped to the panel's width, as many as fit.
    struct Row {
        const PanelLine* line;
        std::string text;
        bool first;
    };
    const int value_x = margin + 10 * glyph;
    const auto value_columns = static_cast<std::size_t>(std::max((w - margin - value_x) / glyph, 8));
    const int line_height = 11 * unit;
    const int rows_top = 68 * unit;
    const int bottom_margin = 4 * unit;
    std::vector<Row> rows;
    for (const PanelLine& line : text.lines) {
        auto pieces = wrap(line.text, value_columns);
        for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
            if (rows_top + static_cast<int>(rows.size() + 1) * line_height + bottom_margin > h) break;
            rows.push_back({&line, std::move(pieces[piece]), piece == 0});
        }
    }
    const int used = std::min(h, rows_top + static_cast<int>(rows.size()) * line_height + bottom_margin);
    if (used_height != nullptr) *used_height = static_cast<std::uint32_t>(used);

    Canvas canvas(width, height, bgra, linear);
    canvas.panel(used, 8 * unit, unit);
    canvas.text(margin, 9 * unit, "OFXR Bridge", body, PanelTone::accent);
    canvas.text(w - margin - glyph * static_cast<int>(text.state.size()), 9 * unit, text.state,
                body, text.state_tone);
    canvas.rule(margin, w - margin, 25 * unit, unit);

    // The four numbers that matter most, large enough to read at a glance.
    const PanelRates& rates = input.rates;
    PanelTone shown_tone = PanelTone::normal;
    if (rates.refresh_hz > 0.0F) {
        shown_tone = rates.shown >= rates.refresh_hz * 0.97F ? PanelTone::good
            : rates.shown >= rates.refresh_hz * 0.85F        ? PanelTone::warn
                                                             : PanelTone::bad;
    }
    const struct {
        std::string value;
        const char* label;
        PanelTone tone;
    } numbers[]{
        {whole(rates.shown), "shown/s", shown_tone},
        {whole(rates.game), "game/s", PanelTone::normal},
        {whole(rates.generated), "generated/s", PanelTone::normal},
        {rates.refresh_hz > 0.0F ? whole(rates.refresh_hz) : std::string("-"), "Hz display",
         PanelTone::dim},
    };
    const int column = (w - 2 * margin) / 4;
    for (int index = 0; index < 4; ++index) {
        const auto& number = numbers[index];
        const int centre = margin + column * index + column / 2;
        canvas.text(centre - kGlyph * big * static_cast<int>(number.value.size()) / 2, 31 * unit,
                    number.value, big, number.tone);
        canvas.text(centre - glyph * static_cast<int>(std::string_view(number.label).size()) / 2,
                    50 * unit, number.label, body, PanelTone::dim);
    }
    canvas.rule(margin, w - margin, 62 * unit, unit);

    int y = rows_top;
    for (const Row& row : rows) {
        if (row.line->highlighted) canvas.bar(margin / 2, w - margin / 2, y - unit, y + line_height - 2 * unit);
        if (row.first) canvas.text(margin, y, row.line->label, body, PanelTone::dim);
        canvas.text(value_x, y, row.text, body, row.line->tone);
        y += line_height;
    }
    return canvas.take();
}

}  // namespace xrfg
