#include "debug/FlightRecorder.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>

namespace horror {

namespace {

const char *gCameraWriter = "none";
const char *gPlayerWriter = "none";
uint32_t gPendingHotkeys = 0;

// A look jump this big in one fixed step cannot come from the mouse: the
// fastest plausible flick at the default sensitivity is a few degrees.
constexpr float kMaxUnexplainedYaw = 0.105f;   // ~6 degrees
constexpr float kMaxUnexplainedPitch = 0.105f; // ~6 degrees
// Camera travel beyond velocity*dt + bob that still counts as a "snap". Head
// bob is < 5 cm and a normal step travels < 8 cm, so 0.30 m is unreachable by
// walking and far below any teleport.
constexpr float kMaxUnexplainedCamMove = 0.30f;
constexpr float kBobAllowance = 0.06f;
// Auto-dumps are rate limited so one bad second cannot fill the disk.
constexpr float kAutoDumpCooldown = 0.5f;

enum AnomalyKind : uint32_t {
    kAnomalyYaw = 1u << 0,
    kAnomalyPitch = 1u << 1,
    kAnomalyCamMove = 1u << 2,
    kAnomalyBody = 1u << 3,
};

std::string anomalyName(uint32_t kinds)
{
    std::string out;
    const auto add = [&out, kinds](uint32_t bit, const char *name) {
        if (!(kinds & bit))
            return;
        if (!out.empty())
            out += '+';
        out += name;
    };
    add(kAnomalyYaw, "yaw-jump");
    add(kAnomalyPitch, "pitch-jump");
    add(kAnomalyCamMove, "camera-jump");
    add(kAnomalyBody, "body-teleport");
    return out.empty() ? "?" : out;
}

std::string stepFlagList(uint32_t flags)
{
    std::string out;
    const auto add = [&out, flags](uint32_t bit, const char *name) {
        if (!(flags & bit))
            return;
        if (!out.empty())
            out += '|';
        out += name;
    };
    add(kStepLanded, "LAND");
    add(kStepJumped, "JUMP");
    add(kStepSteppedUp, "STEP");
    add(kStepPushOut, "PUSH");
    add(kStepGroundProbe, "PROBE");
    add(kStepBobRestart, "BOB");
    add(kStepSubstepped, "SUB");
    add(kStepTeleport, "TELEPORT");
    return out.empty() ? "-" : out;
}

} // namespace

const char *lastCameraWriter() { return gCameraWriter; }
void noteCameraWriter(const char *who)
{
    if (who)
        gCameraWriter = who;
}

const char *lastPlayerWriter() { return gPlayerWriter; }
void notePlayerWriter(const char *who)
{
    if (who)
        gPlayerWriter = who;
}

void noteHotkey(uint32_t bit) { gPendingHotkeys |= bit; }
uint32_t takeHotkeyBits()
{
    const uint32_t bits = gPendingHotkeys;
    gPendingHotkeys = 0;
    return bits;
}

std::string hotkeyList(uint32_t mask)
{
    if (mask == 0)
        return "-";
    std::string out;
    const auto add = [&out, mask](uint32_t bit, const char *name) {
        if (!(mask & bit))
            return;
        if (!out.empty())
            out += '|';
        out += name;
    };
    add(kHotkeyUI, "F1");
    add(kHotkeyWireframe, "P");
    add(kHotkeyDebugDraw, "O");
    add(kHotkeyVsync, "V");
    add(kHotkeyQuit, "Esc");
    add(kHotkeyDump, "F9");
    add(kHotkeyPatrol, "M");
    return out;
}

FlightRecorder &FlightRecorder::get()
{
    static FlightRecorder instance;
    return instance;
}

void FlightRecorder::recordStep(const StepTrace &step)
{
    if (!enabled_)
        return;

    // A deliberate reset/spawn is not an anomaly, but it must be visible in the
    // dump: mark the step so the surrounding rows read correctly.
    const bool teleported = pendingTeleport_ != nullptr;
    if (teleported)
        pendingTeleport_ = nullptr;
    const uint32_t kinds = teleported ? 0 : detectStepAnomaly(step);
    sinceAutoDump_ += step.dt;

    StepTrace copy = step;
    if (teleported)
        copy.flags |= kStepTeleport;
    copy.seq = stepSeq_++;
    steps_[stepHead_] = copy;
    stepHead_ = (stepHead_ + 1) % kCapacity;

    if (kinds != 0)
        onAnomaly(kinds, copy);
}

void FlightRecorder::recordFrame(const FrameTrace &frame)
{
    if (!enabled_)
        return;
    FrameTrace copy = frame;
    copy.seq = frameSeq_++;
    copy.hotkeys = takeHotkeyBits();
    frames_[frameHead_] = copy;
    frameHead_ = (frameHead_ + 1) % kCapacity;
}

uint32_t FlightRecorder::detectStepAnomaly(const StepTrace &cur)
{
    // No predecessor recorded yet.
    if (stepSeq_ == 0)
        return 0;
    // The previous record is the slot before the write head.
    const StepTrace &prev = steps_[(stepHead_ + kCapacity - 1) % kCapacity];

    uint32_t kinds = 0;

    // --- orientation: what the mouse delta explains, nothing more -----------
    const float dYaw = cur.camYaw - prev.camYaw;
    const float dPitch = cur.camPitch - prev.camPitch;
    if (std::abs(dYaw - cur.look.x) > kMaxUnexplainedYaw)
        kinds |= kAnomalyYaw;
    if (std::abs(dPitch - cur.look.y) > kMaxUnexplainedPitch)
        kinds |= kAnomalyPitch;

    // --- camera position: velocity*dt plus head bob -------------------------
    const glm::vec3 camDelta = cur.camPos - prev.camPos;
    const float speed = std::max(glm::length(prev.vel), glm::length(cur.vel));
    const float explained = speed * cur.dt + kBobAllowance;
    if (glm::length(camDelta) > explained + kMaxUnexplainedCamMove)
        kinds |= kAnomalyCamMove;

    // --- body: a push-out or respawn that defies the velocity ---------------
    const glm::vec3 bodyDelta = cur.pos - prev.pos;
    const float bodyExplained = speed * cur.dt + 0.02f;
    if (glm::length(bodyDelta) > bodyExplained + kMaxUnexplainedCamMove)
        kinds |= kAnomalyBody;

    return kinds;
}

void FlightRecorder::onAnomaly(uint32_t kinds, const StepTrace &cur)
{
    ++anomalies_;
    const std::string names = anomalyName(kinds);
    HOR_LOG_WARN(
        "[recorder] {} at step {}: yaw={:.3f} (mouse {:.3f}) pitch={:.3f} (mouse {:.3f}) "
        "cam=({:.3f},{:.3f},{:.3f}) body=({:.3f},{:.3f},{:.3f}) grounded={} flags={} "
        "cap={} focused={} imgui={} camWriter={} playerWriter={}",
        names, cur.seq, cur.camYaw, cur.look.x, cur.camPitch, cur.look.y,
        cur.camPos.x, cur.camPos.y, cur.camPos.z, cur.pos.x, cur.pos.y, cur.pos.z,
        cur.grounded ? 1 : 0, stepFlagList(cur.flags), cur.mouseCaptured ? 1 : 0,
        cur.windowFocused ? 1 : 0, cur.imguiMouse ? 1 : 0, cur.cameraWriter,
        cur.playerWriter);

    if (sinceAutoDump_ < kAutoDumpCooldown)
        return;
    sinceAutoDump_ = 0.0f;
    if (dump("debug_dump.txt", std::format("auto: {}", names)))
        ++autoDumps_;
}

bool FlightRecorder::dump(const std::string &path, const std::string &reason)
{
    FILE *f = std::fopen(path.c_str(), "w");
    if (!f) {
        HOR_LOG_WARN("flight recorder: cannot open dump file '{}'", path);
        return false;
    }

    std::fprintf(f, "# horror-engine flight recorder dump\n");
    std::fprintf(f, "# reason: %s\n", reason.c_str());
    std::fprintf(f, "# steps=%llu frames=%llu anomalies=%d autoDumps=%d capacity=%zu\n",
        static_cast<unsigned long long>(stepSeq_),
        static_cast<unsigned long long>(frameSeq_), anomalies_, autoDumps_, kCapacity);
    std::fprintf(f, "# times are seconds (dt per record); look = applied yaw/pitch delta\n");

    std::fprintf(f, "\n# --- fixed steps (oldest first) ---\n");
    std::fprintf(f, "# seq t pos(x,y,z) vel(x,y,z) g flags yaw pitch look(dyaw,dpitch) "
                  "rawMouse(dx,dy) cam(x,y,z) bob(y,roll) push(x,y,z) cap foc gui camWriter playerWriter\n");
    const size_t stepCount = std::min<size_t>(stepSeq_, kCapacity);
    const size_t stepFirst = (stepHead_ + kCapacity - stepCount) % kCapacity;
    float t = 0.0f;
    for (size_t i = 0; i < stepCount; ++i) {
        const StepTrace &s = steps_[(stepFirst + i) % kCapacity];
        t += s.dt;
        std::fprintf(f,
            "  %6llu %8.3f (%.3f,%.3f,%.3f) (%.3f,%.3f,%.3f) %d %-18s "
            "yaw=%+.4f pitch=%+.4f look=(%+.4f,%+.4f) mouse=(%+.1f,%+.1f) "
            "cam=(%.3f,%.3f,%.3f) bob=(%+.4f,%+.4f) push=(%+.3f,%+.3f,%+.3f) "
            "cap=%d foc=%d gui=%d %s %s\n",
            static_cast<unsigned long long>(s.seq), t, s.pos.x, s.pos.y, s.pos.z,
            s.vel.x, s.vel.y, s.vel.z, s.grounded ? 1 : 0,
            stepFlagList(s.flags).c_str(), s.camYaw, s.camPitch, s.look.x, s.look.y,
            s.rawMouse.x, s.rawMouse.y, s.camPos.x, s.camPos.y, s.camPos.z,
            s.bobOffset.y, s.bobRoll, s.push.x, s.push.y, s.push.z,
            s.mouseCaptured ? 1 : 0, s.windowFocused ? 1 : 0, s.imguiMouse ? 1 : 0,
            s.cameraWriter, s.playerWriter);
    }

    std::fprintf(f, "\n# --- frames (oldest first) ---\n");
    std::fprintf(f, "# seq dt steps cam(x,y,z) yaw pitch pendingMouse(dx,dy) cap foc "
                  "imguiMouse imguiKeys hotkeys camWriter\n");
    const size_t frameCount = std::min<size_t>(frameSeq_, kCapacity);
    const size_t frameFirst = (frameHead_ + kCapacity - frameCount) % kCapacity;
    for (size_t i = 0; i < frameCount; ++i) {
        const FrameTrace &fr = frames_[(frameFirst + i) % kCapacity];
        std::fprintf(f,
            "  %6llu %7.4f %d (%.3f,%.3f,%.3f) yaw=%+.4f pitch=%+.4f "
            "pending=(%+.1f,%+.1f) cap=%d foc=%d guiMouse=%d guiKeys=%d hotkeys=%s %s\n",
            static_cast<unsigned long long>(fr.seq), fr.dt, fr.steps, fr.camPos.x,
            fr.camPos.y, fr.camPos.z, fr.camYaw, fr.camPitch, fr.pendingMouse.x,
            fr.pendingMouse.y, fr.mouseCaptured ? 1 : 0, fr.windowFocused ? 1 : 0,
            fr.imguiMouse ? 1 : 0, fr.imguiKeyboard ? 1 : 0,
            hotkeyList(fr.hotkeys).c_str(), fr.cameraWriter);
    }

    std::fclose(f);
    HOR_LOG_INFO("flight recorder: wrote {} ({} steps, {} frames) reason={}", path,
        stepCount, frameCount, reason);
    return true;
}

} // namespace horror
