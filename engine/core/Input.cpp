#include "core/Input.hpp"

#include "core/Window.hpp"

#include <stdexcept>

namespace horror {

namespace {
constexpr int kUnknown = GLFW_KEY_UNKNOWN;
} // namespace

// Frames to keep discarding cursor samples after a capture/focus change (~130
// ms at 60 fps). Long enough to cover the asynchronous WM warp that produced
// the 205 px fake delta in the recorded camera snap, short enough to be
// unnoticeable when you press F1.
constexpr int Input::kResyncFrames;

Input::Input()
{
    for (auto &k : keys_)
        k = { false, false };
    for (auto &b : buttons_)
        b = { false, false };

    bindings_[(size_t)Action::MoveForward] = { GLFW_KEY_W, GLFW_KEY_UP };
    bindings_[(size_t)Action::MoveBackward] = { GLFW_KEY_S, GLFW_KEY_DOWN };
    bindings_[(size_t)Action::MoveLeft] = { GLFW_KEY_A, GLFW_KEY_LEFT };
    bindings_[(size_t)Action::MoveRight] = { GLFW_KEY_D, GLFW_KEY_RIGHT };
    bindings_[(size_t)Action::Sprint] = { GLFW_KEY_LEFT_SHIFT };
    bindings_[(size_t)Action::Crouch] = { GLFW_KEY_LEFT_CONTROL, GLFW_KEY_C };
    bindings_[(size_t)Action::Jump] = { GLFW_KEY_SPACE };
    bindings_[(size_t)Action::Interact] = { GLFW_KEY_E };
    bindings_[(size_t)Action::ToggleDebugUI] = { GLFW_KEY_F1 };
    bindings_[(size_t)Action::ToggleWireframe] = { GLFW_KEY_P };
    bindings_[(size_t)Action::ToggleDebugDraw] = { GLFW_KEY_O };
    bindings_[(size_t)Action::ToggleVsync] = { GLFW_KEY_V };
    bindings_[(size_t)Action::DumpRecorder] = { GLFW_KEY_F9 };
    bindings_[(size_t)Action::TogglePatrol] = { GLFW_KEY_M };
    bindings_[(size_t)Action::Quit] = { GLFW_KEY_ESCAPE };
}

void Input::attach(Window &window)
{
    window_ = &window;
    glfwWindow_ = window.handle();
    active_ = this;
    glfwSetKeyCallback(glfwWindow_, keyCallback);
    glfwSetMouseButtonCallback(glfwWindow_, mouseButtonCallback);
    glfwSetCursorPosCallback(glfwWindow_, cursorPosCallback);
    glfwSetScrollCallback(glfwWindow_, scrollCallback);
    glfwSetWindowFocusCallback(glfwWindow_, focusCallback);
    focused_ = glfwGetWindowAttrib(glfwWindow_, GLFW_FOCUSED) != 0;
}

void Input::detach()
{
    if (!glfwWindow_)
        return;
    glfwSetKeyCallback(glfwWindow_, nullptr);
    glfwSetMouseButtonCallback(glfwWindow_, nullptr);
    glfwSetCursorPosCallback(glfwWindow_, nullptr);
    glfwSetScrollCallback(glfwWindow_, nullptr);
    glfwSetWindowFocusCallback(glfwWindow_, nullptr);
    if (active_ == this)
        active_ = nullptr;
    glfwWindow_ = nullptr;
    window_ = nullptr;
}

void Input::keyCallback(GLFWwindow *, int key, int /*scancode*/, int action, int /*mods*/)
{
    Input *self = active_;
    if (!self || key < 0 || key > GLFW_KEY_LAST)
        return;
    if (action == GLFW_PRESS)
        self->keys_[key].isDown = true;
    else if (action == GLFW_RELEASE)
        self->keys_[key].isDown = false;
}

void Input::mouseButtonCallback(GLFWwindow *, int button, int action, int /*mods*/)
{
    Input *self = active_;
    if (!self || button < 0 || button > GLFW_MOUSE_BUTTON_LAST)
        return;
    if (action == GLFW_PRESS)
        self->buttons_[button].isDown = true;
    else if (action == GLFW_RELEASE)
        self->buttons_[button].isDown = false;
}

void Input::focusCallback(GLFWwindow *, int focused)
{
    Input *self = active_;
    if (!self)
        return;
    const bool nowFocused = focused != 0;
    if (nowFocused == self->focused_)
        return;
    self->focused_ = nowFocused;
    // Both directions need a re-baseline: on the way out the pending delta
    // belongs to the free-cursor era, and on the way back the pointer may sit
    // anywhere (the WM moved/warped it while we were unfocused).
    self->resyncMouse(nowFocused ? "focus regained" : "focus lost");
}

void Input::resyncMouse(const char *why)
{
    const glm::vec2 pending = mouseDelta_;
    mouseDelta_ = glm::vec2(0.0f);
    firstMouseSample_ = true;
    resyncSamples_ = 2;
    // Also hold the window open for a few frames: the window manager moves the
    // pointer *after* the mode change, sometimes several frames later. Swallow
    // samples until both limits expire.
    resyncFrames_ = kResyncFrames;
    ++resyncEvents_;
    if (glm::length(pending) > 20.0f)
        HOR_LOG_DEBUG("mouse re-baseline ({}): dropped pending delta ({:+.1f},{:+.1f})",
            why, pending.x, pending.y);
}

void Input::cursorPosCallback(GLFWwindow *, double x, double y)
{
    if (active_)
        active_->handleCursorPos(glm::vec2(static_cast<float>(x), static_cast<float>(y)));
}

void Input::simulateCursorMove(const glm::vec2 &position)
{
    handleCursorPos(position);
}

void Input::handleCursorPos(const glm::vec2 &pos)
{
    const glm::vec2 delta = pos - mousePos_;
    mousePos_ = pos;

    if (firstMouseSample_ || resyncSamples_ > 0 || resyncFrames_ > 0) {
        // The first samples after a capture/focus change are not movement: the
        // cursor may have been warped from anywhere to the window/capture
        // position. Swallow a couple of samples so the warp and the position
        // event that follows it both fall inside the window -- swallowing only
        // the first one let the second through as a huge delta, which is what
        // snapped the camera to a fixed orientation (pitch clamped at +/-89).
        if (resyncSamples_ > 0)
            --resyncSamples_;
        firstMouseSample_ = false;
        return;
    }

    // Accumulate only captured motion: free-cursor movement is UI navigation.
    if (!mouseCaptured_)
        return;

    // A single event can never be a real mouse movement at hundreds of pixels;
    // it is a warp (cursor-mode change, WM recentre, virtual desktop switch).
    // Applying it rotates the camera by 30-180 degrees in one step.
    constexpr float kMaxEventDelta = 500.0f;
    if (glm::length(delta) > kMaxEventDelta) {
        if (warpEvents_ < 8) {
            HOR_LOG_WARN(
                "mouse warp dropped: event delta ({:+.1f},{:+.1f}) px, captured={} focused={} raw={}",
                delta.x, delta.y, mouseCaptured_ ? 1 : 0, focused_ ? 1 : 0,
                rawMouseMotion_ ? 1 : 0);
        }
        ++warpEvents_;
        return;
    }

    // Several motion events can land in one frame; a per-event overwrite
    // silently drops pixels (jittery, light look).
    mouseDelta_ += delta;
}

void Input::scrollCallback(GLFWwindow *, double dx, double dy)
{
    if (active_)
        active_->scrollDelta_ += glm::vec2(float(dx), float(dy));
}

void Input::update()
{
    // Roll this frame into "previous frame" for edge detection. MUST run
    // before glfwPollEvents: running it after the callbacks would snapshot the
    // brand-new state into wasDown, which made pressed()/released() -- and
    // therefore jump, F1, Esc and every other edge action -- permanently
    // false.
    for (auto &k : keys_)
        k.wasDown = k.isDown;
    for (auto &b : buttons_)
        b.wasDown = b.isDown;
    scrollDelta_ = glm::vec2(0.0f);
    if (resyncFrames_ > 0)
        --resyncFrames_;

    // Focus backstop: the callback covers every real transition, but sampling
    // here also catches a window that started unfocused (or a platform that
    // never delivered the callback).
    if (glfwWindow_) {
        const bool focused = glfwGetWindowAttrib(glfwWindow_, GLFW_FOCUSED) != 0;
        if (focused != focused_) {
            focused_ = focused;
            resyncMouse(focused ? "focus regained (sampled)" : "focus lost (sampled)");
        }
    }
}

glm::vec2 Input::consumeMouseDelta()
{
    const glm::vec2 delta = mouseDelta_;
    mouseDelta_ = glm::vec2(0.0f);
    // Free cursor = UI navigation, not mouselook (otherwise opening the F1
    // overlay and moving the mouse turns the camera behind it). ImGui owning
    // the pointer counts as free too. No window at all = scripted harness,
    // which injects deltas deliberately.
    if (mouseWantedByImGui || (glfwWindow_ && !mouseCaptured_))
        return glm::vec2(0.0f);
    return delta;
}

void Input::setScriptedKey(int key, bool down)
{
    if (key < 0 || key > GLFW_KEY_LAST)
        return;
    keys_[key].isDown = down;
}

void Input::injectMouseDelta(const glm::vec2 &pixels)
{
    mouseDelta_ += pixels;
}

bool Input::held(Action a) const
{
    const auto &map = bindings_[(size_t)a];
    for (int key : map) {
        if (key == kUnknown)
            continue;
        if (keyHeld(key))
            return true;
    }
    return false;
}

bool Input::pressed(Action a) const
{
    const auto &map = bindings_[(size_t)a];
    for (int key : map) {
        if (key == kUnknown)
            continue;
        if (keyPressed(key))
            return true;
    }
    return false;
}

bool Input::released(Action a) const
{
    const auto &map = bindings_[(size_t)a];
    for (int key : map) {
        if (key == kUnknown)
            continue;
        if (keys_[key].wasDown && !keys_[key].isDown)
            return true;
    }
    return false;
}

bool Input::keyHeld(int key) const
{
    if (key < 0 || key > GLFW_KEY_LAST)
        return false;
    if (!mouseCaptured_ && keyboardWantedByImGui)
        return false; // overlay eats keyboard while visible + focused
    return keys_[key].isDown;
}

bool Input::keyPressed(int key) const
{
    if (key < 0 || key > GLFW_KEY_LAST)
        return false;
    if (!mouseCaptured_ && keyboardWantedByImGui)
        return false;
    return keys_[key].isDown && !keys_[key].wasDown;
}

void Input::setMouseCaptured(bool captured)
{
    if (captured == mouseCaptured_)
        return;
    mouseCaptured_ = captured;
    HOR_ASSERT(glfwWindow_);
    // Disabled = OS cursor hidden and locked to the window (classic FPS mode);
    // deltas still arrive through cursorPosCallback.
    glfwSetInputMode(glfwWindow_, GLFW_CURSOR,
        captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    // Raw motion bypasses cursor acceleration/clamping in the compositor, so
    // the look maps 1:1 to physical mouse movement (no acceleration). Only
    // valid while the cursor is disabled.
    rawMouseMotion_ = captured && glfwRawMouseMotionSupported();
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(glfwWindow_, GLFW_RAW_MOUSE_MOTION,
            rawMouseMotion_ ? GLFW_TRUE : GLFW_FALSE);
    if (rawMouseMotion_)
        HOR_LOG_DEBUG("raw mouse motion enabled");
    // Re-baseline: the cursor is warped to the window when the mode changes,
    // so the first samples after this are not movement, and any pending delta
    // belongs to the free-cursor era.
    resyncMouse(captured ? "capture on" : "capture off");
    resyncSamples_ = 3; // the warp lands after GLFW has re-enabled the cursor
}

const char *Input::actionName(Action a)
{
    switch (a) {
    case Action::MoveForward: return "MoveForward";
    case Action::MoveBackward: return "MoveBackward";
    case Action::MoveLeft: return "MoveLeft";
    case Action::MoveRight: return "MoveRight";
    case Action::Sprint: return "Sprint";
    case Action::Crouch: return "Crouch";
    case Action::Jump: return "Jump";
    case Action::Interact: return "Interact";
    case Action::ToggleDebugUI: return "ToggleDebugUI";
    case Action::ToggleWireframe: return "ToggleWireframe";
    case Action::ToggleDebugDraw: return "ToggleDebugDraw";
    case Action::ToggleVsync: return "ToggleVsync";
    case Action::DumpRecorder: return "DumpRecorder";
    case Action::TogglePatrol: return "TogglePatrol";
    case Action::Quit: return "Quit";
    default: return "Unknown Action";
    }
}

} // namespace horror
