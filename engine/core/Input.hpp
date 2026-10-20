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
    DumpRecorder, // F9: write the flight recorder ring buffer to ./debug_dump.txt
    TogglePatrol, // M: walk the sample models along their in-place clips
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

    // Call once per frame BEFORE glfwPollEvents, so the edge snapshot is taken
    // from the end of the previous frame and pressed()/released() compare a
    // key against the previous frame instead of against itself.
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
    // Window focus, sampled every frame from GLFW (and by the focus callback).
    // Losing/regaining focus with a captured cursor used to hand the camera a
    // huge "first" delta; Input re-baselines the cursor on every transition.
    bool windowFocused() const { return focused_; }
    // Cursor samples thrown away as warps/re-baselines since start (diagnostic).
    int resyncEvents() const { return resyncEvents_; }
    // Pixels accumulated since the last consumeMouseDelta(). Negative y is up.
    glm::vec2 mouseDelta() const { return mouseDelta_; }
    // Hands the accumulated motion to the caller and clears it. Mouselook
    // consumes once per fixed step so the look speed is independent of how
    // many steps a frame runs (and no pixel is ever applied twice).
    glm::vec2 consumeMouseDelta();
    glm::vec2 mousePos() const { return mousePos_; }
    glm::vec2 scrollDelta() const { return scrollDelta_; }

    // --- scripting / tests -----------------------------------------------------
    // Drives key state without a window or real events (--script, --selftest).
    // Mirrors what the GLFW callback does; call after update().
    void setScriptedKey(int key, bool down);
    void injectMouseDelta(const glm::vec2 &pixels);
    // Feeds an absolute cursor position through the *real* cursor path
    // (re-baseline swallowing, warp rejection, capture gating). Used by the
    // self-test to prove that a warp cannot reach the camera.
    void simulateCursorMove(const glm::vec2 &position);
    // Cursor samples rejected as warps since start (diagnostic).
    int warpEvents() const { return warpEvents_; }

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
    static void focusCallback(GLFWwindow *w, int focused);

    // Drops the pending delta and re-reads the cursor position over the next
    // few samples. Called on every capture/focus transition, because the
    // absolute cursor position is meaningless right after either one (the
    // window manager warps the pointer when the cursor mode changes).
    void resyncMouse(const char *why);
    // Shared by the GLFW callback and the test hook.
    void handleCursorPos(const glm::vec2 &pos);

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
    glm::vec2 mouseDelta_{};         // raw cursor movement, un-consumed
    glm::vec2 scrollDelta_{};
    bool firstMouseSample_ = true;
    bool rawMouseMotion_ = false;
    bool focused_ = true;
    // Samples to swallow after a re-baseline: a warp is usually followed by
    // one or two more position events in the same or the next poll, and only
    // one of them being swallowed is what produced a multi-hundred-pixel
    // delta ("camera snaps to a fixed orientation").
    int resyncSamples_ = 0;
    int resyncEvents_ = 0;
    int warpEvents_ = 0;
};

} // namespace horror
