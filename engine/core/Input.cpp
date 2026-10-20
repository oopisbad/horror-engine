#include "core/Input.hpp"

#include "core/Window.hpp"

#include <stdexcept>

namespace horror {

namespace {
constexpr int kUnknown = GLFW_KEY_UNKNOWN;
} // namespace

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
}

void Input::detach()
{
    if (!glfwWindow_)
        return;
    glfwSetKeyCallback(glfwWindow_, nullptr);
    glfwSetMouseButtonCallback(glfwWindow_, nullptr);
    glfwSetCursorPosCallback(glfwWindow_, nullptr);
    glfwSetScrollCallback(glfwWindow_, nullptr);
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

void Input::cursorPosCallback(GLFWwindow *, double x, double y)
{
    Input *self = active_;
    if (!self)
        return;
    const glm::vec2 pos(static_cast<float>(x), static_cast<float>(y));
    if (self->firstMouseSample_) {
        // First callback after capturing (or at startup): no delta, because
        // the cursor may have jumped from anywhere to the window/capture
        // position and that would snap the camera around.
        self->firstMouseSample_ = false;
    } else {
        // Accumulate: several motion events can land in one frame, and a
        // per-event overwrite silently drops pixels (jittery, light look).
        self->mouseDelta_ += pos - self->mousePos_;
    }
    self->mousePos_ = pos;
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
    // Re-baseline: the first position sample after a capture change is not a
    // movement, and any pending delta belongs to the free-cursor era.
    firstMouseSample_ = true;
    mouseDelta_ = glm::vec2(0.0f);
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
    case Action::Quit: return "Quit";
    default: return "Unknown Action";
    }
}

} // namespace horror
