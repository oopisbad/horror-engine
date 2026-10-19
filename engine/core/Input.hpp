#pragma once

#include "core/Log.hpp"

#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace horror {

class Window;

// Semantic actions decouple gameplay from hardware keys. Remapping = editing
// defaultBindings or loading a config later -- no gameplay code changes.
enum class Action : uint8_t {
    MoveForward,
    MoveBackward,
    MoveLeft,
    MoveRight,
    Sprint,
    Crouch,
    Jump,
    Interact,
    ToggleDebugUI, // toggles the ImGui overlay
    ToggleWireframe,
    ToggleDebugDraw,
    ToggleVsync,
    Quit,
    Count
};

// FPS-style input with an action mapping layer.
//
// State model per key/button: [prevFrame, thisFrame], updated once per frame
// by update(); code elsewhere only reads. Guarantees stable edge semantics.
class Input {
public:
    Input();
    void attach(Window &window);
    void detach();

    // Call once per frame AFTER glfwPollEvents, BEFORE gameplay reads state.
    void update();

    // --- action queries -------------------------------------------------------
    bool held(Action a) const;      // down this frame
    bool pressed(Action a) const;   // this frame edge (was up last frame)
    bool released(Action a) const;  // up this frame edge

    // Raw key access for debugging/tools, not gameplay.
    bool keyHeld(int key) const;
    bool keyPressed(int key) const;

    // --- mouse ----------------------------------------------------------------
    void setMouseCaptured(bool captured);
    bool mouseCaptured() const { return mouseCaptured_; }
    glm::vec2 mouseDelta() const { return mouseDelta_; }    // pixels/frame
    glm::vec2 mousePos() const { return mousePos_; }
    glm::vec2 scrollDelta() const { return scrollDelta_; }

    // Only true when ImGui wants the mouse (so clicks reach the overlay
    // instead of reaching gameplay handlers).
    bool mouseWantedByImGui = false;
    bool keyboardWantedByImGui = false;

    static const char *actionName(Action a);

private:
    static void keyCallback(GLFWwindow *w, int key, int scancode, int action, int mods);
    static void mouseButtonCallback(GLFWwindow *w, int button, int action, int mods);
    static void cursorPosCallback(GLFWwindow *w, double x, double y);
    static void scrollCallback(GLFWwindow *w, double dx, double dy);

    struct KeyState {
        bool wasDown : 1; // previous frame
        bool isDown : 1;  // this frame
    };
    std::array<KeyState, GLFW_KEY_LAST + 1> keys_{};
    std::array<KeyState, GLFW_MOUSE_BUTTON_LAST + 1> buttons_{};

    std::array<std::vector<int>, size_t(Action::Count)> bindings_;

    Window *window_ = nullptr;
    GLFWwindow *glfwWindow_ = nullptr;

    // GLFW's single per-window user pointer is already claimed by Window (its
    // framebuffer callback). Input is one-per-process like Window, so callbacks
    // resolve their instance through this instead of clobbering that pointer.
    inline static Input *active_ = nullptr;

    bool mouseCaptured_ = false;
    glm::vec2 mousePos_{};           // logical window coords
    glm::vec2 mouseDelta_{};         // raw cursor movement this frame
    glm::vec2 scrollDelta_{};
    bool firstMouseSample_ = true;
};

} // namespace horror
