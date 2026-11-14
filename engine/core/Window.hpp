#pragma once

#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace horror {

// Owns the GLFW window + GL context. Exactly one instance per process.
//
// The GL function pointers are loaded once via glad here (after
// makeContextCurrent) so every other engine system can call GL freely.
class Window {
public:
    struct Desc {
        std::string_view title = "horror";
        uint32_t width = 800;
        uint32_t height = 600;
        // GL 4.5 core profile; requested explicitly and asserted after creation
        // so failure is loud instead of negotiated silently by the driver.
        int glMajor = 4;
        int glMinor = 5;
        bool vsync = true;
        bool fullscreen = false; // true = windowed fullscreen on primary monitor
        bool visible = true;
    };

    explicit Window(Desc desc);
    ~Window();
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;
    Window(Window &&) = delete;

    void makeCurrent() const;
    void swapBuffers() const;
    void pollEvents() const;

    void setVsync(bool enabled);
    bool vsync() const { return vsync_; }

    bool shouldClose() const;

    // Physical pixel dimensions (what glViewport expects).
    uint32_t frameWidth() const;
    uint32_t frameHeight() const;
    // Logical window coordinates (for ImGui / cursor math).
    uint32_t width() const;
    uint32_t height() const;

    double aspect() const;

    GLFWwindow *handle() const { return window_.get(); }

    // Fired when the *framebuffer* size changes; the renderer resizes its
    // targets through this so nothing else has to check every frame.
    std::function<void(uint32_t w, uint32_t h)> onFramebufferResized;

    // True on frames where a resize happened; lets the game adjust camera
    // aspect without a "was resized" bool of its own.
    bool resizedSinceLastFrame = false;

private:
    void initGLFW(Desc &desc);
    static void glfwErrorCallback(int error, const char *message);

    struct WindowDeleter {
        void operator()(GLFWwindow *w) const;
    };
    std::unique_ptr<GLFWwindow, WindowDeleter> window_;

    uint32_t width_ = 0, height_ = 0;
    bool vsync_ = true;
};

} // namespace horror
