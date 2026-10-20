#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace horror {

// "Who wrote it last" tags. A camera snap is usually somebody overwriting the
// camera outside the normal input path; recording the last writer turns a
// mystery dump into a one-line answer. Literal strings only (stored as
// pointers, never copied).
const char *lastCameraWriter();
void noteCameraWriter(const char *who);
const char *lastPlayerWriter();
void notePlayerWriter(const char *who);

// Hotkeys that fired on a frame, recorded so a dump shows what the player
// pressed immediately before a glitch.
enum HotkeyBits : uint32_t {
    kHotkeyNone = 0,
    kHotkeyUI = 1u << 0,
    kHotkeyWireframe = 1u << 1,
    kHotkeyDebugDraw = 1u << 2,
    kHotkeyVsync = 1u << 3,
    kHotkeyQuit = 1u << 4,
    kHotkeyDump = 1u << 5,
    kHotkeyPatrol = 1u << 6,
};
void noteHotkey(uint32_t bit);       // accumulates into the current frame
uint32_t takeHotkeyBits();           // hands them over and clears
std::string hotkeyList(uint32_t mask);

// Per-fixed-step condition bits. Not exhaustive: only the things that can move
// the camera or the body discontinuously.
enum StepFlags : uint32_t {
    kStepNone = 0,
    kStepLanded = 1u << 0,     // Y resolution this step turned airborne -> grounded
    kStepJumped = 1u << 1,     // jump impulse applied
    kStepSteppedUp = 1u << 2,  // step-offset lifted the body onto a ledge
    kStepPushOut = 1u << 3,    // collision push-out moved the body on X/Z
    kStepGroundProbe = 1u << 4,// walk-off-edge probe changed pos.y
    kStepBobRestart = 1u << 5, // head bob went from damped to driven
    kStepSubstepped = 1u << 6, // more than one collision pass
    kStepTeleport = 1u << 7,   // explicit reset/respawn before this step
    kStepLookRejected = 1u << 8, // mouse look rejected as a warp/spike
};

struct StepTrace {
    uint64_t seq = 0;
    float dt = 0.0f;
    glm::vec3 pos{0.0f};
    glm::vec3 vel{0.0f};
    glm::vec3 push{0.0f};
    glm::vec3 camPos{0.0f};
    glm::vec3 bobOffset{0.0f};
    float eyeHeight = 0.0f; // feet -> eye (crouch lowers it; smoothing jumps show here)
    float camYaw = 0.0f;
    float camPitch = 0.0f;
    float bobRoll = 0.0f;
    glm::vec2 rawMouse{0.0f};   // consumed this step, before sensitivity
    glm::vec2 look{0.0f};       // applied yaw/pitch delta (radians, after sensitivity)
    float forwardAxis = 0.0f;
    float strafeAxis = 0.0f;
    bool grounded = false;
    bool sprint = false;
    bool crouch = false;
    bool mouseCaptured = false;
    bool windowFocused = true;
    bool imguiMouse = false;
    uint32_t flags = 0;
    const char *cameraWriter = "none";
    const char *playerWriter = "none";
};

struct FrameTrace {
    uint64_t seq = 0;
    float dt = 0.0f;
    int steps = 0;              // fixed steps run during this frame
    glm::vec3 camPos{0.0f};
    float camYaw = 0.0f;
    float camPitch = 0.0f;
    glm::vec2 pendingMouse{0.0f};   // accumulated after the steps consumed theirs
    bool mouseCaptured = false;
    bool windowFocused = true;
    bool imguiMouse = false;
    bool imguiKeyboard = false;
    uint32_t hotkeys = 0;
    const char *cameraWriter = "none";
};

// Always-on, cheap flight recorder: the last few seconds of fixed steps and
// frames in ring buffers, plus automatic detection of camera discontinuities.
//
// Deliberately silent in the steady state -- no logging, no allocation after
// construction. An anomaly logs one WARNING and writes the surrounding buffer
// to a dump file; F9 dumps on demand. This exists because the interesting
// bugs (camera snaps, invisible teleports) are not reproducible on demand.
class FlightRecorder {
public:
    // ~7 s at 60 Hz for steps, ~7 s of frames at 60 fps. Both are fixed
    // capacity; the oldest record is overwritten.
    static constexpr size_t kCapacity = 420;

    static FlightRecorder &get();

    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    void recordStep(const StepTrace &step);
    void recordFrame(const FrameTrace &frame);

    // Gameplay just moved the player/camera on purpose (spawn, respawn,
    // resetCharacter). The next step is marked kStepTeleport and exempt from
    // anomaly detection -- an expected jump must not look like the bug.
    void noteTeleport(const char *why) { pendingTeleport_ = why ? why : "teleport"; }

    // Something was about to move the camera that cannot be player intent (a
    // look spike in one step, a warp that slipped through). Logs one warning
    // and dumps, rate limited like the anomaly path.
    void noteSpuriousInput(const char *why);

    // Writes the whole ring buffer (oldest first) plus a header to `path`.
    // Returns false when the file could not be opened.
    bool dump(const std::string &path, const std::string &reason);

    int autoDumpCount() const { return autoDumps_; }
    int anomalyCount() const { return anomalies_; }
    uint64_t stepsRecorded() const { return stepSeq_; }
    uint64_t framesRecorded() const { return frameSeq_; }

private:
    FlightRecorder() = default;

    // Camera jump detection. Returns a bitmask of AnomalyKind describing what
    // the step did that its own input/velocity cannot explain.
    uint32_t detectStepAnomaly(const StepTrace &current);
    void onAnomaly(uint32_t kinds, const StepTrace &current);

    std::vector<StepTrace> steps_ = std::vector<StepTrace>(kCapacity);
    std::vector<FrameTrace> frames_ = std::vector<FrameTrace>(kCapacity);
    size_t stepHead_ = 0;   // next write slot
    size_t frameHead_ = 0;
    uint64_t stepSeq_ = 0;
    uint64_t frameSeq_ = 0;
    int anomalies_ = 0;
    int autoDumps_ = 0;
    // Starts past the cooldown so the *first* anomaly always dumps; the
    // cooldown only throttles a repeating one (a snap that fires every step
    // would otherwise write a file per step).
    float sinceAutoDump_ = 1.0f;
    bool enabled_ = true;
    const char *pendingTeleport_ = nullptr;
};

} // namespace horror
